// SPDX-License-Identifier: MIT
// Functional exercise only: host/KMD identity and release traces plus visible
// pixels are required separately before claiming full-chain zero-copy.
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

static DWORD WINAPI watchdog(void *done)
{
    if (WaitForSingleObject(static_cast<HANDLE>(done), 60000) != WAIT_OBJECT_0) {
        std::printf("RESULT passed=0 stage=watchdog timeout_ms=60000\n");
        TerminateProcess(GetCurrentProcess(), 124);
    }
    return 0;
}

static bool pump()
{
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT)
            return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

static bool number(const char *text, unsigned limit, unsigned *value)
{
    char *end = nullptr;
    unsigned long parsed = std::strtoul(text, &end, 10);
    if (!*text || *end || parsed > limit)
        return false;
    *value = static_cast<unsigned>(parsed);
    return true;
}

// Separate from flip-surface support: explicitly request an NT-shareable
// resource, reopen it on another device, and use it after creator destruction.
static HRESULT shared_nt_smoke(IDXGIAdapter1 *adapter, ID3D11Device *creator,
                               ID3D11DeviceContext *creator_context)
{
    const char *stage = "create";
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 513;
    desc.Height = 257;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ComPtr<ID3D11Texture2D> source;
    HRESULT hr = creator->CreateTexture2D(&desc, nullptr, source.GetAddressOf());
    HANDLE shared = nullptr;
    do {
        if (FAILED(hr)) break;
        stage = "share";
        ComPtr<IDXGIResource1> resource;
        hr = source.As(&resource);
        if (FAILED(hr)) break;
        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                          nullptr, &shared);
        if (FAILED(hr)) break;
        resource.Reset();
        stage = "second-device";
        ComPtr<ID3D11Device> importer;
        ComPtr<ID3D11DeviceContext> importer_context;
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              nullptr, 0, D3D11_SDK_VERSION, importer.GetAddressOf(), nullptr,
                              importer_context.GetAddressOf());
        if (FAILED(hr)) break;
        ComPtr<ID3D11Device1> importer1;
        hr = importer.As(&importer1);
        if (FAILED(hr)) break;
        stage = "open";
        ComPtr<ID3D11Texture2D> opened;
        hr = importer1->OpenSharedResource1(shared, IID_PPV_ARGS(opened.GetAddressOf()));
        if (FAILED(hr)) break;
        CloseHandle(shared);
        shared = nullptr;
        ComPtr<IDXGIKeyedMutex> source_mutex, opened_mutex;
        hr = source.As(&source_mutex);
        if (FAILED(hr)) break;
        hr = opened.As(&opened_mutex);
        if (FAILED(hr)) break;
        ComPtr<ID3D11RenderTargetView> source_target, opened_target;
        hr = creator->CreateRenderTargetView(source.Get(), nullptr, source_target.GetAddressOf());
        if (FAILED(hr)) break;
        hr = importer->CreateRenderTargetView(opened.Get(), nullptr, opened_target.GetAddressOf());
        if (FAILED(hr)) break;
        stage = "creator-acquire";
        hr = source_mutex->AcquireSync(0, 5000);
        if (hr != S_OK) break;
        const float initial[] = {0.25f, 0.5f, 0.75f, 1};
        creator_context->ClearRenderTargetView(source_target.Get(), initial);
        creator_context->Flush();
        hr = source_mutex->ReleaseSync(1);
        if (FAILED(hr)) break;
        // The independent importer must outlive all creator resource handles.
        source_target.Reset();
        source_mutex.Reset();
        source.Reset();
        stage = "importer-after-creator-destroy";
        hr = opened_mutex->AcquireSync(1, 5000);
        if (hr != S_OK) break;
        const float final_color[] = {0.75f, 0.25f, 0.5f, 1};
        importer_context->ClearRenderTargetView(opened_target.Get(), final_color);
        importer_context->Flush();
        hr = opened_mutex->ReleaseSync(2);
        if (FAILED(hr)) break;
        stage = "gpu-completion";
        D3D11_QUERY_DESC query_desc = {D3D11_QUERY_EVENT, 0};
        ComPtr<ID3D11Query> query;
        hr = importer->CreateQuery(&query_desc, query.GetAddressOf());
        if (FAILED(hr)) break;
        importer_context->End(query.Get());
        importer_context->Flush();
        const ULONGLONG deadline = GetTickCount64() + 5000;
        do {
            hr = importer_context->GetData(query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr != S_FALSE) break;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        if (hr == S_FALSE) hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        if (SUCCEEDED(hr)) hr = importer->GetDeviceRemovedReason();
    } while (false);
    if (shared) CloseHandle(shared);
    // WAIT_TIMEOUT/WAIT_ABANDONED from AcquireSync are positive status codes.
    if (hr != S_OK && SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32(static_cast<DWORD>(hr));
    std::printf("SHARED_NT passed=%u stage=%s hr=%08lx pixel_preservation_verified=0 flip_support_verified=0\n",
                static_cast<unsigned>(hr == S_OK), stage, static_cast<unsigned long>(hr));
    return hr;
}

static int exercise(unsigned frames, unsigned idle_ms, unsigned color_hold_ms, bool resize, bool native,
                    bool shared_nt, bool fullscreen)
{
    if (!SetProcessDPIAware() && !IsProcessDPIAware())
        return 2;
    if (!SetEnvironmentVariableW(L"VIOGPU_NATIVE_HOST_SURFACE", native ? L"1" : L"0"))
        return 2;
    std::printf("PROBE native_requested=%u frames=%u idle_ms=%u color_hold_ms=%u resize=%u fullscreen=%u pixel_readbacks=0\n",
                static_cast<unsigned>(native), frames, idle_ms, color_hold_ms, static_cast<unsigned>(resize),
                static_cast<unsigned>(fullscreen));

    HRESULT hr;
    const char *stage = "factory";
    ComPtr<IDXGIFactory2> factory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        std::printf("RESULT passed=0 stage=%s hr=%08lx\n", stage, static_cast<unsigned long>(hr));
        return 1;
    }
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description = {};
    for (UINT index = 0; ; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        hr = factory->EnumAdapters1(index, candidate.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr))
            return 1;
        hr = candidate->GetDesc1(&description);
        if (SUCCEEDED(hr) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            description.VendorId == 0x1af4 && description.DeviceId == 0x1050) {
            adapter = candidate;
            break;
        }
    }
    if (!adapter) {
        std::printf("RESULT passed=0 stage=viogpu-adapter missing=1\n");
        return 1;
    }
    std::printf("ADAPTER vendor=%04x device=%04x luid=%08lx:%08lx\n",
                description.VendorId, description.DeviceId,
                static_cast<unsigned long>(description.AdapterLuid.HighPart),
                static_cast<unsigned long>(description.AdapterLuid.LowPart));
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL actual;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                       D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                          device.GetAddressOf(), &actual, context.GetAddressOf());
    if (FAILED(hr)) {
        std::printf("RESULT passed=0 stage=device hr=%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    std::printf("DEVICE feature_level=%x hardware=1\n", static_cast<unsigned>(actual));
    if (shared_nt && shared_nt_smoke(adapter.Get(), device.Get(), context.Get()) != S_OK)
        return 1;
    WNDCLASSW wc = {};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = window_proc;
    wc.lpszClassName = L"VioGpuNativeAhbProbe";
    if (!RegisterClassW(&wc))
        return 2;
    HWND window = CreateWindowW(wc.lpszClassName, L"VIOGPU native AHB functional probe",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 64, 64, 800, 600,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!window)
        return 2;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = fullscreen ? static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN)) : 800;
    desc.Height = fullscreen ? static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN)) : 600;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> swapchain;
    hr = factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr,
                                        swapchain.GetAddressOf());
    if (FAILED(hr)) {
        std::printf("RESULT passed=0 stage=flip-swapchain hr=%08lx\n", static_cast<unsigned long>(hr));
        DestroyWindow(window);
        return 1;
    }
    if (fullscreen) {
        hr = swapchain->SetFullscreenState(TRUE, nullptr);
        if (SUCCEEDED(hr))
            hr = swapchain->ResizeBuffers(3, desc.Width, desc.Height, desc.Format, 0);
        if (FAILED(hr)) {
            std::printf("RESULT passed=0 stage=fullscreen hr=%08lx\n", static_cast<unsigned long>(hr));
            swapchain->SetFullscreenState(FALSE, nullptr);
            DestroyWindow(window);
            return 1;
        }
        std::printf("FULLSCREEN extent=%ux%u direct_scanout_verified=0\n", desc.Width, desc.Height);
    }
    unsigned presented = 0, resized = 0;
    const float colors[][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};
    const unsigned sample_colors[][3] = {{37,149,211}, {211,37,149}, {149,211,37}, {211,149,37}};
    for (unsigned frame = 0; frame < frames; ++frame) {
        if (!pump()) {
            stage = "window-closed";
            hr = E_ABORT;
            break;
        }
        if (resize && (frame == frames / 3 || frame == frames * 2 / 3)) {
            stage = "resize";
            context->ClearState();
            context->Flush();
            const UINT width = resized == 0 ? 513 : 800;
            const UINT height = resized == 0 ? 257 : 600;
            hr = swapchain->ResizeBuffers(3, width, height, desc.Format, 0);
            if (FAILED(hr))
                break;
            ++resized;
            SetWindowPos(window, nullptr, 64 + 16 * resized, 64, width, height, SWP_NOZORDER);
            std::printf("RESIZE frame=%u extent=%ux%u\n", frame, width, height);
        }
        ComPtr<ID3D11Texture2D> image;
        ComPtr<ID3D11RenderTargetView> target;
        stage = "backbuffer";
        hr = swapchain->GetBuffer(0, IID_PPV_ARGS(image.GetAddressOf()));
        if (FAILED(hr))
            break;
        stage = "render-target";
        hr = device->CreateRenderTargetView(image.Get(), nullptr, target.GetAddressOf());
        if (FAILED(hr))
            break;
        const unsigned color_index = color_hold_ms ? frame % 4 : (frame / 12) % 4;
        float sample_color[4] = {0, 0, 0, 1};
        for (unsigned channel = 0; channel < 3; ++channel)
            sample_color[channel] = sample_colors[color_index][channel] / 255.0f;
        context->ClearRenderTargetView(target.Get(), color_hold_ms ? sample_color : colors[color_index]);
        stage = "present";
        hr = swapchain->Present(1, 0);
        if (hr != S_OK)
            break; // Occlusion is not successful visible-test progress.
        ++presented;
        if (frame < 4 || frame % 60 == 0)
            std::printf("PRESENT frame=%u color=%u hr=%08lx\n", frame, color_index,
                        static_cast<unsigned long>(hr));
        if (color_hold_ms) {
            std::printf("PIXEL_SAMPLE frame=%u expected_rgb=%u,%u,%u hold_ms=%u tick=%llu\n", frame,
                        sample_colors[color_index][0], sample_colors[color_index][1],
                        sample_colors[color_index][2], color_hold_ms, GetTickCount64());
            const ULONGLONG until = GetTickCount64() + color_hold_ms;
            while (GetTickCount64() < until && pump())
                Sleep(10);
        }
        if (idle_ms && frame == frames / 2) {
            // Keep dispatching messages while leaving the current front buffer
            // untouched; it must not acquire a false release timeout.
            std::printf("IDLE begin duration_ms=%u\n", idle_ms);
            const ULONGLONG until = GetTickCount64() + idle_ms;
            while (GetTickCount64() < until && pump())
                Sleep(10);
            std::printf("IDLE end\n");
        }
    }
    context->ClearState();
    context->Flush();
    HRESULT removed = device->GetDeviceRemovedReason();
    const bool passed = presented == frames && (!resize || resized == 2) && removed == S_OK;
    std::printf("RESULT passed=%u stage=%s hr=%08lx removed=%08lx presents=%u resizes=%u "
                "native_requested=%u zero_copy_verified=0 pixel_readbacks=0\n",
                static_cast<unsigned>(passed), stage, static_cast<unsigned long>(hr),
                static_cast<unsigned long>(removed), presented, resized, static_cast<unsigned>(native));
    if (fullscreen)
        swapchain->SetFullscreenState(FALSE, nullptr);
    swapchain.Reset();
    DestroyWindow(window);
    return passed ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned frames = 240, idle_ms = 0, color_hold_ms = 0;
    bool resize = false, native = false, shared_nt = false, fullscreen = false;
    for (int index = 1; index < argc; ++index) {
        if (!std::strcmp(argv[index], "--native")) native = true;
        else if (!std::strcmp(argv[index], "--shared-nt")) shared_nt = true;
        else if (!std::strcmp(argv[index], "--fullscreen")) fullscreen = true;
        else if (!std::strcmp(argv[index], "--resize")) resize = true;
        else if (!std::strcmp(argv[index], "--frames") && index + 1 < argc) {
            if (!number(argv[++index], 600, &frames) || frames < 6) return 2;
        } else if (!std::strcmp(argv[index], "--idle-ms") && index + 1 < argc) {
            if (!number(argv[++index], 10000, &idle_ms)) return 2;
        } else if (!std::strcmp(argv[index], "--color-hold-ms") && index + 1 < argc) {
            if (!number(argv[++index], 2000, &color_hold_ms)) return 2;
        } else {
            std::printf("Usage: native_display_probe [--native] [--shared-nt] [--fullscreen | --resize] [--frames 6..600] "
                        "[--idle-ms 0..10000] [--color-hold-ms 0..2000]\n");
            return 2;
        }
    }
    if (frames * color_hold_ms + idle_ms > 45000) return 2;
    if (fullscreen && resize) return 2;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return 2;
    HANDLE guard = CreateThread(nullptr, 0, watchdog, done, 0, nullptr);
    if (!guard) { CloseHandle(done); return 2; }
    const int result = exercise(frames, idle_ms, color_hold_ms, resize, native, shared_nt, fullscreen);
    SetEvent(done);
    WaitForSingleObject(guard, INFINITE);
    CloseHandle(guard);
    CloseHandle(done);
    return result;
}
