/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 DroidVM contributors
 * Calls the system XInput API. No injection, DLL replacement or private IOCTL.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static volatile LONG stopRequested;
static volatile LONG activeSlot = -1;

// Attempt a zero report during console termination; forced process kill is not covered.
static BOOL WINAPI on_console(DWORD reason)
{
    LONG slot;
    XINPUT_VIBRATION zero = {0, 0};
    if (reason != CTRL_C_EVENT && reason != CTRL_BREAK_EVENT && reason != CTRL_CLOSE_EVENT &&
        reason != CTRL_LOGOFF_EVENT && reason != CTRL_SHUTDOWN_EVENT)
    {
        return FALSE;
    }
    InterlockedExchange(&stopRequested, 1);
    slot = InterlockedCompareExchange(&activeSlot, -1, -1);
    if (slot >= 0)
    {
        (void)XInputSetState((DWORD)slot, &zero);
    }
    return TRUE;
}

// Parse an unsigned decimal argument with explicit bounds and no sign acceptance.
static int parse_value(const char *text, unsigned long limit, DWORD *out)
{
    unsigned long value;
    char *end;
    const char *p;
    if (!text || !*text) return 0;
    for (p = text; *p; ++p)
    {
        if (*p < '0' || *p > '9') return 0;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end || value > limit) return 0;
    *out = (DWORD)value;
    return 1;
}

// Print connection and input state for every system-assigned XInput slot.
static int list_slots(void)
{
    DWORD slot, result;
    int found = 0;
    for (slot = 0; slot < XUSER_MAX_COUNT; ++slot)
    {
        XINPUT_STATE state = {0};
        result = XInputGetState(slot, &state);
        printf("[XINPUT_SLOT]\nslot=%lu\nresult=%lu\n", (unsigned long)slot, (unsigned long)result);
        if (result == ERROR_SUCCESS)
        {
            ++found;
            printf("packet=%lu\nbuttons=0x%04x\nleft_trigger=%u\nright_trigger=%u\n"
                   "left_x=%d\nleft_y=%d\nright_x=%d\nright_y=%d\n",
                   (unsigned long)state.dwPacketNumber, (unsigned int)state.Gamepad.wButtons,
                   (unsigned int)state.Gamepad.bLeftTrigger, (unsigned int)state.Gamepad.bRightTrigger,
                   (int)state.Gamepad.sThumbLX, (int)state.Gamepad.sThumbLY,
                   (int)state.Gamepad.sThumbRX, (int)state.Gamepad.sThumbRY);
        }
    }
    return found ? 0 : 2;
}

// Send one bounded dual-motor state and then an explicit STOP on the selected slot.
int main(int argc, char **argv)
{
    DWORD slot, low, high, duration, result, stopResult;
    ULONGLONG start;
    XINPUT_STATE state = {0};
    XINPUT_VIBRATION vibration, zero = {0, 0};
    if (argc == 1) return list_slots();
    if (argc != 5 || !parse_value(argv[1], XUSER_MAX_COUNT - 1, &slot) ||
        !parse_value(argv[2], 65535, &low) || !parse_value(argv[3], 65535, &high) ||
        !parse_value(argv[4], 10000, &duration))
    {
        fprintf(stderr, "Usage: xinput_probe [slot low high duration_ms]\n"
                        "No arguments: list only. Motor range: 0..65535. Duration: 0..10000 ms.\n");
        return 1;
    }
    if (!duration && (low || high))
    {
        fprintf(stderr, "Nonzero vibration requires a nonzero duration.\n");
        return 1;
    }
    result = XInputGetState(slot, &state);
    if (result != ERROR_SUCCESS)
    {
        fprintf(stderr, "[XINPUT_NOT_CONNECTED]\nslot=%lu\nresult=%lu\n",
                (unsigned long)slot, (unsigned long)result);
        return 2;
    }
    if (!SetConsoleCtrlHandler(on_console, TRUE))
    {
        fprintf(stderr, "[HANDLER_ERROR]\nresult=%lu\n", (unsigned long)GetLastError());
        return 1;
    }
    InterlockedExchange(&activeSlot, (LONG)slot);
    vibration.wLeftMotorSpeed = (WORD)low;
    vibration.wRightMotorSpeed = (WORD)high;
    if (InterlockedCompareExchange(&stopRequested, 0, 0))
    {
        vibration = zero;
    }
    result = XInputSetState(slot, &vibration);
    printf("[XINPUT_SET]\nslot=%lu\nlow=%u\nhigh=%u\nduration_ms=%lu\nresult=%lu\n",
           (unsigned long)slot, (unsigned int)vibration.wLeftMotorSpeed,
           (unsigned int)vibration.wRightMotorSpeed, (unsigned long)duration, (unsigned long)result);
    start = GetTickCount64();
    while (result == ERROR_SUCCESS && GetTickCount64() - start < duration &&
           !InterlockedCompareExchange(&stopRequested, 0, 0))
    {
        Sleep(10);
    }
    stopResult = XInputSetState(slot, &zero);
    InterlockedExchange(&activeSlot, -1);
    (void)SetConsoleCtrlHandler(on_console, FALSE);
    printf("[XINPUT_STOP]\nslot=%lu\nresult=%lu\n", (unsigned long)slot, (unsigned long)stopResult);
    /* API success is not proof that the target is vioinput or the motor moved. */
    return result == ERROR_SUCCESS && stopResult == ERROR_SUCCESS ? 0 : 3;
}
