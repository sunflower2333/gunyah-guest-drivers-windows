// SPDX-License-Identifier: MIT
// Architecture-specific adapter for the genuine Mesa ICD. No rendering code.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
extern "C" {
#include <gldrv.h>
}
#include <vulkan/vulkan_core.h>
#include <cstring>
#include <cwchar>

static INIT_ONCE modules_once = INIT_ONCE_STATIC_INIT;
static INIT_ONCE gallium_once = INIT_ONCE_STATIC_INIT;
static HMODULE turnip, gallium;

static HMODULE load_sibling(const wchar_t *name)
{
   HMODULE self = nullptr;
   wchar_t path[32768];
   if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&modules_once), &self)) return nullptr;
   DWORD length = GetModuleFileNameW(self, path, _countof(path));
   if (!length || length >= _countof(path)) return nullptr;
   wchar_t *slash = wcsrchr(path, L'\\');
   if (!slash) return nullptr;
   slash[1] = 0;
#if defined(_M_ARM64)
   const wchar_t *arch = L"arm64\\";
#elif defined(_M_X64)
   const wchar_t *arch = L"x64\\";
#elif defined(_M_IX86)
   const wchar_t *arch = L"x86\\";
#else
#error Unsupported ICD architecture
#endif
   if (wcscat_s(path, arch) || wcscat_s(path, name)) return nullptr;
   HMODULE result = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
   if (!result) return nullptr;
   wchar_t actual[32768];
   length = GetModuleFileNameW(result, actual, _countof(actual));
   if (!length || length >= _countof(actual) || _wcsicmp(path, actual)) {
      FreeLibrary(result);
      SetLastError(ERROR_INVALID_DLL);
      return nullptr;
   }
   // Keep modules loaded for the lifetime of returned GL/Vulkan function pointers.
   return result;
}

static BOOL CALLBACK initialize_modules(PINIT_ONCE, PVOID, PVOID *)
{
   if (!load_sibling(L"z-1.dll")) return FALSE;
   if (!load_sibling(L"vulkan-1.dll")) return FALSE;
   turnip = load_sibling(L"vulkan_freedreno.dll");
   return turnip != nullptr;
}

static BOOL CALLBACK initialize_gallium(PINIT_ONCE, PVOID, PVOID *)
{
   if (!InitOnceExecuteOnce(&modules_once, initialize_modules, nullptr, nullptr)) return FALSE;
   gallium = load_sibling(L"libgallium_wgl.dll");
   return gallium != nullptr;
}

template<typename T> static T gl_entry(const char *name)
{
   if (!InitOnceExecuteOnce(&gallium_once, initialize_gallium, nullptr, nullptr)) return nullptr;
   FARPROC symbol = GetProcAddress(gallium, name);
   T result;
   static_assert(sizeof(result) == sizeof(symbol), "function pointer ABI");
   std::memcpy(&result, &symbol, sizeof(result));
   return result;
}

// Types come directly from the pinned Mesa gldrv.h, including ULONG DHGLRC
// (which is deliberately not pointer-sized) and the SetProcTable callback.
#define GL_FORWARD(result, name, args, call, failure) \
   extern "C" result APIENTRY name args { \
      auto entry = gl_entry<decltype(&name)>(#name); \
      return entry ? entry call : failure; \
   }
GL_FORWARD(BOOL, DrvCopyContext, (DHGLRC a, DHGLRC b, UINT c), (a,b,c), FALSE)
GL_FORWARD(DHGLRC, DrvCreateContext, (HDC a), (a), 0)
GL_FORWARD(DHGLRC, DrvCreateLayerContext, (HDC a, int b), (a,b), 0)
GL_FORWARD(BOOL, DrvDeleteContext, (DHGLRC a), (a), FALSE)
GL_FORWARD(BOOL, DrvDescribeLayerPlane, (HDC a, INT b, INT c, UINT d, LPLAYERPLANEDESCRIPTOR e), (a,b,c,d,e), FALSE)
GL_FORWARD(LONG, DrvDescribePixelFormat, (HDC a, INT b, ULONG c, PIXELFORMATDESCRIPTOR *d), (a,b,c,d), 0)
GL_FORWARD(INT, DrvGetLayerPaletteEntries, (HDC a, INT b, INT c, INT d, COLORREF *e), (a,b,c,d,e), 0)
GL_FORWARD(PROC, DrvGetProcAddress, (LPCSTR a), (a), nullptr)
GL_FORWARD(BOOL, DrvPresentBuffers, (HDC a, LPPRESENTBUFFERS b), (a,b), FALSE)
GL_FORWARD(BOOL, DrvRealizeLayerPalette, (HDC a, INT b, BOOL c), (a,b,c), FALSE)
GL_FORWARD(BOOL, DrvReleaseContext, (DHGLRC a), (a), FALSE)
extern "C" VOID APIENTRY DrvSetCallbackProcs(INT a, PROC *b)
{
   auto entry = gl_entry<decltype(&DrvSetCallbackProcs)>("DrvSetCallbackProcs");
   if (entry) entry(a, b);
}
GL_FORWARD(PGLCLTPROCTABLE, DrvSetContext, (HDC a, DHGLRC b, PFN_SETPROCTABLE c), (a,b,c), nullptr)
GL_FORWARD(INT, DrvSetLayerPaletteEntries, (HDC a, INT b, INT c, INT d, const COLORREF *e), (a,b,c,d,e), 0)
GL_FORWARD(BOOL, DrvSetPixelFormat, (HDC a, LONG b), (a,b), FALSE)
GL_FORWARD(BOOL, DrvShareLists, (DHGLRC a, DHGLRC b), (a,b), FALSE)
GL_FORWARD(BOOL, DrvSwapBuffers, (HDC a), (a), FALSE)
GL_FORWARD(BOOL, DrvSwapLayerBuffers, (HDC a, UINT b), (a,b), FALSE)
GL_FORWARD(BOOL, DrvValidateVersion, (ULONG a), (a), FALSE)

template<typename T> static T vk_entry(const char *name)
{
   if (!InitOnceExecuteOnce(&modules_once, initialize_modules, nullptr, nullptr)) return nullptr;
   FARPROC symbol = GetProcAddress(turnip, name);
   T result;
   static_assert(sizeof(result) == sizeof(symbol), "function pointer ABI");
   std::memcpy(&result, &symbol, sizeof(result));
   return result;
}
extern "C" VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version)
{
   auto entry = vk_entry<decltype(&vk_icdNegotiateLoaderICDInterfaceVersion)>("vk_icdNegotiateLoaderICDInterfaceVersion");
   return entry ? entry(version) : VK_ERROR_INITIALIZATION_FAILED;
}
extern "C" PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{
   auto entry = vk_entry<decltype(&vk_icdGetInstanceProcAddr)>("vk_icdGetInstanceProcAddr");
   return entry ? entry(instance, name) : nullptr;
}
extern "C" PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *name)
{
   auto entry = vk_entry<decltype(&vk_icdGetPhysicalDeviceProcAddr)>("vk_icdGetPhysicalDeviceProcAddr");
   return entry ? entry(instance, name) : nullptr;
}
