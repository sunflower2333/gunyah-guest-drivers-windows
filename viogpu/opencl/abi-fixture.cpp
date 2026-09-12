// SPDX-License-Identifier: MIT
// CI fixture only, never shipped as a driver runtime or advertised as a GPU.
#include <CL/cl.h>
#include <CL/cl_icd.h>
#include <CL/cl_ext.h>
#include <cstring>

static cl_icd_dispatch dispatch{};
struct Platform { const cl_icd_dispatch* dispatch; };
static Platform platform{&dispatch};

static cl_int CL_API_CALL platform_info(cl_platform_id object, cl_platform_info param,
                                      size_t bytes, void* out, size_t* returned) {
    if (object != reinterpret_cast<cl_platform_id>(&platform)) return CL_INVALID_PLATFORM;
    const char* value = nullptr;
    switch (param) {
    case CL_PLATFORM_NAME: value = "VIOGPU dispatch ABI fixture"; break;
    case CL_PLATFORM_EXTENSIONS: value = "cl_khr_icd"; break;
    case CL_PLATFORM_ICD_SUFFIX_KHR: value = "VIOGPUFIXTURE"; break;
    default: return CL_INVALID_VALUE;
    }
    size_t size = std::strlen(value)+1;
    if (returned) *returned = size;
    if (out) { if (bytes < size) return CL_INVALID_VALUE; std::memcpy(out,value,size); }
    return CL_SUCCESS;
}
extern "C" cl_int CL_API_CALL clIcdGetPlatformIDsKHR(cl_uint count, cl_platform_id* out, cl_uint* returned) {
    if ((out && !count) || (!out && !returned)) return CL_INVALID_VALUE;
    dispatch.clGetPlatformInfo = platform_info;
    if (returned) *returned = 1;
    if (out) *out = reinterpret_cast<cl_platform_id>(&platform);
    return CL_SUCCESS;
}
static void* extension_address(const char* name) {
    return name && !std::strcmp(name,"clIcdGetPlatformIDsKHR") ? reinterpret_cast<void*>(&clIcdGetPlatformIDsKHR) : nullptr;
}
extern "C" void* CL_API_CALL clGetExtensionFunctionAddress(const char* name) {
    return extension_address(name);
}
extern "C" void* CL_API_CALL clGetExtensionFunctionAddressForPlatform(cl_platform_id, const char* name) {
    return extension_address(name);
}
