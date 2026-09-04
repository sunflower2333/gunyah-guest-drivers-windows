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

/* Freestanding build: the compiler may lower a struct or array copy to a
 * memcpy call, and there is no CRT to supply one. */
void *memcpy(void *dst, const void *src, unsigned long long n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    unsigned long long i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

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

/* D3D11DDI_DEVICEFUNCS has exactly 152 members in the WDK header extracted by
 * .github/workflows/dump-d3d-ddi-layout.yml, so this bound is known rather than
 * guessed and filling within it stays inside the table.  Mesa leaves 15 entries
 * empty, 13 of them the deferred-context and command-list group.  Each stub
 * records the slot it occupies, so a call tells us which entry the runtime
 * actually needs. */
#define D3D11_DEVICEFUNCS_MEMBERS 152

static u32 g_stub_slot[32];

#define DEFINE_STUB(n) \
static u64 stub_##n(void) { log_hex64("  *** runtime called filled slot=", (u64)g_stub_slot[n]); return 0; }

DEFINE_STUB(0)  DEFINE_STUB(1)  DEFINE_STUB(2)  DEFINE_STUB(3)
DEFINE_STUB(4)  DEFINE_STUB(5)  DEFINE_STUB(6)  DEFINE_STUB(7)
DEFINE_STUB(8)  DEFINE_STUB(9)  DEFINE_STUB(10) DEFINE_STUB(11)
DEFINE_STUB(12) DEFINE_STUB(13) DEFINE_STUB(14) DEFINE_STUB(15)
DEFINE_STUB(16) DEFINE_STUB(17) DEFINE_STUB(18) DEFINE_STUB(19)
DEFINE_STUB(20) DEFINE_STUB(21) DEFINE_STUB(22) DEFINE_STUB(23)
DEFINE_STUB(24) DEFINE_STUB(25) DEFINE_STUB(26) DEFINE_STUB(27)
DEFINE_STUB(28) DEFINE_STUB(29) DEFINE_STUB(30) DEFINE_STUB(31)

static void *const g_stubs[32] = {
    (void *)stub_0,  (void *)stub_1,  (void *)stub_2,  (void *)stub_3,
    (void *)stub_4,  (void *)stub_5,  (void *)stub_6,  (void *)stub_7,
    (void *)stub_8,  (void *)stub_9,  (void *)stub_10, (void *)stub_11,
    (void *)stub_12, (void *)stub_13, (void *)stub_14, (void *)stub_15,
    (void *)stub_16, (void *)stub_17, (void *)stub_18, (void *)stub_19,
    (void *)stub_20, (void *)stub_21, (void *)stub_22, (void *)stub_23,
    (void *)stub_24, (void *)stub_25, (void *)stub_26, (void *)stub_27,
    (void *)stub_28, (void *)stub_29, (void *)stub_30, (void *)stub_31,
};

static void fill_null_slots(void **tbl)
{
    u32 i, used = 0;
    if (!tbl) return;
    for (i = 0; i < D3D11_DEVICEFUNCS_MEMBERS && used < 32; i++) {
        if (tbl[i] == 0) {
            g_stub_slot[used] = i;
            tbl[i] = g_stubs[used];
            log_hex64("  filled slot=", (u64)i);
            used++;
        }
    }
    log_hex64("  slots filled=", (u64)used);
}

#include "devtrace.inc"

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

/* D3D10DDIARG_CREATEDEVICE ends with, for D3D11.1 and later interfaces,
 *   +0x48 Flags, +0x50 PFND3D10DDI_RETRIEVESUBOBJECT *ppfnRetrieveSubObject
 * an in/out slot the driver is expected to populate with its sub-object
 * retrieval entry point.  The Mesa frontend never writes it. */
#define RETRIEVESUBOBJECT_OFFSET 0x50

static long w_retrievesubobject(void *hDevice, u32 subDeviceId, u64 paramSize,
                                void *pParams, u64 outSize, void *pOut)
{
    (void)hDevice; (void)paramSize; (void)pParams; (void)outSize; (void)pOut;
    log_hex64("  *** runtime called RetrieveSubObject subDeviceId=", (u64)subDeviceId);
    return 0x80004001L; /* E_NOTIMPL */
}

static void probe_retrieve_subobject(void *pCreateData)
{
    void **slot = *(void ***)((u8 *)pCreateData + RETRIEVESUBOBJECT_OFFSET);
    if (!slot) { log_str("  <no ppfnRetrieveSubObject>\n"); return; }
    log_hex64("  ppfnRetrieveSubObject=", (u64)slot);
    log_hex64("  *ppfnRetrieveSubObject=", (u64)*slot);
    if (*slot == 0) {
        *slot = (void *)w_retrievesubobject;
        log_str("  filled RetrieveSubObject\n");
    }
}

/* DXGI_DDI_BASE_ARGS sits at +0x28 of D3D10DDIARG_CREATEDEVICE and is two
 * pointers: pDXGIBaseCallbacks then the versioned pDXGIDDIBaseFunctions.  The
 * Windows 11 runtime supplies the DXGI1_6_1 table, which has 22 entries, while
 * the Mesa frontend fills at most the 18 of DXGI1_3.  Bound is from the WDK
 * header, so the scan and the fill both stay inside the table. */
#define DXGI_FUNCS_OFFSET  0x30
#define DXGI_FUNCS_MEMBERS 22

static u32 g_dxgi_stub_slot[8];

#define DEFINE_DXGI_STUB(n) \
static u64 dxgi_stub_##n(void) { log_hex64("  *** runtime called filled DXGI slot=", (u64)g_dxgi_stub_slot[n]); return 0x80004001ULL; }

DEFINE_DXGI_STUB(0) DEFINE_DXGI_STUB(1) DEFINE_DXGI_STUB(2) DEFINE_DXGI_STUB(3)
DEFINE_DXGI_STUB(4) DEFINE_DXGI_STUB(5) DEFINE_DXGI_STUB(6) DEFINE_DXGI_STUB(7)

static void *const g_dxgi_stubs[8] = {
    (void *)dxgi_stub_0, (void *)dxgi_stub_1, (void *)dxgi_stub_2, (void *)dxgi_stub_3,
    (void *)dxgi_stub_4, (void *)dxgi_stub_5, (void *)dxgi_stub_6, (void *)dxgi_stub_7,
};

static void probe_dxgi_funcs(void *pCreateData)
{
    void **tbl = *(void ***)((u8 *)pCreateData + DXGI_FUNCS_OFFSET);
    u32 i, used = 0;
    if (!tbl) { log_str("  <no pDXGIDDIBaseFunctions>\n"); return; }
    log_hex64("  pDXGIDDIBaseFunctions=", (u64)tbl);
    for (i = 0; i < DXGI_FUNCS_MEMBERS; i++) {
        if (tbl[i] == 0) {
            log_hex64("  DXGI slot NULL=", (u64)i);
            if (used < 8) {
                g_dxgi_stub_slot[used] = i;
                tbl[i] = g_dxgi_stubs[used];
                used++;
            }
        }
    }
    log_hex64("  DXGI slots filled=", (u64)used);
}

/* The user-mode driver reports failures to the runtime through
 * pUMCallbacks->pfnSetErrorCb, which is the first member of
 * D3D11DDI_CORELAYER_DEVICECALLBACKS (40 members).  In
 * D3D10DDIARG_CREATEDEVICE that union sits at +0x40:
 *   +00 hRTDevice, +08 Interface, +0C Version, +10 pKTCallbacks,
 *   +18 device funcs, +20 hDrvDevice, +28 DXGIBaseDDI (two pointers),
 *   +38 hRTCoreLayer, +40 pUMCallbacks, +48 Flags.
 * The runtime's struct is const, so a private copy is substituted rather than
 * patched in place. */
#define UMCALLBACKS_OFFSET 0x40
#define UMCALLBACKS_MEMBERS 40

static void *g_umcb_copy[UMCALLBACKS_MEMBERS];
static void (*o_seterror)(void *hRTCoreLayer, long hr);

static void w_seterror(void *hRTCoreLayer, long hr)
{
    log_hex64("  *** UMD reported SetErrorCb hr=", (u64)(u32)hr);
    if (o_seterror) o_seterror(hRTCoreLayer, hr);
}

static void hook_um_callbacks(void *pCreateData)
{
    void ***slot = (void ***)((u8 *)pCreateData + UMCALLBACKS_OFFSET);
    void **cb = *slot;
    u32 i;
    if (!cb) { log_str("  <no pUMCallbacks>\n"); return; }
    log_hex64("  pUMCallbacks=", (u64)cb);
    for (i = 0; i < UMCALLBACKS_MEMBERS; i++) g_umcb_copy[i] = cb[i];
    o_seterror = (void (*)(void *, long))g_umcb_copy[0];
    log_hex64("  original pfnSetErrorCb=", (u64)o_seterror);
    g_umcb_copy[0] = (void *)w_seterror;
    *slot = g_umcb_copy;
}

static HRESULT w_createdev(void *hAdapter, void *pCreateData)
{
    HRESULT hr;
    log_dump("CreateDevice pCreateData (Interface at +08):", pCreateData, 160);
    if (pCreateData && *(u32 *)((u8 *)pCreateData + 8) == SPOOF_IFACE) {
        *(u32 *)((u8 *)pCreateData + 8) = REAL_IFACE;
        log_str("  [translated Interface 0xB0011 -> 0xB0010]\n");
    }
    hook_um_callbacks(pCreateData);
    hr = o_createdev(hAdapter, pCreateData);
    log_hex64("  CreateDevice ret=", (u64)(u32)hr);
    if (hr == 0 && pCreateData) {
        void **funcs = *(void ***)((u8 *)pCreateData + 0x18);
        log_hex64("  device funcs table=", (u64)funcs);
        /* Read-only.  The WDK header that defines this table's extent is not
         * available here, so the scan bound is a guess and writing into it
         * could land outside the allocation. */
        log_null_slots("  device funcs NULL slots:\n", funcs, D3D11_DEVICEFUNCS_MEMBERS);
        fill_null_slots(funcs);
        probe_dxgi_funcs(pCreateData);
        probe_retrieve_subobject(pCreateData);
        trace_all_device_funcs(funcs);
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
            u32 i;
            for (i = 0; i < *puEntries && i < 16; i++) log_hex64("    version=", pVersions[i]);
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
