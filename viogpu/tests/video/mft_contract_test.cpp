/* SPDX-License-Identifier: BSD-3-Clause
 * Execute the actual IMFTransform against a deterministic device API fixture.
 * This proves MF/ownership behavior, not hardware decode correctness.
 */
#include "../../video/mft_decoder.h"
#include "../../video/video_api.h"
#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>
#include <deque>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
static std::deque<VGPU_VIDEO_EVENT> events;
static unsigned opens = 0, closes = 0, requeues = 0, drains = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
extern "C" HRESULT WINAPI VioGpuVideoOpen(UINT, HANDLE *handle, VGPU_VIDEO_OPEN *info)
{ ++opens; *handle = reinterpret_cast<HANDLE>(uintptr_t(1)); *info = {}; return S_OK; }
extern "C" HRESULT WINAPI VioGpuVideoClose(HANDLE)
{ ++closes; events.clear(); return S_OK; }
extern "C" HRESULT WINAPI VioGpuVideoControl(HANDLE, VGPU_VIDEO_CONTROL *c)
{
    if (c->Code == 2) VmediaWrite32(c->Payload + 44, VMEDIA_H264);
    if (c->Code == 27) VmediaWrite32(c->Payload + 4, 2);
    if (c->Code == 4)
    {
        auto *format = reinterpret_cast<VMEDIA_FORMAT *>(c->Payload);
        VmediaWrite32(format->Data, 64); VmediaWrite32(format->Data + 4, 48);
        VmediaWrite32(format->Data + 8, VMEDIA_NV12);
        VmediaWrite32(format->Data + 20, 5760); VmediaWrite32(format->Data + 24, 80);
        format->Data[180] = 1;
    }
    if (c->Code == 96)
    {
        ++drains;
        VGPU_VIDEO_EVENT e = {}; e.Event = VMEDIA_EVT_DQBUF; e.Type = VMEDIA_CAPTURE; e.Flags = VMEDIA_FLAG_LAST;
        events.push_back(e);
    }
    return S_OK;
}
extern "C" HRESULT WINAPI VioGpuVideoAllocate(HANDLE, VGPU_VIDEO_BUFFERS *b)
{ b->BufferBytes = b->Type == VMEDIA_OUTPUT ? 4096 : 5760; return S_OK; }
extern "C" HRESULT WINAPI VioGpuVideoQueue(HANDLE, const VGPU_VIDEO_BUFFER *b, const void *)
{
    if (b->Type == VMEDIA_CAPTURE) { ++requeues; return S_OK; }
    VGPU_VIDEO_EVENT e = {}; e.Event = VMEDIA_EVT_V4L2; e.V4l2Type = VMEDIA_EVENT_SOURCE_CHANGE;
    events.push_back(e);
    e = {}; e.Event = VMEDIA_EVT_DQBUF; e.Type = VMEDIA_OUTPUT; e.Index = b->Index; events.push_back(e);
    e.Type = VMEDIA_CAPTURE; e.Index = 0; e.BytesUsed = 5760; e.TimestampUs = b->TimestampUs; events.push_back(e);
    return S_OK;
}
extern "C" HRESULT WINAPI VioGpuVideoDequeue(HANDLE, VGPU_VIDEO_EVENT *e)
{ if (events.empty()) return S_FALSE; *e = events.front(); events.pop_front(); return S_OK; }
extern "C" HRESULT WINAPI VioGpuVideoCopy(HANDLE, VGPU_VIDEO_BUFFER *b, void *data, UINT size)
{
    CHECK(size >= 5760); memset(data, 0x5a, 5760);
    b->BytesUsed = 5760; b->TimestampUs = -1234; return S_OK;
}
extern "C" HRESULT WINAPI VioGpuVideoStream(HANDLE, VGPU_VIDEO_STREAM *) { return S_OK; }
static ComPtr<IMFSample> Input()
{
    ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buffer;
    CHECK(SUCCEEDED(MFCreateSample(&sample)));
    CHECK(SUCCEEDED(MFCreateMemoryBuffer(5, &buffer)));
    BYTE *bytes = nullptr; CHECK(SUCCEEDED(buffer->Lock(&bytes, nullptr, nullptr)));
    const BYTE au[] = {0, 0, 0, 1, 0x65}; memcpy(bytes, au, 5);
    CHECK(SUCCEEDED(buffer->Unlock())); CHECK(SUCCEEDED(buffer->SetCurrentLength(5)));
    CHECK(SUCCEEDED(sample->AddBuffer(buffer.Get()))); CHECK(SUCCEEDED(sample->SetSampleTime(-12340)));
    return sample;
}
int main()
{
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    CHECK(SUCCEEDED(MFStartup(MF_VERSION)));
    ComPtr<IMFTransform> transform;
    CHECK(VioGpuCreateVideoDecoder(0, nullptr) == E_POINTER);
    CHECK(SUCCEEDED(VioGpuCreateVideoDecoder(0, &transform)));
    ComPtr<IMFAttributes> attributes; CHECK(SUCCEEDED(transform->GetAttributes(&attributes)));
    UINT32 async = 0; CHECK(FAILED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async)));
    CHECK(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 1) == E_NOTIMPL);
    ComPtr<IMFMediaType> input, output;
    CHECK(SUCCEEDED(transform->GetInputAvailableType(0, 0, &input)));
    CHECK(transform->SetInputType(0, input.Get(), MFT_SET_TYPE_TEST_ONLY) == MF_E_INVALIDMEDIATYPE);
    CHECK(SUCCEEDED(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, 64, 48)));
    CHECK(SUCCEEDED(transform->SetInputType(0, input.Get(), 0)));
    CHECK(SUCCEEDED(transform->GetOutputAvailableType(0, 0, &output)));
    CHECK(SUCCEEDED(transform->SetOutputType(0, output.Get(), 0)));
    // Mutating the caller's input type must not mutate the transform's accepted type.
    CHECK(SUCCEEDED(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, 32, 32)));
    ComPtr<IMFMediaType> saved; CHECK(SUCCEEDED(transform->GetInputCurrentType(0, &saved)));
    UINT32 w = 0, h = 0; CHECK(SUCCEEDED(MFGetAttributeSize(saved.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w == 64 && h == 48);
    for (unsigned run = 0; run != 2; ++run)
    {
        auto inputSample = Input(); CHECK(SUCCEEDED(transform->ProcessInput(0, inputSample.Get(), 0)));
        DWORD status = 0; MFT_OUTPUT_DATA_BUFFER out = {};
        CHECK(transform->ProcessOutput(0, 1, &out, &status) == MF_E_TRANSFORM_STREAM_CHANGE);
        CHECK(out.dwStatus == MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE && out.pSample == nullptr);
        output.Reset(); CHECK(SUCCEEDED(transform->GetOutputAvailableType(0, 0, &output)));
        CHECK(SUCCEEDED(transform->SetOutputType(0, output.Get(), 0)));
        const unsigned lent = requeues;
        CHECK(SUCCEEDED(transform->ProcessOutput(0, 1, &out, &status)) && out.pSample);
        CHECK(requeues == lent + 1);
        ComPtr<IMFSample> sample; sample.Attach(out.pSample); out.pSample = nullptr;
        LONGLONG time = 0; CHECK(SUCCEEDED(sample->GetSampleTime(&time)) && time == -12340);
        ComPtr<IMFMediaBuffer> pixels; CHECK(SUCCEEDED(sample->ConvertToContiguousBuffer(&pixels)));
        BYTE *data = nullptr; DWORD bytes = 0; CHECK(SUCCEEDED(pixels->Lock(&data, nullptr, &bytes)) && bytes == 4608);
        for (DWORD i = 0; i < bytes; ++i) CHECK(data[i] == 0x5a);
        CHECK(SUCCEEDED(pixels->Unlock()));
        CHECK(SUCCEEDED(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0)));
        CHECK(transform->ProcessInput(0, inputSample.Get(), 0) == MF_E_NOTACCEPTING);
        CHECK(transform->ProcessOutput(0, 1, &out, &status) == MF_E_TRANSFORM_NEED_MORE_INPUT);
        CHECK(SUCCEEDED(transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0)));
        CHECK(closes == run + 1);
    }
    CHECK(opens == 2 && drains == 2);
    transform.Reset();
    CHECK(SUCCEEDED(MFShutdown())); CoUninitialize();
    std::puts("PASS actual IMFTransform type negotiation, padded NV12 copy, timestamp, drain and flush/reopen");
}
