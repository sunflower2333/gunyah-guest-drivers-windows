#include <windows.h>
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    auto library = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library) { std::printf("FAIL loader load error=%lu\n", GetLastError()); return 1; }
    using Enumerate = int (WINAPI*)(unsigned, void**, unsigned*);
    auto enumerate = reinterpret_cast<Enumerate>(GetProcAddress(library, "clGetPlatformIDs"));
    if (!enumerate) return 1;
    unsigned count = 0;
    int status = enumerate(0, nullptr, &count);
    // CI does not have an Adreno. Exercise dispatch initialization, not GPU.
    if (status != 0 && status != -1001) return 1;
    std::printf("PASS loader ABI pointer_bits=%zu status=%d platforms=%u; no GPU claim\n", sizeof(void*) * 8, status, count);
    FreeLibrary(library);
    return 0;
}
