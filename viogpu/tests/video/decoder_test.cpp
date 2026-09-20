/* SPDX-License-Identifier: BSD-3-Clause */
#include "../../video/decoder_session.h"
#include <deque>
#include <cstdio>
#include <cstdlib>
using namespace viogpu_video;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Mock final : Transport
{
    std::deque<VGPU_VIDEO_EVENT> events;
    bool opened = false, encoder = false, substitute = false, brokenCopy = false, closeFail = false;
    uint32_t width = 64, height = 48, stride = 80, minimum = 3, copies = 0, returns = 0, stops = 0, drains = 0;
    uint32_t captureCount = 0, capturedBytes = 0;
    bool Open(uint32_t) override { opened = true; return true; }
    bool Close() override { opened = false; events.clear(); return !closeFail; }
    bool Control(uint32_t code, void *payload) override
    {
        auto *bytes = static_cast<uint8_t *>(payload);
        if (code == 2)
        {
            if (VmediaRead32(bytes)) return false;
            VmediaWrite32(bytes + 44, encoder ? VMEDIA_NV12 : VMEDIA_H264);
        }
        else if (code == 5 && substitute) VmediaWrite32(static_cast<VMEDIA_FORMAT *>(payload)->Data + 8, VMEDIA_HEVC);
        else if (code == 4)
        {
            auto &f = *static_cast<VMEDIA_FORMAT *>(payload);
            VmediaWrite32(f.Data, width); VmediaWrite32(f.Data + 4, height);
            VmediaWrite32(f.Data + 8, VMEDIA_NV12); VmediaWrite32(f.Data + 20, stride * height * 3 / 2);
            VmediaWrite32(f.Data + 24, stride); f.Data[180] = 1;
        }
        else if (code == 27) VmediaWrite32(bytes + 4, minimum);
        else if (code == 96) { CHECK(VmediaRead32(bytes) == 1); ++drains; }
        return true;
    }
    bool Allocate(VGPU_VIDEO_BUFFERS &a) override
    {
        if (a.Type == VMEDIA_CAPTURE)
        {
            captureCount = a.Count;
            capturedBytes = a.BufferBytes = stride * height * 3 / 2;
        }
        else { a.Count = 2; a.BufferBytes = 4096; }
        return true;
    }
    bool Queue(const VGPU_VIDEO_BUFFER &b, const void *) override
    { if (b.Type == VMEDIA_CAPTURE) ++returns; return true; }
    int Event(VGPU_VIDEO_EVENT &e) override
    { if (events.empty()) return 0; e = events.front(); events.pop_front(); return 1; }
    bool Copy(VGPU_VIDEO_BUFFER &b, void *data, uint32_t size) override
    {
        CHECK(size >= capturedBytes); memset(data, 0x5a, capturedBytes);
        b.BytesUsed = capturedBytes; b.TimestampUs = -1234; ++copies;
        return !brokenCopy;
    }
    bool Stream(uint32_t, bool on) override { if (!on) ++stops; return true; }
    void Source()
    { VGPU_VIDEO_EVENT e = {}; e.Event = VMEDIA_EVT_V4L2; e.V4l2Type = VMEDIA_EVENT_SOURCE_CHANGE; events.push_back(e); }
    void Done(uint32_t type, uint32_t index = 0, uint32_t bytes = 0, uint32_t flags = 0)
    {
        VGPU_VIDEO_EVENT e = {}; e.Event = VMEDIA_EVT_DQBUF; e.Type = type; e.Index = index;
        e.BytesUsed = bytes; e.Flags = flags; e.TimestampUs = -1234; events.push_back(e);
    }
};
static void Ready(Mock &mock, DecoderSession &session)
{
    CHECK(session.Open(0, 64, 48) == Result::Ok);
    mock.Source();
    CHECK(session.Poll() == Result::FormatChanged);
    CHECK(mock.captureCount == 4);
    CHECK(session.Format().Stride == 80);
    CHECK(!session.CanInput());
    CHECK(session.AcknowledgeFormat() == Result::Ok);
}
int main()
{
    const uint8_t au[] = {0, 0, 0, 1, 0x65};
    {
        Mock m; DecoderSession d(m); Ready(m, d);
        CHECK(d.Submit(au, sizeof(au), -1234) == Result::Ok);
        CHECK(d.Submit(au, sizeof(au), 0) == Result::Ok);
        CHECK(d.Submit(au, sizeof(au), 0) == Result::NotAccepting);
        CHECK(d.Poll() == Result::NeedInput); // no event is never EOF
        const auto lent = m.returns;
        m.Done(VMEDIA_OUTPUT); m.Done(VMEDIA_CAPTURE, 0, m.capturedBytes);
        CHECK(d.Poll() == Result::Frame);
        CHECK(!d.CanInput());
        CHECK(d.Poll() == Result::Frame);
        CHECK(m.returns == lent && m.copies == 0); // event does not requeue before copy
        uint8_t raw[32768] = {}; int64_t pts = 0;
        CHECK(d.CopyFrame(raw, 1, pts) == Result::Invalid);
        CHECK(m.returns == lent);
        CHECK(d.CopyFrame(raw, sizeof(raw), pts) == Result::Ok && pts == -1234 && raw[0] == 0x5a);
        CHECK(m.returns == lent); // caller still owns returned bytes
        CHECK(d.ReleaseFrame() == Result::Ok && m.returns == lent + 1);
        CHECK(d.CanInput());
        CHECK(d.Drain() == Result::Ok && m.drains == 1 && m.stops == 0);
        CHECK(!d.CanInput());
        VGPU_VIDEO_EVENT eos = {}; eos.Event = VMEDIA_EVT_V4L2; eos.V4l2Type = VMEDIA_EVENT_EOS;
        m.events.push_back(eos);
        CHECK(d.Poll() == Result::NeedInput);
        m.Done(VMEDIA_CAPTURE, 1, 0, VMEDIA_FLAG_LAST);
        CHECK(d.Poll() == Result::NeedInput); // still owes OUTPUT index 1
        m.Done(VMEDIA_OUTPUT, 1);
        (void)d.Poll();
        CHECK(d.Poll() == Result::Drained);
        CHECK(d.Close() == Result::Ok && !m.opened);
        CHECK(d.Open(0, 64, 48) == Result::Ok); // flush uses close/reopen generation
    }
    {
        Mock m; DecoderSession d(m); Ready(m, d);
        m.Source(); m.Done(VMEDIA_CAPTURE, 0, 0, VMEDIA_FLAG_LAST);
        m.width = 96; m.height = 64; m.stride = 112;
        CHECK(d.Poll() == Result::FormatChanged && m.stops == 1);
        CHECK(d.Format().Width == 96 && d.Format().Stride == 112);
        CHECK(!d.CanInput());
        CHECK(d.AcknowledgeFormat() == Result::Ok && d.CanInput());
    }
    {
        Mock m; DecoderSession d(m); Ready(m, d);
        CHECK(d.Submit(au, sizeof(au), 0) == Result::Ok);
        m.Done(VMEDIA_OUTPUT); m.Done(VMEDIA_OUTPUT);
        CHECK(d.Poll() == Result::Failed); // duplicate return cannot free an unrelated new AU
        CHECK(!d.CanInput());
    }
    {
        Mock m; DecoderSession d(m); Ready(m, d);
        m.Done(VMEDIA_CAPTURE, 0, m.capturedBytes - 1);
        CHECK(d.Poll() == Result::Failed); // never expose incomplete NV12
    }
    {
        Mock m; DecoderSession d(m); Ready(m, d);
        m.Done(VMEDIA_CAPTURE, 0, m.capturedBytes, VMEDIA_FLAG_ERROR);
        CHECK(d.Poll() == Result::Failed);
    }
    {
        Mock m; m.encoder = true; DecoderSession d(m);
        CHECK(d.Open(0, 64, 48) == Result::Unsupported && !m.opened);
    }
    {
        Mock m; m.substitute = true; DecoderSession d(m);
        CHECK(d.Open(0, 64, 48) == Result::Failed);
    }
    {
        Mock m; m.minimum = 33; DecoderSession d(m);
        CHECK(d.Open(0, 64, 48) == Result::Ok); m.Source();
        CHECK(d.Poll() == Result::Failed && !m.captureCount);
    }
    {
        Mock m; m.stride = 32; DecoderSession d(m);
        CHECK(d.Open(0, 64, 48) == Result::Ok); m.Source();
        CHECK(d.Poll() == Result::Failed);
    }
    {
        Mock m; DecoderSession d(m); Ready(m, d); m.brokenCopy = true;
        m.Done(VMEDIA_CAPTURE, 0, m.capturedBytes); CHECK(d.Poll() == Result::Frame);
        uint8_t raw[32768]; int64_t pts; const auto lent = m.returns;
        CHECK(d.CopyFrame(raw, sizeof(raw), pts) == Result::Failed && m.returns == lent);
        CHECK(d.ReleaseFrame() == Result::Invalid);
    }
    {
        Mock m; DecoderSession d(m); Ready(m, d); m.closeFail = true;
        CHECK(d.Close() == Result::Failed); // teardown failure cannot be reported successful
    }
    std::printf("PASS %d decoder ownership/format/drain checks\n", checks);
}
