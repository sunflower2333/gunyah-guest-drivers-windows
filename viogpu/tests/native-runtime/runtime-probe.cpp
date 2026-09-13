// Ordinary Microsoft runtime acceptance. Never call a private UMD harness entry
// point or accept a different hardware adapter as VIOGPU. WARP is an explicitly
// separate CI validation of the probe, not a hardware acceptance mode.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
constexpr UINT kSize = 64;
struct Failure
{
};

static void require(bool ok, const char *stage, HRESULT hr = E_FAIL)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL stage=%s hr=0x%08lx\n", stage, static_cast<unsigned long>(hr));
        throw Failure{};
    }
}
static void checked(HRESULT hr, const char *stage)
{
    require(SUCCEEDED(hr), stage, hr);
}

static std::wstring modulePath(HMODULE module)
{
    std::vector<wchar_t> path(32768);
    DWORD count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    require(count && count < path.size(), "module-path");
    return std::wstring(path.data(), count);
}

static HMODULE systemModule(const wchar_t *name)
{
    HMODULE module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(module != nullptr, "load-microsoft-runtime", HRESULT_FROM_WIN32(GetLastError()));
    wchar_t directory[MAX_PATH] = {};
    require(GetSystemDirectoryW(directory, ARRAYSIZE(directory)) != 0, "system-directory");
    const std::wstring expected = std::wstring(directory) + L"\\" + name;
    const std::wstring actual = modulePath(module);
    require(_wcsicmp(expected.c_str(), actual.c_str()) == 0, "reject-runtime-proxy");
    std::wprintf(L"SYSTEM_MODULE=%ls\n", actual.c_str());
    return module;
}

static std::wstring fileHash(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    require(file != INVALID_HANDLE_VALUE, "open-umd-file", HRESULT_FROM_WIN32(GetLastError()));
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0)
    {
        status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    }
    std::array<BYTE, 65536> bytes{};
    DWORD count = 0;
    while (status >= 0)
    {
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
        {
            status = static_cast<NTSTATUS>(0xc0000001u);
            break;
        }
        if (!count)
        {
            break;
        }
        status = BCryptHashData(hash, bytes.data(), count, 0);
    }
    std::array<BYTE, 32> digest{};
    if (status >= 0)
    {
        status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    }
    if (hash)
    {
        BCryptDestroyHash(hash);
    }
    if (algorithm)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    CloseHandle(file);
    require(status >= 0, "hash-umd-file", static_cast<HRESULT>(status));
    std::wstring result;
    for (BYTE value : digest)
    {
        constexpr wchar_t digits[] = L"0123456789abcdef";
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}

static void verifyUmd(const std::wstring &path, const std::wstring &sha256)
{
    const auto slash = path.find_last_of(L"\\/");
    require(slash != std::wstring::npos, "require-absolute-umd-path");
    HMODULE loaded = GetModuleHandleW(path.substr(slash + 1).c_str());
    require(loaded != nullptr, "expected-umd-not-loaded");
    const auto actual = modulePath(loaded);
    require(_wcsicmp(path.c_str(), actual.c_str()) == 0, "wrong-loaded-umd-path");
    const auto digest = fileHash(actual);
    require(_wcsicmp(sha256.c_str(), digest.c_str()) == 0, "wrong-loaded-umd-hash");
    std::wprintf(L"LOADED_UMD=%ls\nLOADED_UMD_SHA256=%ls\n", actual.c_str(), digest.c_str());
}

static ComPtr<IDXGIAdapter1> adapterFor(IDXGIFactory4 *factory, bool warp)
{
    ComPtr<IDXGIAdapter1> selected;
    if (warp)
    {
        checked(factory->EnumWarpAdapter(IID_PPV_ARGS(&selected)), "enum-warp-self-test");
    }
    else
    {
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> candidate;
            HRESULT hr = factory->EnumAdapters1(i, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            checked(hr, "enum-hardware-adapter");
            DXGI_ADAPTER_DESC1 desc{};
            checked(candidate->GetDesc1(&desc), "adapter-description");
            if (desc.VendorId != 0x1af4 || desc.DeviceId != 0x1050 || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            {
                continue;
            }
            require(!selected, "ambiguous-viogpu-adapters");
            selected = candidate;
        }
        require(selected != nullptr, "viogpu-hardware-adapter-not-found");
    }
    DXGI_ADAPTER_DESC1 desc{};
    checked(selected->GetDesc1(&desc), "selected-adapter-description");
    std::wprintf(L"ADAPTER=%ls PCI=%04x:%04x LUID=%08lx:%08lx SOFTWARE=%u\n",
                 desc.Description,
                 desc.VendorId,
                 desc.DeviceId,
                 static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                 desc.AdapterLuid.LowPart,
                 (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1u : 0u);
    for (UINT index = 0; index < 16; ++index)
    {
        ComPtr<IDXGIOutput> output;
        const HRESULT status = selected->EnumOutputs(index, &output);
        std::printf("ADAPTER_OUTPUT index=%u hr=0x%08lx\n", index, static_cast<unsigned long>(status));
        if (status == DXGI_ERROR_NOT_FOUND)
            break;
        checked(status, "enumerate-selected-adapter-output");
        DXGI_OUTPUT_DESC outputDesc{};
        checked(output->GetDesc(&outputDesc), "selected-adapter-output-description");
        std::wprintf(L"ADAPTER_MONITOR=%ls attached=%u monitor=%p\n", outputDesc.DeviceName,
                     outputDesc.AttachedToDesktop ? 1u : 0u, outputDesc.Monitor);
    }
    return selected;
}

static void messages();

static void windowState(HWND window, const char *stage)
{
    RECT rect{}, client{};
    GetWindowRect(window, &rect);
    GetClientRect(window, &client);
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    wchar_t desktopName[256] = {}, inputName[256] = {};
    DWORD needed = 0;
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                             desktopName, sizeof(desktopName), &needed);
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    const DWORD inputError = input ? ERROR_SUCCESS : GetLastError();
    if (input)
    {
        GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &needed);
        CloseDesktop(input);
    }
    std::printf("WINDOW_STATE stage=%s visible=%u iconic=%u foreground=%u session=%lu "
                "rect=%ld,%ld,%ld,%ld client=%ld,%ld monitor=%ld,%ld,%ld,%ld\n",
                stage, IsWindowVisible(window) ? 1u : 0u, IsIconic(window) ? 1u : 0u,
                GetForegroundWindow() == window ? 1u : 0u, session,
                rect.left, rect.top, rect.right, rect.bottom, client.right, client.bottom,
                monitor.rcMonitor.left, monitor.rcMonitor.top,
                monitor.rcMonitor.right, monitor.rcMonitor.bottom);
    std::wprintf(L"WINDOW_DESKTOP stage=%hs current=%ls input=%ls input_error=%lu\n",
                 stage, desktopName, inputName, inputError);

    // IsWindowVisible only describes WS_VISIBLE. DWM cloaking, an empty
    // effective clip, and a stalled compositor are separate observations.
    BOOL composition = FALSE;
    const HRESULT compositionStatus = DwmIsCompositionEnabled(&composition);
    DWORD cloaked = 0;
    const HRESULT cloakStatus = DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    const HRESULT timingStatus = DwmGetCompositionTimingInfo(nullptr, &timing);
    std::printf("WINDOW_COMPOSITION stage=%s enabled_hr=0x%08lx enabled=%u "
                "cloak_hr=0x%08lx cloaked=0x%lx style=0x%lx exstyle=0x%lx "
                "timing_hr=0x%08lx frame=%llu displayed=%llu refresh=%llu qpc_vblank=%llu\n",
                stage, static_cast<unsigned long>(compositionStatus), composition ? 1u : 0u,
                static_cast<unsigned long>(cloakStatus), cloaked,
                static_cast<unsigned long>(GetWindowLongPtrW(window, GWL_STYLE)),
                static_cast<unsigned long>(GetWindowLongPtrW(window, GWL_EXSTYLE)),
                static_cast<unsigned long>(timingStatus), timing.cFrame, timing.cFramesDisplayed,
                timing.cRefresh, timing.qpcVBlank);

    HDC dc = GetDC(window);
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    RECT clip{}, systemClip{};
    const int clipKind = dc ? GetClipBox(dc, &clip) : ERROR;
    const int systemResult = dc && region ? GetRandomRgn(dc, region, SYSRGN) : -1;
    const int systemKind = systemResult == 1 ? GetRgnBox(region, &systemClip) : ERROR;
    POINT center = {client.right / 2, client.bottom / 2};
    ClientToScreen(window, &center);
    const HWND centerWindow = WindowFromPoint(center);
    std::printf("WINDOW_CLIP stage=%s dc=%u kind=%d rect=%ld,%ld,%ld,%ld "
                "system_result=%d system_kind=%d system_rect=%ld,%ld,%ld,%ld "
                "center=%ld,%ld center_owned=%u\n",
                stage, dc ? 1u : 0u, clipKind, clip.left, clip.top, clip.right, clip.bottom,
                systemResult, systemKind, systemClip.left, systemClip.top,
                systemClip.right, systemClip.bottom, center.x, center.y,
                centerWindow == window || IsChild(window, centerWindow) ? 1u : 0u);
    if (region)
        DeleteObject(region);
    if (dc)
        ReleaseDC(window, dc);
}

static HWND makeWindow()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VioGpuMicrosoftRuntimeProbe";
    require(RegisterClassW(&wc) != 0, "register-window");
    HWND window = CreateWindowExW(0,
                                  wc.lpszClassName,
                                  L"VIOGPU Microsoft D3D11 runtime test",
                                  WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  320,
                                  320,
                                  nullptr,
                                  nullptr,
                                  wc.hInstance,
                                  nullptr);
    require(window != nullptr, "create-window");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    GetStartupInfoW(&startup);
    std::printf("WINDOW_STARTUP flags=0x%08lx show=%u\n", startup.dwFlags,
                static_cast<unsigned>(startup.wShowWindow));
    ShowWindow(window, SW_SHOW);
    windowState(window, "initial-show");
    // First ShowWindow can be overridden by the process startup show state.
    // This test requires a normal visible window and services its initial paint.
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    messages();
    windowState(window, "ready");
    return window;
}

static void messages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static void d3d11(IDXGIFactory4 *factory,
                  IDXGIAdapter1 *adapter,
                  bool warp,
                  const std::wstring &expectedUmd,
                  const std::wstring &expectedHash,
                  D3D_FEATURE_LEVEL minimum)
{
    HMODULE runtime = systemModule(L"d3d11.dll");
    const auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(runtime, "D3D11CreateDevice"));
    require(create != nullptr, "resolve-d3d11-runtime-entry");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL actual{};
    const D3D_FEATURE_LEVEL supported[] = {D3D_FEATURE_LEVEL_11_1,
                                           D3D_FEATURE_LEVEL_11_0,
                                           D3D_FEATURE_LEVEL_10_1,
                                           D3D_FEATURE_LEVEL_10_0};
    std::vector<D3D_FEATURE_LEVEL> levels;
    for (const auto level : supported)
    {
        if (level >= minimum)
        {
            levels.push_back(level);
        }
    }
    std::printf("D3D11_REQUIRED_FEATURE_LEVEL=0x%04x\n", static_cast<unsigned>(minimum));
    checked(create(adapter,
                   D3D_DRIVER_TYPE_UNKNOWN,
                   nullptr,
                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                   levels.data(),
                   static_cast<UINT>(levels.size()),
                   D3D11_SDK_VERSION,
                   &device,
                   &actual,
                   &context),
            "D3D11CreateDevice");
    std::printf("D3D11_FEATURE_LEVEL=0x%04x\n", static_cast<unsigned>(actual));
    require(actual >= minimum, "unexpected-feature-level-downgrade");
    if (!warp)
    {
        verifyUmd(expectedUmd, expectedHash);
    }

    static const char shader[] = "cbuffer Color : register(b0) {float4 color;}"
                                 "float4 vs(uint i:SV_VertexID):SV_Position {"
                                 "float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[i],0,1);}"
                                 "float4 ps():SV_Target {return color;}";
    HMODULE compiler = systemModule(L"d3dcompiler_47.dll");
    using Compile = decltype(&D3DCompile);
    const auto compile = reinterpret_cast<Compile>(GetProcAddress(compiler, "D3DCompile"));
    require(compile != nullptr, "resolve-shader-compiler");
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    checked(compile(shader,
                    sizeof(shader) - 1,
                    "runtime-probe",
                    nullptr,
                    nullptr,
                    "vs",
                    "vs_4_0",
                    D3DCOMPILE_ENABLE_STRICTNESS,
                    0,
                    &vsCode,
                    &errors),
            "compile-vs");
    errors.Reset();
    checked(compile(shader,
                    sizeof(shader) - 1,
                    "runtime-probe",
                    nullptr,
                    nullptr,
                    "ps",
                    "ps_4_0",
                    D3DCOMPILE_ENABLE_STRICTNESS,
                    0,
                    &psCode,
                    &errors),
            "compile-ps");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    checked(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "create-vs");
    checked(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "create-ps");
    ComPtr<ID3D11Texture2D> target, staging;
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = texture.Height = kSize;
    texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET;
    checked(device->CreateTexture2D(&texture, nullptr, &target), "create-render-target");
    texture.Usage = D3D11_USAGE_STAGING;
    texture.BindFlags = 0;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    checked(device->CreateTexture2D(&texture, nullptr, &staging), "create-readback");
    ComPtr<ID3D11RenderTargetView> rtv;
    checked(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "create-rtv");
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 16;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constant;
    checked(device->CreateBuffer(&buffer, nullptr, &constant), "create-constant-buffer");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rasterState;
    checked(device->CreateRasterizerState(&raster, &rasterState), "create-rasterizer");

    HWND window = nullptr;
    ComPtr<IDXGISwapChain> swapchain;
    if (!warp)
    {
        window = makeWindow();
        DXGI_SWAP_CHAIN_DESC swap{};
        swap.BufferDesc.Width = swap.BufferDesc.Height = kSize;
        swap.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap.SampleDesc.Count = 1;
        swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap.BufferCount = 2;
        swap.OutputWindow = window;
        swap.Windowed = TRUE;
        swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        checked(factory->CreateSwapChain(device.Get(), &swap, &swapchain), "create-runtime-swapchain");
        ComPtr<IDXGIOutput> output;
        const HRESULT outputStatus = swapchain->GetContainingOutput(&output);
        std::printf("SWAPCHAIN_OUTPUT hr=0x%08lx\n", static_cast<unsigned long>(outputStatus));
        if (SUCCEEDED(outputStatus) && output)
        {
            DXGI_OUTPUT_DESC desc{};
            checked(output->GetDesc(&desc), "swapchain-output-description");
            std::wprintf(L"SWAPCHAIN_MONITOR=%ls attached=%u rect=%ld,%ld,%ld,%ld\n",
                         desc.DeviceName, desc.AttachedToDesktop ? 1u : 0u,
                         desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
                         desc.DesktopCoordinates.right, desc.DesktopCoordinates.bottom);
            ComPtr<IDXGIAdapter> owner;
            checked(output->GetParent(IID_PPV_ARGS(&owner)), "swapchain-output-owner");
            DXGI_ADAPTER_DESC ownerDesc{};
            checked(owner->GetDesc(&ownerDesc), "swapchain-output-owner-description");
            std::wprintf(L"SWAPCHAIN_OUTPUT_OWNER=%ls LUID=%08lx:%08lx\n", ownerDesc.Description,
                         static_cast<unsigned long>(ownerDesc.AdapterLuid.HighPart), ownerDesc.AdapterLuid.LowPart);
        }
    }
    for (UINT frame = 0; frame < 4; ++frame)
    {
        const float color[4] = {frame & 1 ? 0.0f : 1.0f, frame & 1 ? 1.0f : 0.0f, 0.0f, 1.0f};
        const BYTE expected[4] = {static_cast<BYTE>(frame & 1 ? 0 : 255),
                                  static_cast<BYTE>(frame & 1 ? 255 : 0),
                                  0,
                                  255};
        context->UpdateSubresource(constant.Get(), 0, nullptr, color, 0, 0);
        const float clear[4] = {0, 0, 1, 1};
        context->ClearRenderTargetView(rtv.Get(), clear);
        ID3D11RenderTargetView *view = rtv.Get();
        context->OMSetRenderTargets(1, &view, nullptr);
        D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterState.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer *cb = constant.Get();
        context->PSSetConstantBuffers(0, 1, &cb);
        context->Draw(3, 0);
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map-readback");
        UINT mismatches = 0;
        std::array<UINT, 5> categories{};
        if (!mapped.pData || mapped.RowPitch < kSize * 4)
        {
            mismatches = kSize * kSize;
        }
        else
        {
            for (UINT y = 0; y < kSize; ++y)
            {
                for (UINT x = 0; x < kSize; ++x)
                {
                    const BYTE *pixel = static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch + x * 4;
                    const BYTE red[4] = {255, 0, 0, 255};
                    const BYTE green[4] = {0, 255, 0, 255};
                    const BYTE blue[4] = {0, 0, 255, 255};
                    const BYTE black[4] = {0, 0, 0, 255};
                    const size_t category = !memcmp(pixel, red, 4) ? 0 : !memcmp(pixel, green, 4) ? 1 :
                                            !memcmp(pixel, blue, 4) ? 2 : !memcmp(pixel, black, 4) ? 3 : 4;
                    ++categories[category];
                    if ((x == 0 && y == 0) || (x == kSize / 2 && y == kSize / 2) ||
                        (x == kSize - 1 && y == kSize - 1))
                    {
                        std::printf("PIXEL frame=%u x=%u y=%u rgba=%u,%u,%u,%u expected=%u,%u,%u,%u row_pitch=%u\n",
                                    frame, x, y, pixel[0], pixel[1], pixel[2], pixel[3],
                                    expected[0], expected[1], expected[2], expected[3], mapped.RowPitch);
                    }
                    if (memcmp(pixel, expected, 4))
                    {
                        ++mismatches;
                    }
                }
            }
        }
        context->Unmap(staging.Get(), 0);
        std::printf("PIXEL_COUNTS frame=%u red=%u green=%u blue=%u black=%u other=%u\n",
                    frame, categories[0], categories[1], categories[2], categories[3], categories[4]);
        std::printf("DRAW_READBACK frame=%u pixels=%u mismatches=%u\n", frame, kSize * kSize, mismatches);
        require(!mismatches, "shader-pixel-mismatch");
        if (swapchain)
        {
            ComPtr<ID3D11Texture2D> backbuffer;
            checked(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "get-backbuffer");
            context->CopyResource(backbuffer.Get(), target.Get());
            messages();
            windowState(window, "before-present");
            HRESULT hr = swapchain->Present(1, 0);
            std::printf("PRESENT_RESULT frame=%u hr=0x%08lx\n", frame, static_cast<unsigned long>(hr));
            windowState(window, "after-present");
            if (frame == 0 && hr == DXGI_STATUS_OCCLUDED)
            {
                // DXGI may defer a new window's redirection surface until the
                // compositor processes its first Present. Bound this startup
                // diagnostic and still require four actual S_OK frame presents.
                const ULONGLONG began = GetTickCount64();
                UINT attempts = 0;
                while (hr == DXGI_STATUS_OCCLUDED && GetTickCount64() - began < 2000)
                {
                    Sleep(16);
                    messages();
                    const HRESULT visible = swapchain->Present(0, DXGI_PRESENT_TEST);
                    require(visible == S_OK || visible == DXGI_STATUS_OCCLUDED,
                            "runtime-present-visibility-test", visible);
                    if (visible == S_OK)
                        hr = swapchain->Present(1, 0);
                    ++attempts;
                }
                std::printf("PRESENT_STARTUP attempts=%u elapsed_ms=%llu hr=0x%08lx\n",
                            attempts, GetTickCount64() - began, static_cast<unsigned long>(hr));
                windowState(window, "after-startup");
            }
            require(hr == S_OK, "runtime-present-not-visible-success", hr);
            std::printf("PRESENT_ACCEPTED frame=%u\n", frame);
            messages();
            Sleep(250);
        }
        checked(device->GetDeviceRemovedReason(), "device-removed");
    }
    context->ClearState();
    context->Flush();
    swapchain.Reset();
    if (window)
    {
        DestroyWindow(window);
    }
    std::puts(warp ? "HARNESS_SELF_TEST_PASS=D3D11 WARP pixels=16384 hardware_acceptance=0"
                   : "NATIVE_RUNTIME_PASS=D3D11 pixels=16384 presents=4 final_display_pixels=not_observed");
}

static void d3d12(IDXGIAdapter1 *adapter,
                  bool warp,
                  const std::wstring &expectedUmd,
                  const std::wstring &expectedHash,
                  D3D_FEATURE_LEVEL minimum)
{
    HMODULE runtime = systemModule(L"d3d12.dll");
    const auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(runtime, "D3D12CreateDevice"));
    require(create != nullptr, "resolve-d3d12-runtime-entry");
    ComPtr<ID3D12Device> device;
    std::printf("D3D12_REQUIRED_FEATURE_LEVEL=0x%04x\n", static_cast<unsigned>(minimum));
    checked(create(adapter, minimum, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    if (!warp)
    {
        verifyUmd(expectedUmd, expectedHash);
    }
    DXGI_ADAPTER_DESC1 desc{};
    checked(adapter->GetDesc1(&desc), "d3d12-adapter-description");
    const LUID luid = device->GetAdapterLuid();
    require(luid.LowPart == desc.AdapterLuid.LowPart && luid.HighPart == desc.AdapterLuid.HighPart,
            "d3d12-device-adapter-mismatch");
    checked(device->GetDeviceRemovedReason(), "d3d12-device-removed");
    std::puts(warp ? "HARNESS_SELF_TEST_PASS=D3D12 WARP hardware_acceptance=0"
                   : "NATIVE_RUNTIME_ACTIVATION_PASS=D3D12 rendering=not_tested present=not_tested");
}

int wmain(int argc, wchar_t **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring api, expectedUmd, expectedHash;
    bool warp = false;
    D3D_FEATURE_LEVEL minimum = D3D_FEATURE_LEVEL_11_0;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring option = argv[i];
            if (option == L"--self-test-warp")
            {
                warp = true;
            }
            else if (option == L"--api" && i + 1 < argc)
            {
                api = argv[++i];
            }
            else if (option == L"--expect-umd" && i + 1 < argc)
            {
                expectedUmd = argv[++i];
            }
            else if (option == L"--expect-umd-sha256" && i + 1 < argc)
            {
                expectedHash = argv[++i];
            }
            else if (option == L"--minimum-feature-level" && i + 1 < argc)
            {
                const std::wstring value = argv[++i];
                if (value == L"10_0")
                {
                    minimum = D3D_FEATURE_LEVEL_10_0;
                }
                else if (value == L"10_1")
                {
                    minimum = D3D_FEATURE_LEVEL_10_1;
                }
                else if (value == L"11_0")
                {
                    minimum = D3D_FEATURE_LEVEL_11_0;
                }
                else if (value == L"11_1")
                {
                    minimum = D3D_FEATURE_LEVEL_11_1;
                }
                else
                {
                    require(false, "invalid-minimum-feature-level");
                }
            }
            else
            {
                require(false, "arguments");
            }
        }
        require(api == L"d3d11" || api == L"d3d12", "api-required");
        require(api != L"d3d12" || minimum >= D3D_FEATURE_LEVEL_11_0, "d3d12-minimum-feature-level");
        if (warp)
        {
            require(expectedUmd.empty() && expectedHash.empty(), "self-test-not-hardware-acceptance");
        }
        else
        {
            require(expectedUmd.size() > 3 && expectedUmd[1] == L':' && expectedUmd[2] == L'\\' && expectedHash.size() == 64,
                    "expected-umd-identity-required");
            require(_wcsicmp(fileHash(expectedUmd).c_str(), expectedHash.c_str()) == 0, "expected-umd-preflight-hash");
            const auto leaf = expectedUmd.substr(expectedUmd.find_last_of(L"\\/") + 1);
            require(GetModuleHandleW(leaf.c_str()) == nullptr, "expected-umd-already-loaded");
        }
        HMODULE dxgi = systemModule(L"dxgi.dll");
        using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
        const auto create = reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        require(create != nullptr, "resolve-dxgi-runtime-entry");
        ComPtr<IDXGIFactory4> factory;
        checked(create(IID_PPV_ARGS(&factory)), "create-dxgi-factory");
        const auto adapter = adapterFor(factory.Get(), warp);
        if (api == L"d3d11")
        {
            d3d11(factory.Get(), adapter.Get(), warp, expectedUmd, expectedHash, minimum);
        }
        else
        {
            d3d12(adapter.Get(), warp, expectedUmd, expectedHash, minimum);
        }
        return 0;
    }
    catch (const Failure &)
    {
        return 1;
    }
    catch (...)
    {
        std::fputs("FAIL stage=unexpected-exception\n", stderr);
        return 2;
    }
}
