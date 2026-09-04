/* viogpud3d interposer: forwards the D3D10/11 user-mode driver entry points to
 * the real Mesa d3d10umd build and records the DDI interface versions the
 * Windows runtime negotiates.  Built freestanding (no CRT) so it can be
 * cross-compiled with clang for aarch64-windows without the WDK. */

typedef unsigned char       u8;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef long                HRESULT;
typedef void               *HANDLE;

#define REAL_UMD  "C:\\DroidVM\\ZinkD3D\\viogpud3d-zink.dll"
#define LOG_PATH  "C:\\DroidVM\\ZinkD3D\\interpose.log"
/* Every D3D application in the guest loads this module, so logging is gated on
 * a marker file a probe run creates.  Without it the module only forwards. */
#define LOG_GATE  "C:\\DroidVM\\ZinkD3D\\interpose.on"

#define SPOOF_IFACE   0x000B0011u
#define REAL_IFACE    0x000B0010u
#define SPOOF_VERSION 0x000B001100090000ULL

__declspec(dllimport) void  *__cdecl LoadLibraryA(const char *);
__declspec(dllimport) void  *__cdecl GetProcAddress(void *, const char *);
__declspec(dllimport) HANDLE __cdecl CreateFileA(const char *, u32, u32, void *, u32, u32, void *);
__declspec(dllimport) int    __cdecl WriteFile(HANDLE, const void *, u32, u32 *, void *);
__declspec(dllimport) int    __cdecl CloseHandle(HANDLE);
__declspec(dllimport) u32    __cdecl SetFilePointer(HANDLE, long, long *, u32);
__declspec(dllimport) u32    __cdecl GetLastError(void);
__declspec(dllimport) u32    __cdecl GetFileAttributesA(const char *);

#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_RW 0x00000003u
#define OPEN_ALWAYS   4u
#define CREATE_ALWAYS 2u
#define FILE_END      2u
#define INVALID_H     ((HANDLE)(long long)-1)

static const char hexd[] = "0123456789ABCDEF";

static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static int logging_enabled(void)
{
    return GetFileAttributesA(LOG_GATE) != 0xFFFFFFFFu;
}

static void log_raw(const char *buf, u32 len)
{
    HANDLE h;
    u32 wrote;
    if (!logging_enabled()) return;
    h = CreateFileA(LOG_PATH, GENERIC_WRITE, FILE_SHARE_RW, 0, OPEN_ALWAYS, 0x80u, 0);
    if (h == INVALID_H) return;
    SetFilePointer(h, 0, 0, FILE_END);
    WriteFile(h, buf, len, &wrote, 0);
    CloseHandle(h);
}

static void log_str(const char *s) { log_raw(s, slen(s)); }

static void log_hex64(const char *label, u64 v)
{
    char out[64];
    u32 i = 0, k;
    while (label[i]) { out[i] = label[i]; i++; }
    out[i++] = '0'; out[i++] = 'x';
    for (k = 16; k-- > 0; ) out[i++] = hexd[(v >> (k * 4)) & 0xF];
    out[i++] = '\n';
    log_raw(out, i);
}

/* Dump n bytes of p as space-separated 32-bit little-endian words with offsets. */
static void log_dump(const char *label, const void *p, u32 n)
{
    char out[512];
    u32 i = 0, off;
    const u8 *b = (const u8 *)p;
    while (label[i]) { out[i] = label[i]; i++; }
    out[i++] = '\n';
    log_raw(out, i);
    if (!p) { log_str("  <null>\n"); return; }
    for (off = 0; off < n; off += 8) {
        u32 j = 0, k;
        char line[96];
        line[j++] = ' '; line[j++] = ' ';
        line[j++] = '+';
        line[j++] = hexd[(off >> 4) & 0xF];
        line[j++] = hexd[off & 0xF];
        line[j++] = ':'; line[j++] = ' ';
        for (k = 0; k < 8 && off + k < n; k++) {
            line[j++] = hexd[(b[off + k] >> 4) & 0xF];
            line[j++] = hexd[b[off + k] & 0xF];
            line[j++] = ' ';
        }
        line[j++] = '\n';
        log_raw(line, j);
    }
}

static void *g_real;
static HRESULT (*g_oa)(void *);
static HRESULT (*g_oa10)(void *);
static HRESULT (*g_oa10_2)(void *);

/* saved originals from the adapter function table */
static HRESULT (*o_calcsize)(void *, void *);
static HRESULT (*o_createdev)(void *, void *);
static HRESULT (*o_closeadapter)(void *);
static HRESULT (*o_getversions)(void *, u32 *, u64 *);
static HRESULT (*o_getcaps)(void *, const void *);

static int load_real(void)
{
    if (g_real) return 1;
    g_real = LoadLibraryA(REAL_UMD);
    if (!g_real) { log_hex64("LoadLibrary(real) FAILED err=", GetLastError()); return 0; }
    g_oa     = (HRESULT (*)(void *))GetProcAddress(g_real, "OpenAdapter");
    g_oa10   = (HRESULT (*)(void *))GetProcAddress(g_real, "OpenAdapter10");
    g_oa10_2 = (HRESULT (*)(void *))GetProcAddress(g_real, "OpenAdapter10_2");
    log_hex64("real base=", (u64)g_real);
    return 1;
}

/* ---- adapter function thunks ---------------------------------------- */

static HRESULT w_calcsize(void *hAdapter, void *pArg)
{
    HRESULT hr;
    log_dump("CalcPrivateDeviceSize arg:", pArg, 32);
    if (pArg && *(u32 *)pArg == SPOOF_IFACE) {
        *(u32 *)pArg = REAL_IFACE;
        log_str("  [translated Interface 0xB0011 -> 0xB0010]\n");
    }
    hr = o_calcsize(hAdapter, pArg);
    log_hex64("  CalcPrivateDeviceSize ret=", (u64)(u32)hr);
    return hr;
}

/* Report which slots of a driver-filled function table are still NULL.  The
 * D3D11 runtime answers DXGI_ERROR_DRIVER_INTERNAL_ERROR when it finds a
 * required entry missing, so the gaps are the interesting part. */
static void log_null_slots(const char *label, void **tbl, u32 slots)
{
    char line[128];
    u32 i, j = 0, nulls = 0;
    if (!tbl) { log_str("  <null table>\n"); return; }
    log_str(label);
    for (i = 0; i < slots; i++) {
        if (tbl[i] == 0) {
            nulls++;
            if (nulls <= 48) {
                line[j++] = ' ';
                line[j++] = hexd[(i >> 8) & 0xF];
                line[j++] = hexd[(i >> 4) & 0xF];
                line[j++] = hexd[i & 0xF];
                if (j > 100) { line[j++] = '\n'; log_raw(line, j); j = 0; }
            }
        }
    }
    if (j) { line[j++] = '\n'; log_raw(line, j); }
    log_hex64("  null slot count=", (u64)nulls);
}

static HRESULT w_createdev(void *hAdapter, void *pCreateData)
{
    HRESULT hr;
    log_dump("CreateDevice pCreateData (Interface at +08):", pCreateData, 160);
    if (pCreateData && *(u32 *)((u8 *)pCreateData + 8) == SPOOF_IFACE) {
        *(u32 *)((u8 *)pCreateData + 8) = REAL_IFACE;
        log_str("  [translated Interface 0xB0011 -> 0xB0010]\n");
    }
    hr = o_createdev(hAdapter, pCreateData);
    log_hex64("  CreateDevice ret=", (u64)(u32)hr);
    if (hr == 0 && pCreateData) {
        void **funcs = *(void ***)((u8 *)pCreateData + 0x18);
        log_hex64("  device funcs table=", (u64)funcs);
        log_null_slots("  device funcs NULL slots:\n", funcs, 400);
    }
    return hr;
}

static HRESULT w_closeadapter(void *hAdapter)
{
    log_str("CloseAdapter\n");
    return o_closeadapter(hAdapter);
}

/* Mesa's d3d10umd advertises the D3D11.1 and WDDM1.3 DDIs but never fills the
 * device function-table entries those versions add: it populates only the D3D10,
 * D3D10.1 and D3D11.0 views of the union.  The runtime picks the highest version
 * offered, finds required slots NULL and fails device creation.  Drop the two
 * versions whose tables are incomplete so the negotiation settles on D3D11.0,
 * which is fully populated. */
#define MAX_KEPT_IFACE 0x000B000BULL

static HRESULT w_getversions(void *hAdapter, u32 *puEntries, u64 *pVersions)
{
    HRESULT hr = o_getversions(hAdapter, puEntries, pVersions);
    log_hex64("GetSupportedVersions ret=", (u64)(u32)hr);
    if (hr == 0 && puEntries) {
        if (!pVersions) {
            log_hex64("  count query, entries=", (u64)*puEntries);
        } else {
            u32 i, kept = 0;
            for (i = 0; i < *puEntries; i++) {
                if ((pVersions[i] >> 32) <= MAX_KEPT_IFACE) {
                    pVersions[kept++] = pVersions[i];
                } else {
                    log_hex64("    dropped version=", pVersions[i]);
                }
            }
            *puEntries = kept;
            for (i = 0; i < kept && i < 16; i++) log_hex64("    version=", pVersions[i]);
        }
    }
    return hr;
}

static HRESULT w_getcaps(void *hAdapter, const void *pData)
{
    HRESULT hr;
    log_dump("GetCaps arg:", pData, 32);
    hr = o_getcaps(hAdapter, pData);
    log_hex64("  GetCaps ret=", (u64)(u32)hr);
    if (pData) {
        /* arg layout observed on this runtime: +00 Type, +10 pData, +18 DataSize */
        void *out = *(void **)((u8 *)pData + 0x10);
        u32 size = *(u32 *)((u8 *)pData + 0x18);
        if (size > 32) size = 32;
        log_dump("  -> written:", out, size);
    }
    return hr;
}

/* pAdapterFuncs pointer lives at offset 32 of D3D10DDIARG_OPENADAPTER:
 *   +00 hAdapter, +08 hRTAdapter, +10 pAdapterCallbacks, +18 Interface/Version,
 *   +20 pAdapterFuncs.  The table order is CalcPrivateDeviceSize, CreateDevice,
 *   CloseAdapter, then (for _2) GetSupportedVersions, GetCaps. */
static void hook_funcs(void *pOpenData, int with_2)
{
    void **tbl = *(void ***)((u8 *)pOpenData + 32);
    if (!tbl) { log_str("  <no adapter funcs table>\n"); return; }
    log_hex64("  funcs table=", (u64)tbl);
    o_calcsize     = (HRESULT (*)(void *, void *))tbl[0];
    o_createdev    = (HRESULT (*)(void *, void *))tbl[1];
    o_closeadapter = (HRESULT (*)(void *))tbl[2];
    tbl[0] = (void *)w_calcsize;
    tbl[1] = (void *)w_createdev;
    tbl[2] = (void *)w_closeadapter;
    if (with_2) {
        o_getversions = (HRESULT (*)(void *, u32 *, u64 *))tbl[3];
        o_getcaps     = (HRESULT (*)(void *, const void *))tbl[4];
        tbl[3] = (void *)w_getversions;
        tbl[4] = (void *)w_getcaps;
    }
}

__declspec(dllexport) HRESULT OpenAdapter10_2(void *pOpenData)
{
    HRESULT hr;
    log_str("=== OpenAdapter10_2 ===\n");
    if (!load_real() || !g_oa10_2) { log_str("  no real entry\n"); return (HRESULT)0x80004005L; }
    log_dump("  in  (Interface at +18):", pOpenData, 64);
    hr = g_oa10_2(pOpenData);
    log_hex64("  ret=", (u64)(u32)hr);
    if (hr == 0) { log_dump("  out:", pOpenData, 64); hook_funcs(pOpenData, 1); }
    return hr;
}

__declspec(dllexport) HRESULT OpenAdapter10(void *pOpenData)
{
    HRESULT hr;
    log_str("=== OpenAdapter10 ===\n");
    if (!load_real() || !g_oa10) { log_str("  no real entry\n"); return (HRESULT)0x80004005L; }
    log_dump("  in  (Interface at +18):", pOpenData, 64);
    hr = g_oa10(pOpenData);
    log_hex64("  ret=", (u64)(u32)hr);
    if (hr == 0) hook_funcs(pOpenData, 0);
    return hr;
}

__declspec(dllexport) HRESULT OpenAdapter(void *pOpenData)
{
    HRESULT hr;
    log_str("=== OpenAdapter (D3D9) ===\n");
    if (!load_real() || !g_oa) { log_str("  no real entry\n"); return (HRESULT)0x80004005L; }
    log_dump("  in:", pOpenData, 64);
    hr = g_oa(pOpenData);
    log_hex64("  ret=", (u64)(u32)hr);
    return hr;
}

int __stdcall DllMain(void *inst, u32 reason, void *reserved)
{
    (void)inst; (void)reserved;
    if (reason == 1) log_str("--- interposer attached ---\n");
    return 1;
}
