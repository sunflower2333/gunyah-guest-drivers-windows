/* SPDX-License-Identifier: BSD-3-Clause
 * Real device acceptance: explicit local IMFTransform, exact frame count + SHA256.
 * Never instantiates a software decoder or a system fallback transform.
 */
#include "../../video/mft_decoder.h"
#include <mfapi.h>
#include <mferror.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <fstream>
#include <vector>
#include <string>
#include <iostream>
#include <stdexcept>
#include <cstdint>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr, const char *operation)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(uint32_t(hr)));
    }
}
static UINT32 Number(const char *text)
{
    size_t end = 0;
    const auto value = std::stoull(text, &end);
    if (!end || text[end] || text[0] == '-' || !value || value > UINT32_MAX)
    {
        throw std::runtime_error("invalid positive argument");
    }
    return static_cast<UINT32>(value);
}
struct Hash
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Hash()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        {
            throw std::runtime_error("SHA256 initialization failed");
        }
    }
    ~Hash()
    {
        if (hash)
        {
            BCryptDestroyHash(hash);
        }
        if (algorithm)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    }
    void Add(BYTE *bytes, DWORD count)
    {
        if (BCryptHashData(hash, bytes, count, 0) < 0)
        {
            throw std::runtime_error("SHA256 update failed");
        }
    }
    std::string Finish()
    {
        BYTE digest[32];
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
        {
            throw std::runtime_error("SHA256 finish failed");
        }
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        for (BYTE b : digest)
        {
            result += hex[b >> 4];
            result += hex[b & 15];
        }
        return result;
    }
};
static std::vector<size_t> AccessUnits(const std::vector<BYTE> &input)
{
    std::vector<size_t> result{0};
    bool first = true;
    for (size_t i = 0; i + 4 < input.size(); ++i)
    {
        if (input[i] || input[i + 1])
        {
            continue;
        }
        size_t prefix = input[i + 2] == 1 ? 3 : (input[i + 2] == 0 && input[i + 3] == 1 ? 4 : 0);
        if (prefix && (input[i + prefix] & 31) == 9)
        {
            if (!first)
            {
                result.push_back(i);
            }
            first = false;
            i += prefix;
        }
    }
    if (first)
    {
        throw std::runtime_error("input must contain H.264 Annex-B AUD-delimited access units");
    }
    result.push_back(input.size());
    return result;
}
int main(int argc, char **argv)
{
    try
    {
        if (argc != 9)
        {
            std::cerr << "Usage: mft-probe device-index width height fps input.h264 output.nv12 expected-frames "
                         "expected-sha256\n";
            return 2;
        }
        const UINT32 device = std::string(argv[1]) == "0" ? 0 : Number(argv[1]);
        const UINT32 width = Number(argv[2]), height = Number(argv[3]), fps = Number(argv[4]),
                     expected = Number(argv[7]);
        const std::string expectedHash = argv[8];
        if (expectedHash.size() != 64 || expectedHash.find_first_not_of("0123456789abcdef") != std::string::npos)
        {
            throw std::runtime_error("expected SHA256 must be 64 lowercase hexadecimal characters");
        }
        std::ifstream stream(argv[5], std::ios::binary | std::ios::ate);
        if (!stream || stream.tellg() <= 0 || stream.tellg() > 128 * 1024 * 1024)
        {
            throw std::runtime_error("input missing, empty, or above 128 MiB limit");
        }
        std::vector<BYTE> encoded(static_cast<size_t>(stream.tellg()));
        stream.seekg(0);
        stream.read(reinterpret_cast<char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        if (!stream)
        {
            throw std::runtime_error("input read failed");
        }
        auto offsets = AccessUnits(encoded);
        if (offsets.size() - 1 != expected)
        {
            throw std::runtime_error("AUD access-unit count differs from expected frame count");
        }
        std::ofstream raw(argv[6], std::ios::binary);
        if (!raw)
        {
            throw std::runtime_error("cannot create output");
        }
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");
        Check(MFStartup(MF_VERSION), "MFStartup");
        ComPtr<IMFTransform> decoder;
        Check(VioGpuCreateVideoDecoder(device, &decoder), "VioGpuCreateVideoDecoder");
        ComPtr<IMFMediaType> input, output;
        Check(decoder->GetInputAvailableType(0, 0, &input), "H264 input");
        Check(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height), "input size");
        Check(MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1), "input rate");
        Check(decoder->SetInputType(0, input.Get(), 0), "SetInputType");
        Check(decoder->GetOutputAvailableType(0, 0, &output), "NV12 output");
        Check(decoder->SetOutputType(0, output.Get(), 0), "SetOutputType");
        Check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "begin");
        Check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "start");
        Hash digest;
        UINT32 submitted = 0, frames = 0;
        bool draining = false;
        ULONGLONG progress = GetTickCount64();
        for (;;)
        {
            if (GetTickCount64() - progress > 30000)
            {
                throw std::runtime_error("no decoder progress for 30 seconds");
            }
            if (submitted < expected)
            {
                const size_t bytes = offsets[submitted + 1] - offsets[submitted];
                if (bytes > UINT32_MAX)
                {
                    throw std::runtime_error("AU too large");
                }
                ComPtr<IMFSample> sample;
                ComPtr<IMFMediaBuffer> buffer;
                Check(MFCreateSample(&sample), "sample");
                Check(MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &buffer), "buffer");
                BYTE *data = nullptr;
                Check(buffer->Lock(&data, nullptr, nullptr), "lock input");
                memcpy(data, encoded.data() + offsets[submitted], bytes);
                Check(buffer->Unlock(), "unlock input");
                Check(buffer->SetCurrentLength(static_cast<DWORD>(bytes)), "length");
                Check(sample->AddBuffer(buffer.Get()), "add buffer");
                Check(sample->SetSampleTime(LONGLONG(uint64_t(submitted) * 10000000 / fps)), "PTS");
                HRESULT hr = decoder->ProcessInput(0, sample.Get(), 0);
                if (hr != MF_E_NOTACCEPTING)
                {
                    Check(hr, "ProcessInput");
                    ++submitted;
                    progress = GetTickCount64();
                }
            }
            if (submitted == expected && !draining)
            {
                Check(decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "drain");
                draining = true;
            }
            MFT_OUTPUT_DATA_BUFFER out = {};
            DWORD status = 0;
            HRESULT hr = decoder->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents)
            {
                out.pEvents->Release();
                throw std::runtime_error("unexpected output events");
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                output.Reset();
                Check(decoder->GetOutputAvailableType(0, 0, &output), "new output type");
                UINT32 w = 0, h = 0;
                Check(MFGetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, &w, &h), "output dimensions");
                if (w != width || h != height)
                {
                    throw std::runtime_error("canonical stream unexpectedly changed dimensions");
                }
                Check(decoder->SetOutputType(0, output.Get(), 0), "accept format change");
                continue;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            {
                if (draining)
                {
                    break;
                }
                continue;
            }
            Check(hr, "ProcessOutput");
            ComPtr<IMFSample> sample;
            sample.Attach(out.pSample);
            if (!sample)
            {
                throw std::runtime_error("success without output sample");
            }
            ComPtr<IMFMediaBuffer> buffer;
            Check(sample->ConvertToContiguousBuffer(&buffer), "output buffer");
            BYTE *data = nullptr;
            DWORD bytes = 0;
            Check(buffer->Lock(&data, nullptr, &bytes), "lock output");
            if (bytes != uint64_t(width) * height * 3 / 2)
            {
                throw std::runtime_error("incorrect NV12 sample size");
            }
            digest.Add(data, bytes);
            raw.write(reinterpret_cast<char *>(data), bytes);
            Check(buffer->Unlock(), "unlock output");
            if (!raw)
            {
                throw std::runtime_error("output write failed");
            }
            ++frames;
            progress = GetTickCount64();
        }
        Check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0), "close");
        const std::string actual = digest.Finish();
        std::cout << "submitted=" << submitted << " frames=" << frames << " sha256=" << actual << "\n";
        if (frames != expected || actual != expectedHash)
        {
            throw std::runtime_error("frame-count/reference-hash mismatch");
        }
        decoder.Reset();
        Check(MFShutdown(), "MFShutdown");
        CoUninitialize();
        std::cout << "PASS application-local MFT decoded exact reference frames; verify Android hardware codec "
                     "selection separately\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL " << error.what() << "\n";
        return 1;
    }
}
