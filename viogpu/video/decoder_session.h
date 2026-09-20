/* SPDX-License-Identifier: BSD-3-Clause
 * Stateful decoder shared by the Media Foundation frontend and transport tests.
 * Transport errors are terminal until Close; no software decoding fallback exists.
 */
#ifndef VIOGPU_DECODER_SESSION_H
#define VIOGPU_DECODER_SESSION_H
#include "video_ioctl.h"
#include <initializer_list>

namespace viogpu_video
{
enum class Result
{
    Ok,
    NeedInput,
    NotAccepting,
    FormatChanged,
    Frame,
    Drained,
    Invalid,
    Unsupported,
    Failed
};
struct Layout
{
    uint32_t Width = 0, Height = 0, Stride = 0, Bytes = 0;
};
struct Transport
{
    virtual ~Transport() = default;
    virtual bool Open(uint32_t index) = 0;
    virtual bool Close() = 0;
    virtual bool Control(uint32_t code, void *payload) = 0;
    virtual bool Allocate(VGPU_VIDEO_BUFFERS &buffers) = 0;
    virtual bool Queue(const VGPU_VIDEO_BUFFER &buffer, const void *data) = 0;
    // 1 = event, 0 = timeout, -1 = transport error. Timeout never means EOS.
    virtual int Event(VGPU_VIDEO_EVENT &event) = 0;
    virtual bool Copy(VGPU_VIDEO_BUFFER &buffer, void *data, uint32_t capacity) = 0;
    virtual bool Stream(uint32_t type, bool on) = 0;
};

inline bool Nv12Layout(const VMEDIA_FORMAT &format, Layout &layout)
{
    uint32_t bytes = 0;
    Layout value;
    value.Width = VmediaRead32(format.Data);
    value.Height = VmediaRead32(format.Data + 4);
    value.Stride = VmediaRead32(format.Data + 24);
    if (format.Type != VMEDIA_CAPTURE || VmediaRead32(format.Data + 8) != VMEDIA_NV12 || !value.Width ||
        !value.Height || (value.Width & 1) || (value.Height & 1) || value.Stride < value.Width || (value.Stride & 1) ||
        !VmediaFormatSize(&format, VGPU_VIDEO_MAX_BUFFER_BYTES, &bytes))
    {
        return false;
    }
    const uint64_t required = uint64_t(value.Stride) * value.Height * 3 / 2;
    if (required > bytes || required > VGPU_VIDEO_MAX_BUFFER_BYTES)
    {
        return false;
    }
    value.Bytes = static_cast<uint32_t>(required);
    layout = value;
    return true;
}

class DecoderSession
{
    Transport &transport;
    bool opened = false, failed = false, capture = false, changing = false;
    bool formatPending = false, framePending = false, draining = false, last = false;
    bool inputBusy[VGPU_VIDEO_MAX_BUFFERS] = {};
    VGPU_VIDEO_BUFFERS inputs = {}, captures = {};
    VGPU_VIDEO_EVENT frame = {};
    Layout layout;
    Result Fail()
    {
        failed = true;
        return Result::Failed;
    }
    bool CaptureQueue(uint32_t index)
    {
        VGPU_VIDEO_BUFFER buffer = {};
        buffer.Type = VMEDIA_CAPTURE;
        buffer.Index = index;
        return transport.Queue(buffer, nullptr);
    }
    Result ConfigureCapture()
    {
        VMEDIA_FORMAT format = {};
        format.Type = VMEDIA_CAPTURE;
        if (!transport.Control(4, &format) || !Nv12Layout(format, layout))
        {
            return Fail();
        }
        // The backend's minimum is authoritative; do not allocate 32 full frames unconditionally.
        uint32_t minimum[2] = {0x00980927u, 0}; // V4L2_CID_MIN_BUFFERS_FOR_CAPTURE
        if (!transport.Control(27, minimum) || !minimum[1] || minimum[1] > VGPU_VIDEO_MAX_BUFFERS)
        {
            return Fail();
        }
        captures = {};
        captures.Type = VMEDIA_CAPTURE;
        captures.Count = minimum[1] < VGPU_VIDEO_MAX_BUFFERS ? minimum[1] + 1 : minimum[1];
        if (!transport.Allocate(captures) || captures.Count < minimum[1] || captures.Count > VGPU_VIDEO_MAX_BUFFERS ||
            captures.BufferBytes < layout.Bytes)
        {
            return Fail();
        }
        for (uint32_t i = 0; i < captures.Count; ++i)
        {
            if (!CaptureQueue(i))
            {
                return Fail();
            }
        }
        if (!transport.Stream(VMEDIA_CAPTURE, true))
        {
            return Fail();
        }
        capture = true;
        changing = false;
        formatPending = true;
        return Result::FormatChanged;
    }
    Result FinishCapture()
    {
        const bool isLast = (frame.Flags & VMEDIA_FLAG_LAST) != 0;
        const uint32_t index = frame.Index;
        framePending = false;
        frame = {};
        if (!isLast)
        {
            return CaptureQueue(index) ? Result::Ok : Fail();
        }
        if (changing)
        {
            VGPU_VIDEO_BUFFERS release = {};
            release.Type = VMEDIA_CAPTURE;
            if (!transport.Stream(VMEDIA_CAPTURE, false) || !transport.Allocate(release))
            {
                return Fail();
            }
            capture = false;
            return ConfigureCapture();
        }
        if (!draining)
        {
            return Fail();
        }
        last = true;
        return Result::Ok;
    }

  public:
    explicit DecoderSession(Transport &backend) : transport(backend)
    {
    }
    ~DecoderSession()
    {
        if (opened)
        {
            (void)Close();
        }
    }
    DecoderSession(const DecoderSession &) = delete;
    DecoderSession &operator=(const DecoderSession &) = delete;
    const Layout &Format() const
    {
        return layout;
    }
    uint32_t InputBytes() const
    {
        return inputs.BufferBytes;
    }
    bool IsOpen() const
    {
        return opened;
    }
    bool HasFrame() const
    {
        return framePending;
    }
    bool CanInput() const
    {
        if (!opened || failed || draining || formatPending || framePending)
        {
            return false;
        }
        for (uint32_t i = 0; i < inputs.Count; ++i)
        {
            if (!inputBusy[i])
            {
                return true;
            }
        }
        return false;
    }
    Result Open(uint32_t index, uint32_t width, uint32_t height)
    {
        if (opened || !width || !height || width > 16384 || height > 16384 || (width & 1) || (height & 1))
        {
            return Result::Invalid;
        }
        if (!transport.Open(index))
        {
            return Result::Failed;
        }
        opened = true;
        // ENUM_FMT verifies decoder direction; an encoder's OUTPUT is raw NV12.
        bool h264 = false;
        for (uint32_t i = 0; i < 64; ++i)
        {
            uint8_t format[64] = {};
            VmediaWrite32(format, i);
            VmediaWrite32(format + 4, VMEDIA_OUTPUT);
            if (!transport.Control(2, format))
            {
                break;
            }
            if (VmediaRead32(format + 44) == VMEDIA_H264)
            {
                h264 = true;
                break;
            }
        }
        if (!h264)
        {
            (void)Close();
            return Result::Unsupported;
        }
        for (uint32_t event : {VMEDIA_EVENT_SOURCE_CHANGE, VMEDIA_EVENT_EOS})
        {
            uint8_t subscription[32] = {};
            VmediaWrite32(subscription, event);
            if (!transport.Control(90, subscription))
            {
                return Fail();
            }
        }
        VMEDIA_FORMAT format = {};
        format.Type = VMEDIA_OUTPUT;
        VmediaWrite32(format.Data, width);
        VmediaWrite32(format.Data + 4, height);
        VmediaWrite32(format.Data + 8, VMEDIA_H264);
        VmediaWrite32(format.Data + 12, 1); // progressive
        format.Data[180] = 1;
        if (!transport.Control(5, &format) || VmediaRead32(format.Data + 8) != VMEDIA_H264)
        {
            return Fail();
        }
        inputs.Type = VMEDIA_OUTPUT;
        inputs.Count = 8;
        if (!transport.Allocate(inputs) || !inputs.Count || inputs.Count > VGPU_VIDEO_MAX_BUFFERS ||
            !inputs.BufferBytes || inputs.BufferBytes > VGPU_VIDEO_MAX_BUFFER_BYTES ||
            !transport.Stream(VMEDIA_OUTPUT, true))
        {
            return Fail();
        }
        return Result::Ok;
    }
    Result Close()
    {
        bool success = !opened || transport.Close();
        opened = false;
        failed = capture = changing = formatPending = framePending = draining = last = false;
        memset(inputBusy, 0, sizeof(inputBusy));
        inputs = {};
        captures = {};
        frame = {};
        layout = {};
        return success ? Result::Ok : Result::Failed;
    }
    Result AcknowledgeFormat()
    {
        if (!formatPending || failed)
        {
            return Result::Invalid;
        }
        formatPending = false;
        return Result::Ok;
    }
    Result Submit(const void *bytes, uint32_t length, int64_t timestampUs)
    {
        if (failed)
        {
            return Result::Failed;
        }
        if (!bytes || !length || length > inputs.BufferBytes)
        {
            return Result::Invalid;
        }
        if (!CanInput())
        {
            return Result::NotAccepting;
        }
        for (uint32_t i = 0; i < inputs.Count; ++i)
        {
            if (inputBusy[i])
            {
                continue;
            }
            VGPU_VIDEO_BUFFER buffer = {};
            buffer.Type = VMEDIA_OUTPUT;
            buffer.Index = i;
            buffer.BytesUsed = length;
            buffer.TimestampUs = timestampUs;
            if (!transport.Queue(buffer, bytes))
            {
                return Fail();
            }
            inputBusy[i] = true;
            return Result::Ok;
        }
        return Result::NotAccepting;
    }
    Result Drain()
    {
        if (!opened || failed)
        {
            return Result::Failed;
        }
        if (draining)
        {
            return Result::Ok;
        }
        uint8_t command[72] = {};
        VmediaWrite32(command, 1); // DECODER_CMD_STOP: drain, not STREAMOFF.
        if (!transport.Control(96, command))
        {
            return Fail();
        }
        draining = true;
        return Result::Ok;
    }
    Result Poll(uint32_t timeoutMs = 0)
    {
        if (!opened || failed || timeoutMs > 1000)
        {
            return Result::Failed;
        }
        if (formatPending)
        {
            return Result::FormatChanged;
        }
        if (framePending)
        {
            return Result::Frame;
        }
        bool busy = false;
        for (uint32_t i = 0; i < inputs.Count; ++i)
        {
            busy |= inputBusy[i];
        }
        if (last && !busy)
        {
            return Result::Drained;
        }
        // Bound event processing even if a malfunctioning host keeps producing events.
        for (uint32_t count = 0; count < 128; ++count)
        {
            VGPU_VIDEO_EVENT event = {};
            event.TimeoutMs = count ? 0 : timeoutMs;
            const int received = transport.Event(event);
            if (received < 0)
            {
                return Fail();
            }
            if (!received)
            {
                return Result::NeedInput;
            }
            if (event.Event == VMEDIA_EVT_ERROR)
            {
                return Fail();
            }
            if (event.Event == VMEDIA_EVT_V4L2)
            {
                if (event.V4l2Type == VMEDIA_EVENT_SOURCE_CHANGE)
                {
                    if (!capture)
                    {
                        return ConfigureCapture();
                    }
                    changing = true;
                }
                // EOS event is advisory: only LAST plus returned inputs ends drain.
                continue;
            }
            if (event.Event != VMEDIA_EVT_DQBUF)
            {
                return Fail();
            }
            if (event.Type == VMEDIA_OUTPUT)
            {
                if (event.Index >= inputs.Count || !inputBusy[event.Index] || (event.Flags & VMEDIA_FLAG_ERROR))
                {
                    return Fail();
                }
                inputBusy[event.Index] = false;
                continue;
            }
            if (event.Type != VMEDIA_CAPTURE || !capture || event.Index >= captures.Count ||
                event.BytesUsed > captures.BufferBytes || (event.Flags & VMEDIA_FLAG_ERROR) ||
                (event.BytesUsed && event.BytesUsed < layout.Bytes))
            {
                return Fail();
            }
            frame = event;
            framePending = true;
            if (event.BytesUsed)
            {
                return Result::Frame;
            }
            Result result = FinishCapture();
            if (result != Result::Ok)
            {
                return result;
            }
        }
        return Result::NeedInput;
    }
    Result CopyFrame(void *data, uint32_t capacity, int64_t &timestampUs)
    {
        if (failed)
        {
            return Result::Failed;
        }
        if (!framePending || !data || capacity < captures.BufferBytes)
        {
            return Result::Invalid;
        }
        VGPU_VIDEO_BUFFER buffer = {};
        buffer.Type = VMEDIA_CAPTURE;
        buffer.Index = frame.Index;
        if (!transport.Copy(buffer, data, capacity) || buffer.BytesUsed != frame.BytesUsed ||
            buffer.TimestampUs != frame.TimestampUs || buffer.Flags != frame.Flags)
        {
            return Fail();
        }
        timestampUs = buffer.TimestampUs;
        // A failed destination copy must not return the frame to the device.
        return Result::Ok;
    }
    Result ReleaseFrame()
    {
        return framePending && !failed ? FinishCapture() : Result::Invalid;
    }
    uint32_t CaptureCapacity() const
    {
        return captures.BufferBytes;
    }
};
} // namespace viogpu_video
#endif
