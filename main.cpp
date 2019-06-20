#define WIND32_LEAN_AND_MEAN
#include <Windows.H>
#include <shellapi.h> // For CommandLineToArgvW

// The min/max macros conflict with like-named memeber functions
// Only use std::min and std::max defined in <algorithm>.
#if defined(min)
#undef min
#endif

#if defined(max)
#undef max
#endif

// In order to define a function called CreateWindow, the Window macro needs to 
// be undefined.
#if defined(CreateWindow)
#undef CreateWindow
#endif

//  Window Runtime Library. Needed for Microsoft::WRL::ComPtr<> template class.
#include <wrl.h>
using namespace Microsoft::WRL;

// DirectX 12 specific headers.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

// D3D12 extension library.
#include "d3dx12.h"

// STL Headers
#include <algorithm>
#include <cassert>
#include <chrono>

// Helper functions
#include "Helpers.h"

// The number of swap chain back buffers;
const uint8_t g_NumFrames = 3;
// Use WARP adapter
bool g_UseWarp = false;

uint32_t g_ClientWidth = 1280;
uint32_t g_ClientHeight = 720;

// Set to true once the DX12 objects have been initialized.
bool g_IsInitialized = false;

// Window handle
HWND g_hWnd;
// Window rectangle (use to toggle fullscreen state). Stores the previouse size of the 
// window so that whne switching back to windowed mode the window dimensions can be restored correctly.
RECT g_WindowRect;

// DirectX 12 Objects
ComPtr<ID3D12Device2> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain4> g_SwapChain;
ComPtr<ID3D12Resource> g_BackBuffers[g_NumFrames];
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[g_NumFrames];
ComPtr<ID3D12DescriptorHeap> g_RTVDescriptorHeap;
// Render Target View size is based o vender so needs to be set at initialization.
UINT g_RTVDescriptorSize;
UINT g_CurrentBackBufferIndex;

// Synchronization objects
ComPtr<ID3D12Fence> g_Fence;
uint64_t g_FenceValue = 0;
// Used to keep track of the fence values that were used to signal the command queue for a particular frame
uint64_t g_FrameFenceValues[g_NumFrames] = {};
HANDLE g_FenceEvent;

// Control swap chain present method
// By default, enable V-Sync.
// Canb be toggled with the V key.
bool g_Vsync = true;
bool g_TearingSupported = false;
// By default, use windowed mode.
// Can be toggled with the Alt+Enter of F11
bool g_Fullscreen = false;

// Window callback function
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

/**
 * Allows a fe of the globally defined variables to be overridden by suppluing command-line arguments
 * when the application is being executed.
 */
void ParseCommandLineArguments()
{
    int argc;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
 
    for (size_t i = 0; i < argc; ++i)
    {
        if (::wcscmp(argv[i], L"-w") == 0 || ::wcscmp(argv[i], L"--width") == 0)
        {
            g_ClientWidth = ::wcstol(argv[++i], nullptr, 10);
        }
        if (::wcscmp(argv[i], L"-h") == 0 || ::wcscmp(argv[i], L"--height") == 0)
        {
            g_ClientHeight = ::wcstol(argv[++i], nullptr, 10);
        }
        if (::wcscmp(argv[i], L"-warp") == 0 || ::wcscmp(argv[i], L"--warp") == 0)
        {
            g_UseWarp = true;
        }
    }
 
    // Free memory allocated by CommandLineToArgvW
    ::LocalFree(argv);
}

void EnableDebugLayer()
{
#if defined(_DEBUG)
    // Always enable the dbug layer before doing anything DX12 related
    // so all possible errors generated while creating DX2 objects
    // are coaught by the debug layer.
    ComPtr<ID3D12Debug> debugInterface;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
    debugInterface->EnableDebugLayer();
#endif
}

void RegisterWindowClass( HINSTANCE hInst, const wchar_t* windowClassName )
{
    // Register a window class for creating our render window with.
    WNDCLASSEXW windowClass = {};

    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &WndProc;
    windowClass.cbClsExtra = 0;
    windowClass.cbWndExtra = 0;
    windowClass.hInstance = hInst;
    windowClass.hIcon = ::LoadIcon(hInst, NULL);
    windowClass.hCursor = ::LoadCursor(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    windowClass.lpszMenuName = NULL;
    windowClass.lpszClassName = windowClassName;
    windowClass.hIconSm = ::LoadIcon(hInst, NULL);

    static ATOM atom = ::RegisterClassExW(&windowClass);
    assert(atom > 0);
}

HWND CreateWindow(const wchar_t* windowClassName, HINSTANCE hInst,
    const wchar_t* windowTitle, uint32_t width, uint32_t height)
{
    int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

    RECT windowRect = {0, 0, static_cast<LONG>(width, static_cast<LONG>(height))};
    ::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    // Center the window within the scrren. Clamp to 0,0 for the top left corner.
    int windowX = std::max<int>(0, (screenWidth - windowWidth) / 2);
    int windowY = std::max<int>(0, (screenHeight = windowHeight) / 2);
    HWND hWnd = ::CreateWindowExW(
        NULL,
        windowClassName,
        windowTitle,
        WS_OVERLAPPEDWINDOW,
        windowX,
        windowY,
        windowWidth,
        windowHeight,
        NULL,
        NULL,
        hInst,
        nullptr
    );

    assert(hWnd && "Failed to create window:");

    return hWnd;

}

/**
 * Before creating the DirectX 12 device, a compatible adapter must be present on the user's computer.
 * The GetAdapter function is used to query for a compatible adapter
 */
ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp)
{
    ComPtr<IDXGIFactory4> dxgiFactory;
    UINT createFactoryFlags = 0;

#if defined(_DEBUG)
    createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));
    ComPtr<IDXGIAdapter1> dxgiAdapter1;
    ComPtr<IDXGIAdapter4> dxgiAdapter4;

    if (useWarp)
    {
        ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
        ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
    }
    else
    {
        SIZE_T maxDedicatedVideoMemory = 0;
        for (UINT i = 0; dxgiFactory->EnumAdapters(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
            dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

            // Check to see if the adapter can create a D3D12 device withougt actually
            // creating it. The adapter with the largest dedicated video memory
            // is favored.
            if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FALG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(),
                    D3D_FEATURE_LEVEL_11_9, __uuidof(ID3D12Device), nullptr)) &&
                dxgiAdapterDesc1.DedicatedViedoMemory > maxDedicatedVideoMemory )
            {
                maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
                ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
            }
        }
    }

    return dxgiAdapter4;
}