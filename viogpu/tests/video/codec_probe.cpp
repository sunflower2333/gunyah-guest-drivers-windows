/* SPDX-License-Identifier: BSD-3-Clause
 * Hardware acceptance client for the explicit VioGPU video UMD API.
 * decode requires an Annex-B H.264 elementary stream with AUD delimiters.
 * Timestamps are synthetic at fps; use a real demuxer to test B-frame PTS/DTS.
 */
#include "../../video/video_api.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
/* Turn a failed HRESULT into an actionable test failure, never a successful fallback. */
void Check(HRESULT hr, const char *op)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(std::string(op) + " failed, HRESULT=" + std::to_string(static_cast<uint32_t>(hr)));
    }
}
/* A Linux codec error is not a successful Windows transport operation. */
void Linux(uint32_t error, const char *op)
{
    if (error)
    {
        throw std::runtime_error(std::string(op) + " failed, Linux errno=" + std::to_string(error));
    }
}
struct Session
{
    HANDLE Handle = nullptr;
    ~Session()
    {
        if (Handle)
        {
            (void)VioGpuVideoClose(Handle);
        }
    }
};
/* Send a fixed-size protocol control using the production user-mode bridge. */
void Control(HANDLE h, uint32_t code, void *data)
{
    VGPU_VIDEO_CONTROL c = {};
    c.Code = code;
    c.PayloadBytes = VmediaControlSize(code);
    memcpy(c.Payload, data, c.PayloadBytes);
    Check(VioGpuVideoControl(h, &c), "CONTROL");
    Linux(c.LinuxErrno, "CONTROL");
    memcpy(data, c.Payload, c.PayloadBytes);
}
/* Set a single-plane MPLANE format and reject silent codec substitution. */
VMEDIA_FORMAT Format(HANDLE h, uint32_t type, uint32_t codec, uint32_t w, uint32_t height)
{
    VMEDIA_FORMAT f = {};
    f.Type = type;
    VmediaWrite32(f.Data, w);
    VmediaWrite32(f.Data + 4, height);
    VmediaWrite32(f.Data + 8, codec);
    VmediaWrite32(f.Data + 12, 1);
    f.Data[180] = 1;
    Control(h, 5, &f);
    if (VmediaRead32(f.Data + 8) != codec)
    {
        throw std::runtime_error("requested codec/format was not accepted on this device");
    }
    return f;
}
/* Allocate a queue; the returned Count is authoritative. */
VGPU_VIDEO_BUFFERS Allocate(HANDLE h, uint32_t type, uint32_t count)
{
    VGPU_VIDEO_BUFFERS a = {};
    a.Type = type;
    a.Count = count;
    Check(VioGpuVideoAllocate(h, &a), "ALLOCATE");
    Linux(a.LinuxErrno, "ALLOCATE");
    return a;
}
/* Start/stop one queue. */
void Stream(HANDLE h, uint32_t type, bool on)
{
    VGPU_VIDEO_STREAM s = {};
    s.Type = type;
    s.On = on ? 1u : 0u;
    Check(VioGpuVideoStream(h, &s), "STREAM");
    Linux(s.LinuxErrno, "STREAM");
}
/* Lend a fresh output destination. */
void Capture(HANDLE h, uint32_t index)
{
    VGPU_VIDEO_BUFFER b = {};
    b.Type = VMEDIA_CAPTURE;
    b.Index = index;
    Check(VioGpuVideoQueue(h, &b, nullptr), "QBUF(CAPTURE)");
}
/* Reconfigure only CAPTURE for decoder source changes; OUTPUT remains streaming. */
VGPU_VIDEO_BUFFERS ConfigureCapture(HANDLE h)
{
    VMEDIA_FORMAT f = {};
    f.Type = VMEDIA_CAPTURE;
    Control(h, 4, &f);
    std::cout << "CAPTURE format=" << VmediaRead32(f.Data + 8) << " width=" << VmediaRead32(f.Data)
              << " height=" << VmediaRead32(f.Data + 4) << "\n";
    VGPU_VIDEO_BUFFERS a = Allocate(h, VMEDIA_CAPTURE, VGPU_VIDEO_MAX_BUFFERS);
    for (uint32_t i = 0; i < a.Count; ++i)
    {
        Capture(h, i);
    }
    Stream(h, VMEDIA_CAPTURE, true);
    return a;
}
/* Find AUD NAL starts without interpreting compressed slice data as frame parameters. */
std::vector<size_t> AudOffsets(const std::vector<uint8_t> &data)
{
    std::vector<size_t> offsets{0};
    unsigned audits = 0;
    for (size_t i = 0; i + 4 < data.size();)
    {
        size_t prefix = 0;
        if (data[i] == 0 && data[i + 1] == 0)
        {
            if (data[i + 2] == 1)
            {
                prefix = 3;
            }
            else if (i + 4 < data.size() && data[i + 2] == 0 && data[i + 3] == 1)
            {
                prefix = 4;
            }
        }
        if (prefix)
        {
            if ((data[i + prefix] & 31) == 9)
            {
                if (audits && i != offsets.back())
                {
                    offsets.push_back(i);
                }
                ++audits;
            }
            i += prefix + 1;
        }
        else
        {
            ++i;
        }
    }
    if (!audits)
    {
        throw std::runtime_error("H.264 input needs AUD NALs; insert them with h264_metadata=aud=insert before "
                                 "testing");
    }
    offsets.push_back(data.size());
    return offsets;
}
} // namespace

/* Exercise real codec queues; no software codec is used by this program. */
int main(int argc, char **argv)
{
    try
    {
        if (argc != 8 || (std::string(argv[1]) != "encode" && std::string(argv[1]) != "decode"))
        {
            std::cerr << "Usage: video-codec-test encode|decode device-index width height fps input output\n"
                      << "encode: tight NV12 -> H.264; decode: H.264 Annex B + AUD -> NV12\n";
            return 2;
        }
        const bool encode = std::string(argv[1]) == "encode";
        auto number = [](const char *s) -> uint32_t {
            std::string text(s);
            size_t used = 0;
            unsigned long long n = std::stoull(text, &used);
            if (text.empty() || text[0] == '-' || used != text.size() || n > UINT32_MAX)
            {
                throw std::runtime_error("invalid numeric argument");
            }
            return static_cast<uint32_t>(n);
        };
        const uint32_t index = number(argv[2]), width = number(argv[3]), height = number(argv[4]),
                       fps = number(argv[5]);
        if (!width || !height || width > 16384 || height > 16384 || (width & 1) || (height & 1) || !fps || fps > 240)
        {
            throw std::runtime_error("unsupported dimensions or frame rate");
        }
        const uint64_t rawBytes = static_cast<uint64_t>(width) * height * 3 / 2;
        if (rawBytes > VGPU_VIDEO_MAX_BUFFER_BYTES)
        {
            throw std::runtime_error("unsupported dimensions or frame rate");
        }
        Session session;
        VGPU_VIDEO_OPEN information = {};
        Check(VioGpuVideoOpen(index, &session.Handle, &information), "OPEN");
        std::cout << "Opened VioGPU video function; generation=" << information.Header.Generation
                  << " device_caps=" << information.Config.DeviceCaps << "\n";
        std::ifstream source(argv[6], std::ios::binary);
        std::ofstream destination(argv[7], std::ios::binary);
        if (!source || !destination)
        {
            throw std::runtime_error("cannot open input/output");
        }
        if (source.peek() == std::ifstream::traits_type::eof())
        {
            throw std::runtime_error("input file is empty");
        }
        std::vector<uint8_t> elementary;
        std::vector<size_t> offsets;
        if (!encode)
        {
            source.seekg(0, std::ios::end);
            auto length = source.tellg();
            source.seekg(0);
            if (length <= 0 || length > 128 * 1024 * 1024)
            {
                throw std::runtime_error("decode probe input must be 1..128 MiB");
            }
            elementary.resize(static_cast<size_t>(length));
            if (!source.read(reinterpret_cast<char *>(elementary.data()), static_cast<std::streamsize>(length)))
            {
                throw std::runtime_error("short input");
            }
            offsets = AudOffsets(elementary);
            uint8_t subscribe[32] = {};
            VmediaWrite32(subscribe, VMEDIA_EVENT_SOURCE_CHANGE);
            Control(session.Handle, 90, subscribe);
        }
        VMEDIA_FORMAT inputFormat = Format(session.Handle,
                                           VMEDIA_OUTPUT,
                                           encode ? VMEDIA_NV12 : VMEDIA_H264,
                                           width,
                                           height);
        if (encode)
        {
            if (VmediaRead32(inputFormat.Data) != width || VmediaRead32(inputFormat.Data + 4) != height ||
                VmediaRead32(inputFormat.Data + 24) != width || VmediaRead32(inputFormat.Data + 20) != rawBytes)
            {
                throw std::runtime_error("probe requires a tight NV12 input layout matching the requested dimensions");
            }
            (void)Format(session.Handle, VMEDIA_CAPTURE, VMEDIA_H264, width, height);
            uint8_t parm[204] = {};
            VmediaWrite32(parm, VMEDIA_OUTPUT);
            VmediaWrite32(parm + 12, 1);
            VmediaWrite32(parm + 16, fps);
            Control(session.Handle, 22, parm);
        }
        const VGPU_VIDEO_BUFFERS inputs = Allocate(session.Handle, VMEDIA_OUTPUT, 8);
        std::vector<bool> busy(inputs.Count, false);
        std::vector<uint8_t> frame(static_cast<size_t>(rawBytes)), output;
        VGPU_VIDEO_BUFFERS captures = {};
        bool captureReady = false, changing = false;
        if (encode)
        {
            captures = ConfigureCapture(session.Handle);
            captureReady = true;
            output.resize(captures.BufferBytes);
        }
        Stream(session.Handle, VMEDIA_OUTPUT, true);
        size_t au = 0;
        uint64_t submitted = 0, received = 0;
        uint32_t inflight = 0;
        bool eof = false, draining = false, last = false;
        ULONGLONG progress = GetTickCount64();
        while (!last || inflight)
        {
            if (GetTickCount64() - progress > 30000)
            {
                throw std::runtime_error("no codec progress for 30 seconds; not treating timeout as EOS");
            }
            for (uint32_t i = 0; i < inputs.Count && !eof && !draining; ++i)
            {
                if (busy[i])
                {
                    continue;
                }
                const void *data = nullptr;
                size_t bytes = 0;
                if (encode)
                {
                    source.read(reinterpret_cast<char *>(frame.data()), static_cast<std::streamsize>(frame.size()));
                    auto got = source.gcount();
                    if (!got)
                    {
                        eof = true;
                        break;
                    }
                    if (static_cast<size_t>(got) != frame.size())
                    {
                        throw std::runtime_error("truncated NV12 frame");
                    }
                    data = frame.data();
                    bytes = frame.size();
                }
                else
                {
                    if (au + 1 >= offsets.size())
                    {
                        eof = true;
                        break;
                    }
                    bytes = offsets[au + 1] - offsets[au];
                    data = elementary.data() + offsets[au];
                    ++au;
                }
                if (bytes > inputs.BufferBytes)
                {
                    throw std::runtime_error("access unit is larger than the negotiated input buffer");
                }
                VGPU_VIDEO_BUFFER b = {};
                b.Type = VMEDIA_OUTPUT;
                b.Index = i;
                b.BytesUsed = static_cast<uint32_t>(bytes);
                b.TimestampUs = static_cast<int64_t>((submitted * 1000000) / fps);
                Check(VioGpuVideoQueue(session.Handle, &b, data), "QBUF(OUTPUT)");
                busy[i] = true;
                ++inflight;
                ++submitted;
                progress = GetTickCount64();
            }
            if (eof && !draining)
            {
                uint8_t command[72] = {};
                VmediaWrite32(command, 1); // DECODER_CMD_STOP / ENCODER_CMD_STOP
                Control(session.Handle, encode ? 77u : 96u, command);
                draining = true;
            }
            VGPU_VIDEO_EVENT e = {};
            e.TimeoutMs = 100;
            HRESULT hr = VioGpuVideoDequeue(session.Handle, &e);
            Check(hr, "DEQUEUE");
            if (hr == S_FALSE)
            {
                continue;
            }
            progress = GetTickCount64();
            if (e.Event == VMEDIA_EVT_ERROR)
            {
                Linux(e.LinuxErrno ? e.LinuxErrno : 5, "codec ERROR event");
            }
            if (e.Event == VMEDIA_EVT_V4L2 && e.V4l2Type == VMEDIA_EVENT_SOURCE_CHANGE)
            {
                if (!captureReady)
                {
                    captures = ConfigureCapture(session.Handle);
                    output.resize(captures.BufferBytes);
                    captureReady = true;
                }
                else
                {
                    changing = true;
                }
            }
            else if (e.Event == VMEDIA_EVT_DQBUF && e.Type == VMEDIA_OUTPUT)
            {
                if (e.Index >= busy.size() || !busy[e.Index] || !inflight)
                {
                    throw std::runtime_error("duplicate input completion");
                }
                busy[e.Index] = false;
                --inflight;
            }
            else if (e.Event == VMEDIA_EVT_DQBUF && e.Type == VMEDIA_CAPTURE)
            {
                if (e.Flags & VMEDIA_FLAG_ERROR)
                {
                    throw std::runtime_error("codec returned a corrupt output buffer");
                }
                if (e.BytesUsed)
                {
                    VGPU_VIDEO_BUFFER b = {};
                    b.Type = VMEDIA_CAPTURE;
                    b.Index = e.Index;
                    Check(VioGpuVideoCopy(session.Handle, &b, output.data(), static_cast<UINT>(output.size())), "COPY");
                    destination.write(reinterpret_cast<const char *>(output.data()), b.BytesUsed);
                    if (!destination)
                    {
                        throw std::runtime_error("output write failed");
                    }
                    ++received;
                }
                if (e.Flags & VMEDIA_FLAG_LAST)
                {
                    if (changing)
                    {
                        Stream(session.Handle, VMEDIA_CAPTURE, false);
                        (void)Allocate(session.Handle, VMEDIA_CAPTURE, 0);
                        captures = ConfigureCapture(session.Handle);
                        output.resize(captures.BufferBytes);
                        changing = false;
                    }
                    else if (draining)
                    {
                        last = true;
                    }
                    else
                    {
                        throw std::runtime_error("unexpected LAST outside drain/source change");
                    }
                }
                else
                {
                    Capture(session.Handle, e.Index);
                }
            }
        }
        HRESULT closed = VioGpuVideoClose(session.Handle);
        session.Handle = nullptr;
        Check(closed, "CLOSE");
        std::cout << "Transport drain completed\nSubmitted units: " << submitted << "\nNonempty outputs: " << received
                  << "\nValidate the output with an independent decoder/frame checksum; this is not a DXVA test.\n";
        return received ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
