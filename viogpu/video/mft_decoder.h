/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef VIOGPU_MFT_DECODER_H
#define VIOGPU_MFT_DECODER_H
#include <windows.h>
#include <mftransform.h>
// Application-local synchronous transform. No global registration or software fallback.
// Call MFStartup before creating it. The device index refers to a decoder media function.
#ifdef VIOGPU_MFT_BUILD
#define VIOGPU_MFT_API __declspec(dllexport)
#else
#define VIOGPU_MFT_API
#endif
extern "C" VIOGPU_MFT_API HRESULT WINAPI VioGpuCreateVideoDecoder(UINT deviceIndex, IMFTransform **transform);
#endif
