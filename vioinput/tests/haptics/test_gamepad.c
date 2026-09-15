/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "GamepadCore.h"

// Parse HID short items independently: only report 1 input and report 2 output are allowed.
static void descriptor_contract(void)
{
    size_t cursor = 0;
    unsigned int id = 0, bits = 0, count = 0, input[3] = {0}, output[3] = {0};
    int collections = 0;
    while (cursor < sizeof(DvhGamepadDescriptor))
    {
        unsigned int prefix = DvhGamepadDescriptor[cursor++];
        unsigned int n = prefix & 3u, value = 0, i;
        n = n == 3 ? 4 : n;
        assert(prefix != 0xfe && cursor + n <= sizeof(DvhGamepadDescriptor));
        for (i = 0; i < n; ++i)
        {
            value |= (unsigned int)DvhGamepadDescriptor[cursor++] << (i * 8);
        }
        switch (prefix & 0xfcu)
        {
            case 0x84: id = value; assert(id == 1 || id == 2); break;
            case 0x74: bits = value; break;
            case 0x94: count = value; break;
            case 0x80: assert(id == 1); input[id] += bits * count; break;
            case 0x90: assert(id == 2); output[id] += bits * count; break;
            case 0xb0: assert(!"unexpected feature report"); break;
            case 0xa0: ++collections; break;
            case 0xc0: assert(collections > 0); --collections; break;
            default: break;
        }
    }
    assert(!collections && input[1] == 128 && output[2] == 64);
    assert(input[1] / 8 + 1 == DVH_PAD_INPUT_BYTES);
    assert(output[2] / 8 + 1 == DVH_PAD_OUTPUT_BYTES);
}

// Check every mapped button, both trigger ranges, stick saturation and eight hat directions.
static void input_contract(void)
{
    static const unsigned int codes[] = {0x130,0x131,0x133,0x134,0x136,0x137,0x13a,0x13b,0x13c,0x13d,0x13e};
    static const unsigned int bits[] = {0,1,3,4,6,7,10,11,12,13,14};
    static const int hats[9][2] = {{0,0},{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1}};
    DvhPad pad = {0};
    unsigned int i;
    for (i = 0; i < 6; ++i)
    {
        pad.minimum[i] = INT32_MIN;
        pad.maximum[i] = INT32_MAX;
    }
    DvhPadNeutral(&pad);
    assert(pad.report[0] == 1 && DvhRead16(pad.report + 1) == 32768);
    assert(!DvhRead16(pad.report + 9));
    for (i = 0; i < sizeof(codes)/sizeof(codes[0]); ++i)
    {
        assert(DvhPadEvent(&pad, 1, (uint16_t)codes[i], 1));
        assert(DvhRead16(pad.report + 14) == 1u << bits[i]);
        assert(!DvhPadEvent(&pad, 1, (uint16_t)codes[i], 2));
        assert(DvhPadEvent(&pad, 1, (uint16_t)codes[i], 0));
        assert(!DvhRead16(pad.report + 14));
    }
    assert(DvhPadEvent(&pad, 3, 0, INT32_MIN) && !DvhRead16(pad.report + 1));
    assert(DvhPadEvent(&pad, 3, 0, INT32_MAX) && DvhRead16(pad.report + 1) == 65535);
    assert(DvhPadEvent(&pad, 3, 0, 0) && DvhRead16(pad.report + 1) == 32768);
    assert(DvhPadEvent(&pad, 3, 2, INT32_MAX) && DvhRead16(pad.report + 9) == 1023);
    assert(DvhPadEvent(&pad, 3, 5, INT32_MAX) && DvhRead16(pad.report + 11) == 1023);
    for (i = 0; i < 9; ++i)
    {
        DvhPadEvent(&pad, 3, 0x10, hats[i][0]);
        DvhPadEvent(&pad, 3, 0x11, hats[i][1]);
        assert(pad.report[13] == i);
    }
    DvhPadNeutral(&pad);
    DvhPadEvent(&pad, 1, 0x220, 1);
    DvhPadEvent(&pad, 1, 0x221, 1);
    assert(pad.report[13] == 0);
    assert(DvhPadEvent(&pad, 1, 0xa7, 1) && pad.report[16] == 1);
    assert(!DvhPadEvent(&pad, 1, 0xffff, 1));
    assert(DvhPadScale(-1000, -100, 100, 65535) == 0);
    assert(DvhPadScale(1000, -100, 100, 65535) == 65535);
    DvhPadNeutral(&pad);
    assert(!pad.report[13] && !pad.report[16] && pad.minimum[0] == INT32_MIN);
}

// Preserve the complete standard rumble report and keep transport leases separate from duration.
static void output_contract(void)
{
    uint8_t report[10] = {2,15,10,20,30,40,50,60,70,0};
    uint8_t frame[DVH_FRAME_BYTES];
    DvhMotorReport decoded;
    DvhGuestState state;
    DvhMessage message, copy, keepalive;
    size_t n;
    for (n = 0; n < 10; ++n)
    {
        assert(DvhParseMotorReport(report, n, &decoded) == (n == 9));
    }
    assert(DvhParseMotorReport(report, 9, &decoded) && !decoded.stop);
    assert(decoded.magnitudes == UINT32_C(0x281e140a));
    assert(decoded.timing == UINT32_C(0x463c320f));
    DvhGuestReset(&state);
    assert(DvhGuestBegin(&state, 17) && DvhGuestReady(&state, 17));
    assert(!DvhGuestPrepareXinputReport(&state, decoded.magnitudes, decoded.timing, 500, NULL));
    assert(DvhGuestPrepareXinputReport(&state, decoded.magnitudes, decoded.timing, 500, &message));
    assert(DvhEncode(frame, sizeof(frame), DVH_EVENT_TYPE, &message));
    assert(DvhDecode(frame, sizeof(frame), DVH_EVENT_TYPE, &copy));
    assert(copy.motors == decoded.magnitudes && copy.detail == decoded.timing);
    assert(copy.lease_ms == 500 && state.revision == 0); // no speculative commit
    assert(DvhGuestCommit(&state, &message));
    assert(DvhGuestPrepareKeepalive(&state, &keepalive));
    assert(keepalive.opcode == DVH_KEEPALIVE_XINPUT_REPORT && keepalive.detail == message.detail);
    assert(keepalive.revision == message.revision && DvhGuestCommit(&state, &keepalive));
    assert(DvhGuestPrepareOutput(&state, 0, 0, 0, &message));
    assert(DvhGuestCommit(&state, &message) && !DvhGuestCommit(&state, &keepalive));
    assert(!DvhGuestPrepareKeepalive(&state, &keepalive));
    report[0] = 1; assert(!DvhParseMotorReport(report, 9, &decoded)); report[0] = 2;
    report[1] = 16; assert(!DvhParseMotorReport(report, 9, &decoded)); report[1] = 15;
    report[2] = 101; assert(!DvhParseMotorReport(report, 9, &decoded)); report[2] = 10;
    report[1] = 0; assert(DvhParseMotorReport(report, 9, &decoded) && decoded.stop);
    report[1] = 15; report[6] = 0;
    assert(DvhParseMotorReport(report, 9, &decoded) && decoded.stop);
    report[6] = 50; memset(report + 2, 0, 4);
    assert(DvhParseMotorReport(report, 9, &decoded) && decoded.stop);
}

// Entry point for portable input/output and HID descriptor regression.
int main(void)
{
    descriptor_contract();
    input_contract();
    output_contract();
    puts("gamepad: descriptor lengths, controls, scaling, raw rumble timing and stop passed");
    return 0;
}
