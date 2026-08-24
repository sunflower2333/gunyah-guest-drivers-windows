#pragma once

/* Keep the Windows user-mode DDI declarations isolated from the kernel
 * miniport headers.  This is an activation-only adapter shim; it does not
 * expose a second rendering ABI. */
#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000
#endif

#include <windef.h>
#include <wingdi.h>

#ifndef _NTSTATUS_DEFINED
typedef _Return_type_success_(return >= 0) LONG NTSTATUS;
#define _NTSTATUS_DEFINED
#endif

#pragma warning(push)
#pragma warning(disable : 4201)
#include <d3d10umddi.h>
#include <d3d11.h>
#pragma warning(pop)
