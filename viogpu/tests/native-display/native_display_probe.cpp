// SPDX-License-Identifier: MIT
// Functional exercise only: host/KMD identity and release traces plus visible
// pixels are required separately before claiming full-chain zero-copy.
#include <windows.h>
#include <d3d11.h>
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

static int exercise(unsigned frames, unsigned idle_ms, bool resize, bool native)
{
    if (!SetProcessDPIAware() && !IsProcessDPIAware())
        return 2;
    if (!SetEnvironmentVariableW(L"VIOGPU_NATIVE_HOST_SURFACE", native ? L"1" : L"0"))
        return 2;
    std::printf("PROBE native_requested=%u frames=%u idle_ms=%u resize=%u pixel_readbacks=0\n",
                static_cast<unsigned>(native), frames, idle_ms, static_cast<unsigned>(resize));

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
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                          device.GetAddressOf(), &actual, context.GetAddressOf());
    if (FAILED(hr)) {
        std::printf("RESULT passed=0 stage=device hr=%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    std::printf("DEVICE feature_level=%x hardware=1\n", static_cast<unsigned>(actual));
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
    desc.Width = 800;
    desc.Height = 600;
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
    unsigned presented = 0, resized = 0;
    const float colors[][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};
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
        context->ClearRenderTargetView(target.Get(), colors[(frame / 12) % 4]);
        stage = "present";
        hr = swapchain->Present(1, 0);
        if (hr != S_OK)
            break; // Occlusion is not successful visible-test progress.
        ++presented;
        if (frame < 4 || frame % 60 == 0)
            std::printf("PRESENT frame=%u color=%u hr=%08lx\n", frame, (frame / 12) % 4,
                        static_cast<unsigned long>(hr));
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
    swapchain.Reset();
    DestroyWindow(window);
    return passed ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned frames = 240, idle_ms = 0;
    bool resize = false, native = false;
    for (int index = 1; index < argc; ++index) {
        if (!std::strcmp(argv[index], "--native")) native = true;
        else if (!std::strcmp(argv[index], "--resize")) resize = true;
        else if (!std::strcmp(argv[index], "--frames") && index + 1 < argc) {
            if (!number(argv[++index], 600, &frames) || frames < 6) return 2;
        } else if (!std::strcmp(argv[index], "--idle-ms") && index + 1 < argc) {
            if (!number(argv[++index], 10000, &idle_ms)) return 2;
        } else {
            std::printf("Usage: native_display_probe [--native] [--resize] [--frames 6..600] [--idle-ms 0..10000]\n");
            return 2;
        }
    }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return 2;
    HANDLE guard = CreateThread(nullptr, 0, watchdog, done, 0, nullptr);
    if (!guard) { CloseHandle(done); return 2; }
    const int result = exercise(frames, idle_ms, resize, native);
    SetEvent(done);
    WaitForSingleObject(guard, INFINITE);
    CloseHandle(guard);
    CloseHandle(done);
    return result;
}
