/* SPDX-License-Identifier: BSD-3-Clause */
#include <ntddk.h>
/* Instantiate the bus interface GUID required by VirtIO/WDF in this TU only. */
#include <initguid.h>
#include <wdmguid.h>
#include <stdarg.h>

/* Supply VirtIO library diagnostics without enabling repeated per-frame traces. */
static void VvDebug(const char *format, ...)
{
    va_list args;
    va_start(args,format);
    vDbgPrintEx(DPFLTR_IHVVIDEO_ID,DPFLTR_ERROR_LEVEL,format,args);
    va_end(args);
}

typedef void (*VV_DEBUG_PRINT)(const char *, ...);
VV_DEBUG_PRINT VirtioDebugPrintProc=VvDebug;
int virtioDebugLevel=0;
int bDebugPrint=1;
