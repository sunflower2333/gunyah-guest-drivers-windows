/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef VIOGPU_MFT_DECODER_H
#define VIOGPU_MFT_DECODER_H
#include <windows.h>
#include <mftransform.h>
// Application-local synchronous transform. No global registration or software fallback.
// Call MFStartup before creating it. The device index refers to a decoder media function.
extern "C" HRESULT WINAPI VioGpuCreateVideoDecoder(UINT deviceIndex, IMFTransform **transform);
#endif
