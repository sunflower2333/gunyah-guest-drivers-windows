/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 DroidVM contributors
 * XInputHID layout: Microsoft aka.ms/gipdocs, XInputHID Driver Documentation.
 * Do not substitute a generic joystick descriptor or spoof Microsoft's VID.
 */
#ifndef VIOINPUT_GAMEPAD_CORE_H
#define VIOINPUT_GAMEPAD_CORE_H
#include "HapticsCore.h"

#define DVH_CONFIG_SELECT 0x80u
#define DVH_CONFIG_SUBSEL 1u
#define DVH_CONFIG_BYTES 32u
#define DVH_EVENT_TYPE 0xff80u
#define DVH_PROFILE_XINPUT 1u
#define DVH_CAPS_REQUIRED 7u
#define DVH_PAD_INPUT_BYTES 17u
#define DVH_PAD_OUTPUT_BYTES 9u

// A dedicated opt-in profile uses the standard report IDs 1 and 2.
static const uint8_t DvhGamepadDescriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x95,
    0x02, 0x75, 0x10, 0x81, 0x02, 0xc0, 0x09, 0x01, 0xa1, 0x00, 0x09, 0x32,
    0x09, 0x35, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x95, 0x02, 0x75,
    0x10, 0x81, 0x02, 0xc0, 0x05, 0x02, 0x09, 0xc5, 0x15, 0x00, 0x26, 0xff,
    0x03, 0x95, 0x01, 0x75, 0x0a, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00, 0x75,
    0x06, 0x95, 0x01, 0x81, 0x03, 0x05, 0x02, 0x09, 0xc4, 0x15, 0x00, 0x26,
    0xff, 0x03, 0x95, 0x01, 0x75, 0x0a, 0x81, 0x02, 0x15, 0x00, 0x25, 0x00,
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01, 0x09, 0x39, 0x15, 0x01,
    0x25, 0x08, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x66, 0x14, 0x00, 0x75, 0x04,
    0x95, 0x01, 0x81, 0x42, 0x75, 0x04, 0x95, 0x01, 0x15, 0x00, 0x25, 0x00,
    0x35, 0x00, 0x45, 0x00, 0x65, 0x00, 0x81, 0x03, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0f, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0f, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x01, 0x95, 0x01, 0x81, 0x03, 0x05, 0x0c,
    0x0a, 0xb2, 0x00, 0x15, 0x00, 0x25, 0x01, 0x95, 0x01, 0x75, 0x01, 0x81,
    0x02, 0x15, 0x00, 0x25, 0x00, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03, 0x05,
    0x0f, 0x09, 0x21, 0x85, 0x02, 0xa1, 0x02, 0x09, 0x97, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x04, 0x95, 0x01, 0x91, 0x02, 0x15, 0x00, 0x25, 0x00, 0x75,
    0x04, 0x95, 0x01, 0x91, 0x03, 0x09, 0x70, 0x15, 0x00, 0x25, 0x64, 0x75,
    0x08, 0x95, 0x04, 0x91, 0x02, 0x09, 0x50, 0x66, 0x01, 0x10, 0x55, 0x0e,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02, 0x09,
    0xa7, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
    0x65, 0x00, 0x55, 0x00, 0x09, 0x7c, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75,
    0x08, 0x95, 0x01, 0x91, 0x02, 0xc0, 0xc0,
};

typedef struct DvhCaps
{
    uint64_t epoch;
    uint32_t lease_ms;
} DvhCaps;

// Fail closed unless the entire private capability record is understood.
static inline int DvhParseCaps(const uint8_t *p, size_t n, DvhCaps *out)
{
    DvhCaps caps;
    if (!p || !out || n != DVH_CONFIG_BYTES || DvhRead32(p) != UINT32_C(0x31485644) ||
        DvhRead16(p + 4) != DVH_VERSION || DvhRead16(p + 6) != DVH_CONFIG_BYTES ||
        DvhRead32(p + 8) != DVH_CAPS_REQUIRED || DvhRead16(p + 12) != DVH_EVENT_TYPE ||
        DvhRead16(p + 14) != DVH_PROFILE_XINPUT || DvhRead32(p + 28))
    {
        return 0;
    }
    caps.epoch = DvhRead32(p + 16) | ((uint64_t)DvhRead32(p + 20) << 32);
    caps.lease_ms = DvhRead32(p + 24);
    if (!caps.epoch || caps.lease_ms < 100 || caps.lease_ms > DVH_MAX_LEASE_MS)
    {
        return 0;
    }
    *out = caps;
    return 1;
}

typedef struct DvhMotorReport
{
    uint32_t magnitudes; /* left trigger, right trigger, left main, right main */
    uint32_t timing;     /* mask, duration_10ms, delay_10ms, repeat count */
    int stop;
} DvhMotorReport;

// Decode without casts, including explicit report-ID, range and reserved-bit checks.
static inline int DvhParseMotorReport(const uint8_t *p, size_t n, DvhMotorReport *out)
{
    DvhMotorReport r;
    unsigned int i;
    if (!p || !out || n != DVH_PAD_OUTPUT_BYTES || p[0] != 2 || (p[1] & 0xf0u))
    {
        return 0;
    }
    for (i = 2; i <= 5; ++i)
    {
        if (p[i] > 100)
        {
            return 0;
        }
    }
    r.magnitudes = DvhRead32(p + 2);
    r.timing = (uint32_t)p[1] | ((uint32_t)p[6] << 8) | ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 24);
    r.stop = !DvhXinputReportActive(r.magnitudes, r.timing);
    *out = r;
    return 1;
}

typedef struct DvhPad
{
    uint8_t report[DVH_PAD_INPUT_BYTES];
    int32_t minimum[6];
    int32_t maximum[6];
    int8_t hat[2];
    uint8_t dpad;
} DvhPad;

// Normalize signed evdev ranges in 64 bits, including INT32_MIN..INT32_MAX.
static inline uint16_t DvhPadScale(int32_t v, int32_t lo, int32_t hi, uint32_t top)
{
    uint64_t span, offset;
    if (hi <= lo || v <= lo)
    {
        return 0;
    }
    if (v >= hi)
    {
        return (uint16_t)top;
    }
    span = (uint64_t)((int64_t)hi - lo);
    offset = (uint64_t)((int64_t)v - lo);
    return (uint16_t)((offset * top + span / 2) / span);
}

// Release every control while retaining calibration across D0 transitions.
static inline void DvhPadNeutral(DvhPad *s)
{
    unsigned int i;
    for (i = 0; i < DVH_PAD_INPUT_BYTES; ++i)
    {
        s->report[i] = 0;
    }
    s->report[0] = 1;
    for (i = 1; i <= 7; i += 2)
    {
        DvhWrite16(s->report + i, 32768);
    }
    s->hat[0] = s->hat[1] = 0;
    s->dpad = 0;
}

// Encode 1..8 clockwise from up, zero for neutral/opposing directions.
static inline void DvhPadHat(DvhPad *s)
{
    int x = s->hat[0], y = s->hat[1];
    if (s->dpad)
    {
        x = !!(s->dpad & 8) - !!(s->dpad & 4);
        y = !!(s->dpad & 2) - !!(s->dpad & 1);
    }
    s->report[13] = (uint8_t)(y < 0 ? (x < 0 ? 8 : x > 0 ? 2 : 1) :
                             y > 0 ? (x < 0 ? 6 : x > 0 ? 4 : 5) : x < 0 ? 7 : x > 0 ? 3 : 0);
}

// Translate the negotiated Linux gamepad layout; unknown controls never write a report.
static inline int DvhPadEvent(DvhPad *s, uint16_t type, uint16_t code, int32_t value)
{
    static const uint16_t axes[6] = {0, 1, 3, 4, 2, 5};
    /* HID button usage numbers from the 15-button XInputHID layout. */
    static const uint8_t buttons[15] = {1, 2, 0, 4, 5, 0, 7, 8, 0, 0, 11, 12, 13, 14, 15};
    unsigned int i;
    if (type == 3)
    {
        for (i = 0; i < 6; ++i)
        {
            if (axes[i] == code)
            {
                DvhWrite16(s->report + 1 + 2 * i,
                           DvhPadScale(value, s->minimum[i], s->maximum[i], i < 4 ? 65535u : 1023u));
                return 1;
            }
        }
        if (code == 0x10 || code == 0x11)
        {
            s->hat[code - 0x10] = (int8_t)(value < 0 ? -1 : value > 0 ? 1 : 0);
            DvhPadHat(s);
            return 1;
        }
    }
    else if (type == 1 && (value == 0 || value == 1))
    {
        if (code >= 0x130 && code <= 0x13e && buttons[code - 0x130])
        {
            unsigned int bit = buttons[code - 0x130] - 1u;
            uint8_t mask = (uint8_t)(1u << (bit % 8));
            uint8_t *byte = s->report + 14 + bit / 8;
            *byte = value ? (uint8_t)(*byte | mask) : (uint8_t)(*byte & ~mask);
            return 1;
        }
        if (code >= 0x220 && code <= 0x223)
        {
            uint8_t mask = (uint8_t)(1u << (code - 0x220));
            s->dpad = value ? (uint8_t)(s->dpad | mask) : (uint8_t)(s->dpad & ~mask);
            DvhPadHat(s);
            return 1;
        }
        if (code == 0xa7) /* KEY_RECORD -> optional Share */
        {
            s->report[16] = (uint8_t)value;
            return 1;
        }
    }
    return 0;
}
#endif
