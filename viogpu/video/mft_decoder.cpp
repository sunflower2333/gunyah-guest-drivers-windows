/* SPDX-License-Identifier: BSD-3-Clause
 * Application-local H.264 Annex-B -> NV12 Media Foundation transform.
 * CPU copies move encoded/raw bytes; all decoding is performed by the media device.
 */
#define VIOGPU_MFT_BUILD
#include "mft_decoder.h"
#include "video_api.h"
#include <initializer_list>
#include "decoder_session.h"
#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>
#include <mutex>
#include <new>
#include <limits>

using Microsoft::WRL::ComPtr;
using namespace viogpu_video;
namespace
{
class DeviceTransport final : public Transport
{
    HANDLE handle = nullptr;

  public:
    bool Open(uint32_t index) override
    {
        VGPU_VIDEO_OPEN info = {};
        return SUCCEEDED(VioGpuVideoOpen(index, &handle, &info));
    }
    bool Close() override
    {
        HANDLE closing = handle;
        handle = nullptr;
        return !closing || SUCCEEDED(VioGpuVideoClose(closing));
    }
    bool Control(uint32_t code, void *payload) override
    {
        VGPU_VIDEO_CONTROL control = {};
        control.Code = code;
        control.PayloadBytes = VmediaControlSize(code);
        memcpy(control.Payload, payload, control.PayloadBytes);
        if (FAILED(VioGpuVideoControl(handle, &control)) || control.LinuxErrno)
        {
            return false;
        }
        memcpy(payload, control.Payload, control.PayloadBytes);
        return true;
    }
    bool Allocate(VGPU_VIDEO_BUFFERS &buffers) override
    {
        return SUCCEEDED(VioGpuVideoAllocate(handle, &buffers)) && !buffers.LinuxErrno;
    }
    bool Queue(const VGPU_VIDEO_BUFFER &buffer, const void *data) override
    {
        return SUCCEEDED(VioGpuVideoQueue(handle, &buffer, data));
    }
    int Event(VGPU_VIDEO_EVENT &event) override
    {
        HRESULT hr = VioGpuVideoDequeue(handle, &event);
        return hr == S_FALSE ? 0 : (FAILED(hr) ? -1 : 1);
    }
    bool Copy(VGPU_VIDEO_BUFFER &buffer, void *data, uint32_t capacity) override
    {
        return SUCCEEDED(VioGpuVideoCopy(handle, &buffer, data, capacity));
    }
    bool Stream(uint32_t type, bool on) override
    {
        VGPU_VIDEO_STREAM stream = {};
        stream.Type = type;
        stream.On = on ? 1u : 0u;
        return SUCCEEDED(VioGpuVideoStream(handle, &stream)) && !stream.LinuxErrno;
    }
};
HRESULT Status(Result result)
{
    switch (result)
    {
        case Result::Ok:
            return S_OK;
        case Result::NeedInput:
        case Result::Drained:
            return MF_E_TRANSFORM_NEED_MORE_INPUT;
        case Result::NotAccepting:
            return MF_E_NOTACCEPTING;
        case Result::FormatChanged:
            return MF_E_TRANSFORM_STREAM_CHANGE;
        case Result::Unsupported:
            return MF_E_INVALIDMEDIATYPE;
        case Result::Invalid:
            return E_INVALIDARG;
        default:
            return E_FAIL;
    }
}
bool AnnexB(const BYTE *bytes, DWORD length)
{
    return bytes && length >= 4 && bytes[0] == 0 && bytes[1] == 0 &&
           (bytes[2] == 1 || (length >= 5 && bytes[2] == 0 && bytes[3] == 1));
}
HRESULT CloneType(IMFMediaType *source, IMFMediaType **out)
{
    ComPtr<IMFMediaType> copy;
    HRESULT hr = MFCreateMediaType(&copy);
    if (SUCCEEDED(hr))
    {
        hr = source->CopyAllItems(copy.Get());
    }
    if (SUCCEEDED(hr))
    {
        *out = copy.Detach();
    }
    return hr;
}
class Decoder final : public IMFTransform
{
    LONG references = 1;
    std::mutex mutex;
    DeviceTransport transport;
    DecoderSession session{transport};
    UINT index;
    ComPtr<IMFMediaType> input, output;
    UINT32 width = 0, height = 0, rateN = 0, rateD = 0;
    bool sequencePending = true, ended = false;
    Result state = Result::NeedInput;

    HRESULT OutputType(IMFMediaType **out)
    {
        if (!input)
        {
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        }
        const Layout &layout = session.Format();
        const UINT32 w = layout.Width ? layout.Width : width;
        const UINT32 h = layout.Height ? layout.Height : height;
        ComPtr<IMFMediaType> type;
        HRESULT hr = MFCreateMediaType(&type);
        if (SUCCEEDED(hr))
        {
            hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        }
        if (SUCCEEDED(hr))
        {
            hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        }
        if (SUCCEEDED(hr))
        {
            hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h);
        }
        if (SUCCEEDED(hr))
        {
            hr = type->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
        }
        if (SUCCEEDED(hr))
        {
            hr = type->SetUINT32(MF_MT_SAMPLE_SIZE, w * h * 3 / 2);
        }
        if (SUCCEEDED(hr))
        {
            hr = type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        }
        if (SUCCEEDED(hr))
        {
            hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        }
        if (SUCCEEDED(hr))
        {
            hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        }
        if (SUCCEEDED(hr) && rateN && rateD)
        {
            hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, rateN, rateD);
        }
        if (SUCCEEDED(hr))
        {
            *out = type.Detach();
        }
        return hr;
    }
    HRESULT EnsureSession()
    {
        if (session.IsOpen())
        {
            return S_OK;
        }
        Result result = session.Open(index, width, height);
        if (result != Result::Ok)
        {
            (void)session.Close();
            return Status(result);
        }
        sequencePending = true;
        ended = false;
        state = Result::NeedInput;
        return S_OK;
    }
    Result Pump(uint32_t wait = 0)
    {
        if (session.IsOpen())
        {
            state = session.Poll(wait);
        }
        return state;
    }

  public:
    explicit Decoder(UINT deviceIndex) : index(deviceIndex)
    {
    }
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override
    {
        if (!object)
        {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFTransform))
        {
            return E_NOINTERFACE;
        }
        *object = static_cast<IMFTransform *>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&references));
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG count = InterlockedDecrement(&references);
        if (!count)
        {
            delete this;
        }
        return static_cast<ULONG>(count);
    }
    STDMETHODIMP GetStreamLimits(DWORD *a, DWORD *b, DWORD *c, DWORD *d) override
    {
        if (!a || !b || !c || !d)
        {
            return E_POINTER;
        }
        *a = *b = *c = *d = 1;
        return S_OK;
    }
    STDMETHODIMP GetStreamCount(DWORD *a, DWORD *b) override
    {
        if (!a || !b)
        {
            return E_POINTER;
        }
        *a = *b = 1;
        return S_OK;
    }
    STDMETHODIMP GetStreamIDs(DWORD, DWORD *, DWORD, DWORD *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetInputStreamInfo(DWORD id, MFT_INPUT_STREAM_INFO *info) override
    {
        if (id)
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }
        if (!info)
        {
            return E_POINTER;
        }
        std::lock_guard<std::mutex> lock(mutex);
        *info = {};
        info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;
        return S_OK;
    }
    STDMETHODIMP GetOutputStreamInfo(DWORD id, MFT_OUTPUT_STREAM_INFO *info) override
    {
        if (id)
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }
        if (!info)
        {
            return E_POINTER;
        }
        std::lock_guard<std::mutex> lock(mutex);
        *info = {};
        info->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES | MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER |
                        MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE | MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
        if (!output)
        {
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        }
        UINT32 bytes = 0;
        HRESULT hr = output->GetUINT32(MF_MT_SAMPLE_SIZE, &bytes);
        if (SUCCEEDED(hr))
        {
            info->cbSize = bytes;
        }
        return hr;
    }
    STDMETHODIMP GetAttributes(IMFAttributes **attributes) override
    {
        if (!attributes)
        {
            return E_POINTER;
        }
        *attributes = nullptr;
        // No MF_TRANSFORM_ASYNC, D3D-aware, hardware URL, or GPU surface claims.
        return MFCreateAttributes(attributes, 0);
    }
    STDMETHODIMP GetInputStreamAttributes(DWORD id, IMFAttributes **) override
    {
        return id ? MF_E_INVALIDSTREAMNUMBER : E_NOTIMPL;
    }
    STDMETHODIMP GetOutputStreamAttributes(DWORD id, IMFAttributes **) override
    {
        return id ? MF_E_INVALIDSTREAMNUMBER : E_NOTIMPL;
    }
    STDMETHODIMP DeleteInputStream(DWORD) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP AddInputStreams(DWORD, DWORD *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetInputAvailableType(DWORD id, DWORD typeIndex, IMFMediaType **type) override
    {
        if (id)
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }
        if (!type)
        {
            return E_POINTER;
        }
        *type = nullptr;
        if (typeIndex)
        {
            return MF_E_NO_MORE_TYPES;
        }
        ComPtr<IMFMediaType> value;
        HRESULT hr = MFCreateMediaType(&value);
        if (SUCCEEDED(hr))
        {
            hr = value->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        }
        if (SUCCEEDED(hr))
        {
            hr = value->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        }
        if (SUCCEEDED(hr))
        {
            *type = value.Detach();
        }
        return hr;
    }
    STDMETHODIMP GetOutputAvailableType(DWORD id, DWORD typeIndex, IMFMediaType **type) override
    {
        if (id)
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }
        if (!type)
        {
            return E_POINTER;
        }
        *type = nullptr;
        if (typeIndex)
        {
            return MF_E_NO_MORE_TYPES;
        }
        std::lock_guard<std::mutex> lock(mutex);
        return OutputType(type);
    }
    STDMETHODIMP SetInputType(DWORD id, IMFMediaType *type, DWORD flags) override
    {
        if (id)
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }
        if (flags & ~MFT_SET_TYPE_TEST_ONLY)
        {
            return E_INVALIDARG;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (session.IsOpen())
        {
            return MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING;
        }
        if (!type)
        {
            if (!flags)
            {
                input.Reset();
                output.Reset();
            }
            return S_OK;
        }
        GUID major = {}, subtype = {};
        UINT32 w = 0, h = 0;
        if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video ||
            FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_H264 ||
            FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) || !w || !h || w > 8192 || h > 8192 || (w & 1) ||
            (h & 1) || uint64_t(w) * h * 3 / 2 > VGPU_VIDEO_MAX_BUFFER_BYTES)
        {
            return MF_E_INVALIDMEDIATYPE;
        }
        UINT32 interlace = MFVideoInterlace_Progressive;
        (void)type->GetUINT32(MF_MT_INTERLACE_MODE, &interlace);
        if (interlace != MFVideoInterlace_Progressive)
        {
            return MF_E_INVALIDMEDIATYPE;
        }
        if (flags)
        {
            return S_OK;
        }
        ComPtr<IMFMediaType> copy;
        HRESULT hr = CloneType(type, &copy);
        if (FAILED(hr))
        {
            return hr;
        }
        input = copy;
        output.Reset();
        width = w;
        height = h;
        rateN = rateD = 0;
        (void)MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &rateN, &rateD);
        return S_OK;
    }
    STDMETHODIMP SetOutputType(DWORD id, IMFMediaType *type, DWORD flags) override
    {
        if (id)
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }
        if (flags & ~MFT_SET_TYPE_TEST_ONLY)
        {
            return E_INVALIDARG;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (!type)
        {
            if (session.IsOpen())
            {
                return MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING;
            }
            if (!flags)
            {
                output.Reset();
            }
            return S_OK;
        }
        ComPtr<IMFMediaType> expected;
        HRESULT hr = OutputType(&expected);
        if (FAILED(hr))
        {
            return hr;
        }
        GUID major = {}, subtype = {};
        UINT32 w = 0, h = 0, ew = 0, eh = 0;
        (void)MFGetAttributeSize(expected.Get(), MF_MT_FRAME_SIZE, &ew, &eh);
        if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video ||
            FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12 ||
            FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) || w != ew || h != eh)
        {
            return MF_E_INVALIDMEDIATYPE;
        }
        UINT32 stride = w;
        (void)type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
        if (stride != w)
        {
            return MF_E_INVALIDMEDIATYPE;
        }
        if (flags)
        {
            return S_OK;
        }
        output = expected;
        if (state == Result::FormatChanged)
        {
            hr = Status(session.AcknowledgeFormat());
            if (SUCCEEDED(hr))
            {
                state = Result::NeedInput;
            }
        }
        return hr;
    }
    STDMETHODIMP GetInputCurrentType(DWORD id, IMFMediaType **type) override
    {
        if (id)
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }
        if (!type)
        {
            return E_POINTER;
        }
        *type = nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        return input ? CloneType(input.Get(), type) : MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    STDMETHODIMP GetOutputCurrentType(DWORD id, IMFMediaType **type) override
    {
        if (id)
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }
        if (!type)
        {
            return E_POINTER;
        }
        *type = nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        return output ? CloneType(output.Get(), type) : MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    STDMETHODIMP GetInputStatus(DWORD id, DWORD *flags) override
    {
        if (id)
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }
        if (!flags)
        {
            return E_POINTER;
        }
        std::lock_guard<std::mutex> lock(mutex);
        *flags = input && output && !ended && (!session.IsOpen() || session.CanInput()) ? MFT_INPUT_STATUS_ACCEPT_DATA
                                                                                        : 0;
        return S_OK;
    }
    STDMETHODIMP GetOutputStatus(DWORD *flags) override
    {
        if (!flags)
        {
            return E_POINTER;
        }
        std::lock_guard<std::mutex> lock(mutex);
        *flags = session.HasFrame() ? MFT_OUTPUT_STATUS_SAMPLE_READY : 0;
        return S_OK;
    }
    STDMETHODIMP SetOutputBounds(LONGLONG, LONGLONG) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP ProcessEvent(DWORD id, IMFMediaEvent *) override
    {
        return id ? MF_E_INVALIDSTREAMNUMBER : E_NOTIMPL;
    }
    STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE message, ULONG_PTR) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        switch (message)
        {
            case MFT_MESSAGE_COMMAND_FLUSH:
            case MFT_MESSAGE_NOTIFY_END_STREAMING:
                {
                    Result result = session.Close();
                    state = Result::NeedInput;
                    sequencePending = true;
                    ended = false;
                    return Status(result);
                }
            case MFT_MESSAGE_COMMAND_DRAIN:
                ended = true;
                return session.IsOpen() ? Status(session.Drain()) : S_OK;
            case MFT_MESSAGE_NOTIFY_START_OF_STREAM:
                if (ended && session.IsOpen())
                {
                    return MF_E_INVALIDREQUEST; // flush/reopen ends old ownership.
                }
                ended = false;
                return S_OK;
            case MFT_MESSAGE_NOTIFY_END_OF_STREAM:
                return S_OK;
            case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
                return S_OK;
            case MFT_MESSAGE_SET_D3D_MANAGER:
                return E_NOTIMPL;
            default:
                return E_NOTIMPL;
        }
    }
    STDMETHODIMP ProcessInput(DWORD id, IMFSample *sample, DWORD flags) override
    {
        if (id)
        {
            return MF_E_INVALIDSTREAMNUMBER;
        }
        if (!sample)
        {
            return E_POINTER;
        }
        if (flags)
        {
            return E_INVALIDARG;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (!input || !output)
        {
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        }
        if (ended)
        {
            return MF_E_NOTACCEPTING;
        }
        HRESULT hr = EnsureSession();
        if (FAILED(hr))
        {
            return hr;
        }
        Result current = Pump();
        if (current == Result::Failed)
        {
            return E_FAIL;
        }
        if (!session.CanInput())
        {
            return MF_E_NOTACCEPTING;
        }
        LONGLONG timestamp = 0;
        hr = sample->GetSampleTime(&timestamp);
        if (FAILED(hr))
        {
            return hr;
        }
        ComPtr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr))
        {
            return hr;
        }
        BYTE *data = nullptr;
        DWORD bytes = 0;
        hr = buffer->Lock(&data, nullptr, &bytes);
        if (FAILED(hr))
        {
            return hr;
        }
        if (!AnnexB(data, bytes))
        {
            hr = MF_E_INVALID_STREAM_DATA;
        }
        UINT32 headerBytes = 0;
        if (sequencePending)
        {
            (void)input->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &headerBytes);
        }
        BYTE *combined = nullptr;
        if (SUCCEEDED(hr) && (bytes > session.InputBytes() || headerBytes > session.InputBytes() - bytes))
        {
            hr = MF_E_BUFFERTOOSMALL;
        }
        if (SUCCEEDED(hr) && headerBytes)
        {
            combined = static_cast<BYTE *>(HeapAlloc(GetProcessHeap(), 0, size_t(headerBytes) + bytes));
            if (!combined)
            {
                hr = E_OUTOFMEMORY;
            }
            if (SUCCEEDED(hr))
            {
                hr = input->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, combined, headerBytes, nullptr);
            }
            if (SUCCEEDED(hr) && !AnnexB(combined, headerBytes))
            {
                hr = MF_E_INVALIDMEDIATYPE;
            }
            if (SUCCEEDED(hr))
            {
                memcpy(combined + headerBytes, data, bytes);
            }
        }
        if (SUCCEEDED(hr))
        {
            hr = Status(session.Submit(combined ? combined : data, bytes + headerBytes, timestamp / 10));
        }
        if (combined)
        {
            HeapFree(GetProcessHeap(), 0, combined);
        }
        HRESULT unlock = buffer->Unlock();
        if (SUCCEEDED(hr))
        {
            sequencePending = false;
            hr = unlock;
        }
        return hr;
    }
    STDMETHODIMP ProcessOutput(DWORD flags, DWORD count, MFT_OUTPUT_DATA_BUFFER *buffers, DWORD *status) override
    {
        if (!buffers || !status)
        {
            return E_POINTER;
        }
        if (flags || count != 1 || buffers[0].dwStreamID || buffers[0].pSample)
        {
            return E_INVALIDARG;
        }
        std::lock_guard<std::mutex> lock(mutex);
        *status = 0;
        buffers[0].dwStatus = 0;
        buffers[0].pEvents = nullptr;
        if (!input || !output)
        {
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        }
        if (!session.IsOpen())
        {
            return MF_E_TRANSFORM_NEED_MORE_INPUT;
        }
        Result result = Pump(100);
        const ULONGLONG deadline = GetTickCount64() + 15000;
        // During drain NEED_MORE_INPUT promises all prior samples were drained.
        // A backend polling timeout cannot fulfil that promise. Also do not ask
        // for more input while every input buffer is still owned by the device.
        while (result == Result::NeedInput && (ended || !session.CanInput()))
        {
            if (GetTickCount64() >= deadline)
            {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            result = Pump(100);
        }
        if (result == Result::FormatChanged)
        {
            buffers[0].dwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
            return MF_E_TRANSFORM_STREAM_CHANGE;
        }
        if (result != Result::Frame)
        {
            return Status(result);
        }
        const Layout layout = session.Format();
        const DWORD tightBytes = layout.Width * layout.Height * 3 / 2;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr))
        {
            hr = MFCreateMemoryBuffer(tightBytes, &buffer);
        }
        BYTE *scratch = nullptr, *destination = nullptr;
        if (SUCCEEDED(hr))
        {
            scratch = static_cast<BYTE *>(HeapAlloc(GetProcessHeap(), 0, session.CaptureCapacity()));
            if (!scratch)
            {
                hr = E_OUTOFMEMORY;
            }
        }
        int64_t timestamp = 0;
        if (SUCCEEDED(hr))
        {
            hr = Status(session.CopyFrame(scratch, session.CaptureCapacity(), timestamp));
        }
        if (SUCCEEDED(hr) && (timestamp > INT64_MAX / 10 || timestamp < INT64_MIN / 10))
        {
            hr = MF_E_INVALID_TIMESTAMP;
        }
        if (SUCCEEDED(hr))
        {
            hr = buffer->Lock(&destination, nullptr, nullptr);
        }
        if (SUCCEEDED(hr))
        {
            for (UINT32 row = 0; row < layout.Height * 3 / 2; ++row)
            {
                memcpy(destination + size_t(row) * layout.Width, scratch + size_t(row) * layout.Stride, layout.Width);
            }
            hr = buffer->Unlock();
        }
        if (scratch)
        {
            HeapFree(GetProcessHeap(), 0, scratch);
        }
        if (SUCCEEDED(hr))
        {
            hr = buffer->SetCurrentLength(tightBytes);
        }
        if (SUCCEEDED(hr))
        {
            hr = sample->AddBuffer(buffer.Get());
        }
        if (SUCCEEDED(hr))
        {
            hr = sample->SetSampleTime(timestamp * 10);
        }
        if (SUCCEEDED(hr) && rateN && rateD)
        {
            hr = sample->SetSampleDuration(LONGLONG(10000000ull * rateD / rateN));
        }
        if (FAILED(hr))
        {
            return hr; // Retain CAPTURE so allocation failures are retryable.
        }
        result = session.ReleaseFrame();
        if (result == Result::Failed)
        {
            return E_FAIL;
        }
        if (result == Result::FormatChanged)
        {
            state = result;
        }
        buffers[0].pSample = sample.Detach();
        return S_OK;
    }
};
} // namespace
extern "C" __declspec(dllexport) HRESULT WINAPI VioGpuCreateVideoDecoder(UINT deviceIndex, IMFTransform **transform)
{
    if (!transform)
    {
        return E_POINTER;
    }
    *transform = new (std::nothrow) Decoder(deviceIndex);
    return *transform ? S_OK : E_OUTOFMEMORY;
}
