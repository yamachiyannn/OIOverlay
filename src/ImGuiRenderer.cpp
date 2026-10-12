#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d9.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "ImGuiRenderer.hpp"

#pragma comment(lib, "d3d9.lib")

namespace
{
static_assert(sizeof(void*) == 4, "OIOverlay in-game hooks require the 32-bit th123.exe process.");

// Addresses and instruction templates match the default CallNearHookMethod in
// SokuDev/InGameHostlist for the supported 32-bit th123.exe build. Convert the
// published preferred VAs to RVAs in one place to avoid hand-calculation errors.
constexpr uintptr_t kPreferredImageBase = 0x00400000;
constexpr uintptr_t RvaFromPreferredVa(uintptr_t preferredVa)
{
    return preferredVa - kPreferredImageBase;
}
constexpr uintptr_t kSokuSetupCallRva = RvaFromPreferredVa(0x007FB871);
constexpr uintptr_t kEndSceneCallRva = RvaFromPreferredVa(0x0040104C);
constexpr uintptr_t kResetPatchRva = RvaFromPreferredVa(0x004151A6);
constexpr uintptr_t kResetCallRva = RvaFromPreferredVa(0x004151AC);
constexpr uintptr_t kGameDevicePointerRva = RvaFromPreferredVa(0x008A0E30);
constexpr uintptr_t kPresentParametersRva = RvaFromPreferredVa(0x008A0F68);
static_assert(kSokuSetupCallRva == 0x003FB871, "Unexpected Soku setup RVA");
static_assert(kEndSceneCallRva == 0x0000104C, "Unexpected EndScene RVA");
static_assert(kResetPatchRva == 0x000151A6, "Unexpected Reset patch RVA");
static_assert(kResetCallRva == 0x000151AC, "Unexpected Reset CALL RVA");
constexpr uintptr_t kEndSceneHookReturnValue = 0x008A0E14;
constexpr size_t kResetVtableIndex = 16;
constexpr size_t kEndSceneVtableIndex = 42;
constexpr size_t kDeviceVtableEntries = 119;
constexpr size_t kMaxPatchBytes = 16;

using SetupFunction = bool(__thiscall*)(void**, HWND*);
using ResetFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using EndSceneFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);

struct CallHookState
{
    uintptr_t patchBase = 0;
    uintptr_t callSite = 0;
    size_t patchSize = 0;
    size_t callOffset = 0;
    bool injectedPattern = false;
    bool installed = false;
    uintptr_t originalTarget = 0;
    uintptr_t hookTarget = 0;
    uint8_t originalBytes[kMaxPatchBytes]{};
    uint8_t patchedBytes[kMaxPatchBytes]{};
};

std::atomic<HWND> g_targetWindow{ nullptr };
std::atomic<OIOverlayImGui::RenderCallback> g_renderCallback{ nullptr };
std::atomic<SetupFunction> g_originalSokuSetup{ nullptr };
std::atomic<ResetFunction> g_originalReset{ nullptr };
std::atomic<EndSceneFunction> g_originalEndScene{ nullptr };
volatile LONG g_bootstrapInstalled = 0;
volatile LONG g_hookInstalled = 0;
volatile LONG g_stopping = 0;
volatile LONG g_activeHooks = 0;
uintptr_t g_setupCallSite = 0;
uintptr_t g_originalSetupTarget = 0;
CallHookState g_resetHook{};
CallHookState g_endSceneHook{};
// Serializes installation and restoration so shutdown cannot miss a hook that
// was being installed at the same time. A small Interlocked-based lock avoids
// requiring newer Windows synchronization APIs in this legacy game module.
volatile LONG g_hookMutationLock = 0;
IDirect3DDevice9* g_imguiDevice = nullptr;
ImFont* g_uiFont = nullptr;
// Keep the glyph ranges alive until ImGui builds its font atlas (ranges are not copied).
ImVector<ImWchar> g_uiGlyphRanges;
std::chrono::steady_clock::time_point g_lastFrame{};

class HookMutationGuard
{
public:
    HookMutationGuard()
    {
        while (InterlockedCompareExchange(&g_hookMutationLock, 1, 0) != 0)
            Sleep(0);
    }
    ~HookMutationGuard()
    {
        InterlockedExchange(&g_hookMutationLock, 0);
    }
    HookMutationGuard(const HookMutationGuard&) = delete;
    HookMutationGuard& operator=(const HookMutationGuard&) = delete;
};

// Hook definitions are declared before their addresses are placed in game code.
bool __fastcall HookedSokuSetup(void** dxWindowHandle, void* edx, HWND* windowOut);
HRESULT STDMETHODCALLTYPE HookedEndScene(IDirect3DDevice9* device);
HRESULT STDMETHODCALLTYPE HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters);

uintptr_t GameAddress(uintptr_t rva)
{
    const auto imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return imageBase ? imageBase + rva : 0;
}

bool IsReadableAddress(const void* address, size_t size)
{
    if (!address || size == 0)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    return begin >= regionBegin && size <= mbi.RegionSize && begin - regionBegin <= mbi.RegionSize - size;
}

bool IsExecutableAddress(const void* address)
{
    if (!address)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    switch (mbi.Protect & 0xFF) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool SafeReadMemory(uintptr_t address, void* output, size_t size)
{
    if (!address || !output || !size)
        return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
        output, size, &bytesRead) != FALSE && bytesRead == size;
}

bool TryReadGameDevice(IDirect3DDevice9** device)
{
    if (!device)
        return false;
    *device = nullptr;
    return SafeReadMemory(GameAddress(kGameDevicePointerRva), device, sizeof(*device));
}

bool SafeReadDeviceVtable(IDirect3DDevice9* device, void*** output)
{
    if (!device || !output || !IsReadableAddress(device, sizeof(void*)))
        return false;
    *output = nullptr;
    return SafeReadMemory(reinterpret_cast<uintptr_t>(device), output, sizeof(*output)) && *output;
}

HRESULT SafeGetCreationParameters(IDirect3DDevice9* device, D3DDEVICE_CREATION_PARAMETERS* parameters)
{
    // Keep SEH in a POD-only helper to avoid MSVC C2712 (SEH + C++ unwinding).
    __try {
        return device->GetCreationParameters(parameters);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_POINTER;
    }
}

bool IsSameProcessWindow(HWND window)
{
    if (!window || !IsWindow(window))
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}

bool ProbeDevice(IDirect3DDevice9* device, HWND requestedWindow, void** resetFallback, void** endSceneFallback)
{
    if (!device || !IsReadableAddress(device, sizeof(void*)))
        return false;

    void** vtable = nullptr;
    if (!SafeReadDeviceVtable(device, &vtable) ||
        !IsReadableAddress(vtable, sizeof(void*) * kDeviceVtableEntries))
        return false;

    void* resetAddress = nullptr;
    void* endSceneAddress = nullptr;
    if (!SafeReadMemory(reinterpret_cast<uintptr_t>(vtable) + kResetVtableIndex * sizeof(void*),
            &resetAddress, sizeof(resetAddress)) ||
        !SafeReadMemory(reinterpret_cast<uintptr_t>(vtable) + kEndSceneVtableIndex * sizeof(void*),
            &endSceneAddress, sizeof(endSceneAddress)) ||
        !IsExecutableAddress(resetAddress) || !IsExecutableAddress(endSceneAddress))
        return false;

    D3DDEVICE_CREATION_PARAMETERS parameters{};
    if (FAILED(SafeGetCreationParameters(device, &parameters)))
        return false;

    if (!IsSameProcessWindow(requestedWindow))
        return false;
    if (parameters.hFocusWindow) {
        if (!IsWindow(parameters.hFocusWindow))
            return false;
        DWORD focusPid = 0;
        GetWindowThreadProcessId(parameters.hFocusWindow, &focusPid);
        if (focusPid != GetCurrentProcessId())
            return false;
    }
    if (resetFallback)
        *resetFallback = resetAddress;
    if (endSceneFallback)
        *endSceneFallback = endSceneAddress;
    return true;
}

bool IsExecutableImageAddress(uintptr_t address)
{
    HMODULE exe = GetModuleHandleW(nullptr);
    MEMORY_BASIC_INFORMATION mbi{};
    return exe && address && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) &&
        mbi.State == MEM_COMMIT && mbi.AllocationBase == exe && IsExecutableAddress(reinterpret_cast<void*>(address));
}

bool WriteCodeBytes(uintptr_t address, const void* bytes, size_t size)
{
    if (!IsExecutableImageAddress(address) || !IsReadableAddress(reinterpret_cast<void*>(address), size))
        return false;
    DWORD oldProtection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;
    std::memcpy(reinterpret_cast<void*>(address), bytes, size);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    DWORD ignored = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, oldProtection, &ignored)) {
        // The bytes have already been written. Report the situation for diagnosis,
        // but do not tell the caller the patch was never applied (that would leave
        // a live branch into this DLL while the caller believes installation failed).
        OutputDebugStringW(L"[OIOverlay] Warning: code-page protection could not be restored after a patch.\n");
        DWORD retryOldProtection = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), size, oldProtection, &retryOldProtection))
            OutputDebugStringW(L"[OIOverlay] Warning: retry to restore code-page protection also failed.\n");
    }
    return true;
}

bool ReadCallTarget(uintptr_t callSite, uintptr_t& target)
{
    uint8_t opcode = 0;
    int32_t displacement = 0;
    if (!SafeReadMemory(callSite, &opcode, sizeof(opcode)) || opcode != 0xE8 ||
        !SafeReadMemory(callSite + 1, &displacement, sizeof(displacement)))
        return false;
    const int64_t signedTarget = static_cast<int64_t>(callSite + 5) + displacement;
    if (signedTarget <= 0)
        return false;
    target = static_cast<uintptr_t>(signedTarget);
    return IsExecutableAddress(reinterpret_cast<void*>(target));
}

bool MakeRel32(uintptr_t callSite, uintptr_t target, int32_t* displacement)
{
    if (!displacement)
        return false;
    const int64_t value = static_cast<int64_t>(target) - static_cast<int64_t>(callSite + 5);
    if (value < INT32_MIN || value > INT32_MAX)
        return false;
    *displacement = static_cast<int32_t>(value);
    return true;
}

bool PrepareCallHook(CallHookState& state, uintptr_t patchBase, uintptr_t callSite,
    size_t patchSize, size_t callOffset, uintptr_t hookTarget, uintptr_t fallbackTarget,
    const uint8_t* injectionTemplate)
{
    if (patchSize > kMaxPatchBytes || patchSize < 5 || callOffset + 5 > patchSize ||
        !IsExecutableImageAddress(patchBase) || !IsReadableAddress(reinterpret_cast<void*>(patchBase), patchSize) ||
        !IsExecutableImageAddress(callSite))
        return false;

    state = {};
    state.patchBase = patchBase;
    state.callSite = callSite;
    state.patchSize = patchSize;
    state.callOffset = callOffset;
    state.hookTarget = hookTarget;

    if (!SafeReadMemory(patchBase, state.originalBytes, patchSize))
        return false;

    const uint8_t opcode = state.originalBytes[callSite - patchBase];
    uintptr_t originalTarget = 0;
    if (opcode == 0xE8) {
        const size_t relOffset = static_cast<size_t>(callSite - patchBase) + 1;
        int32_t displacement = 0;
        std::memcpy(&displacement, state.originalBytes + relOffset, sizeof(displacement));
        const int64_t targetSigned = static_cast<int64_t>(callSite + 5) + displacement;
        if (targetSigned <= 0)
            return false;
        originalTarget = static_cast<uintptr_t>(targetSigned);
        if (!IsExecutableAddress(reinterpret_cast<void*>(originalTarget)) || originalTarget == hookTarget)
            return false;

        // Existing direct CALL sites are chained: the current destination becomes our original.
        state.patchSize = 5;
        state.patchBase = callSite;
        state.callOffset = 0;
        if (!SafeReadMemory(callSite, state.originalBytes, 5))
            return false;
        std::memcpy(state.patchedBytes, state.originalBytes, 5);
        int32_t newDisplacement = 0;
        if (!MakeRel32(callSite, hookTarget, &newDisplacement))
            return false;
        std::memcpy(state.patchedBytes + 1, &newDisplacement, sizeof(newDisplacement));
        state.injectedPattern = false;
    } else {
        // Only this exact th123 build can use the known instruction templates below.
        if (!injectionTemplate || !IsExecutableAddress(reinterpret_cast<void*>(fallbackTarget)))
            return false;
        originalTarget = fallbackTarget;
        std::memcpy(state.patchedBytes, injectionTemplate, patchSize);
        int32_t newDisplacement = 0;
        if (!MakeRel32(callSite, hookTarget, &newDisplacement))
            return false;
        std::memcpy(state.patchedBytes + callOffset + 1, &newDisplacement, sizeof(newDisplacement));
        state.injectedPattern = true;
    }
    state.originalTarget = originalTarget;
    return true;
}

bool ApplyCallHook(CallHookState& state)
{
    if (!state.patchBase || !state.patchSize || !state.originalTarget)
        return false;
    if (!WriteCodeBytes(state.patchBase, state.patchedBytes, state.patchSize))
        return false;
    state.installed = true;
    return true;
}

void RestoreCallHook(CallHookState& state)
{
    if (!state.installed || !state.callSite)
        return;
    uintptr_t currentTarget = 0;
    if (!ReadCallTarget(state.callSite, currentTarget) || currentTarget != state.hookTarget) {
        // Another component changed this call after us. Don't overwrite its hook.
        OutputDebugStringW(L"[OIOverlay] A DirectX call site changed after install; leaving it untouched on shutdown.\n");
        return;
    }

    bool restored = false;
    if (state.injectedPattern) {
        restored = WriteCodeBytes(state.patchBase, state.originalBytes, state.patchSize);
    } else {
        int32_t originalDisplacement = 0;
        std::memcpy(&originalDisplacement, state.originalBytes + 1, sizeof(originalDisplacement));
        restored = WriteCodeBytes(state.callSite + 1, &originalDisplacement, sizeof(originalDisplacement));
    }
    if (restored) {
        state.installed = false;
    } else {
        OutputDebugStringW(L"[OIOverlay] ERROR: failed to restore a DirectX call site.\n");
    }
}

bool InstallCallsiteHooks(IDirect3DDevice9* device, HWND window)
{
    HookMutationGuard mutationGuard;
    if (InterlockedCompareExchange(&g_stopping, 0, 0) != 0)
        return false;
    if (InterlockedCompareExchange(&g_hookInstalled, 0, 0) != 0)
        return true;

    void* resetFallback = nullptr;
    void* endSceneFallback = nullptr;
    if (!ProbeDevice(device, window, &resetFallback, &endSceneFallback)) {
        OutputDebugStringW(L"[OIOverlay] The D3D9 device did not pass validation; call-site hooks were not installed.\n");
        return false;
    }

    const uintptr_t endSceneCall = GameAddress(kEndSceneCallRva);
    const uintptr_t resetCall = GameAddress(kResetCallRva);
    const uintptr_t resetPatch = GameAddress(kResetPatchRva);
    const auto endSceneHook = reinterpret_cast<uintptr_t>(&HookedEndScene);
    const auto resetHook = reinterpret_cast<uintptr_t>(&HookedReset);

    // Exact instruction templates used by InGameHostlist for the same th123 build.
    const uint8_t endSceneTemplate[7] = { 0xE8, 0, 0, 0, 0, 0x50, 0x90 };
    uint8_t resetTemplate[13] = { 0x68, 0, 0, 0, 0, 0x50, 0xE8, 0, 0, 0, 0, 0x90, 0x90 };
    const uintptr_t presentParams = GameAddress(kPresentParametersRva);
    if (presentParams > UINT32_MAX) {
        OutputDebugStringW(L"[OIOverlay] D3D9 present-parameters address is out of x86 range.\n");
        return false;
    }
    const uint32_t presentParams32 = static_cast<uint32_t>(presentParams);
    std::memcpy(resetTemplate + 1, &presentParams32, sizeof(presentParams32));

    CallHookState resetCandidate{};
    CallHookState endSceneCandidate{};

    // If Reset's callsite is already E8, redirect just that CALL. Otherwise insert
    // the same 13-byte call sequence used by InGameHostlist, preserving old bytes.
    uint8_t resetOpcode = 0;
    if (!SafeReadMemory(resetCall, &resetOpcode, sizeof(resetOpcode)))
        return false;
    const bool resetAlreadyCall = resetOpcode == 0xE8;
    if (!PrepareCallHook(resetCandidate,
            resetAlreadyCall ? resetCall : resetPatch,
            resetCall,
            resetAlreadyCall ? 5 : 13,
            resetAlreadyCall ? 0 : 6,
            resetHook, reinterpret_cast<uintptr_t>(resetFallback),
            resetAlreadyCall ? nullptr : resetTemplate)) {
        OutputDebugStringW(L"[OIOverlay] Reset call signature check failed; no DirectX hooks installed.\n");
        return false;
    }

    uint8_t endSceneOpcode = 0;
    if (!SafeReadMemory(endSceneCall, &endSceneOpcode, sizeof(endSceneOpcode)))
        return false;
    const bool endSceneAlreadyCall = endSceneOpcode == 0xE8;
    if (!PrepareCallHook(endSceneCandidate,
            endSceneCall, endSceneCall,
            endSceneAlreadyCall ? 5 : 7, 0,
            endSceneHook, reinterpret_cast<uintptr_t>(endSceneFallback),
            endSceneAlreadyCall ? nullptr : endSceneTemplate)) {
        OutputDebugStringW(L"[OIOverlay] EndScene call signature check failed; no DirectX hooks installed.\n");
        return false;
    }

    if (InterlockedCompareExchange(&g_stopping, 0, 0) != 0)
        return false;

    // Publish original call targets before any patched site can execute.
    g_originalReset.store(reinterpret_cast<ResetFunction>(resetCandidate.originalTarget));
    g_originalEndScene.store(reinterpret_cast<EndSceneFunction>(endSceneCandidate.originalTarget));

    if (!ApplyCallHook(resetCandidate)) {
        OutputDebugStringW(L"[OIOverlay] Failed to patch Reset call site.\n");
        // Keep the saved function pointers valid in case another thread entered a
        // just-patched hook while rollback was being performed.
        return false;
    }
    if (InterlockedCompareExchange(&g_stopping, 0, 0) != 0) {
        RestoreCallHook(resetCandidate);
        return false;
    }
    if (!ApplyCallHook(endSceneCandidate)) {
        RestoreCallHook(resetCandidate);
        OutputDebugStringW(L"[OIOverlay] Failed to patch EndScene call site; Reset hook rolled back.\n");
        // Keep the saved function pointers valid in case another thread entered a
        // just-patched hook while rollback was being performed.
        return false;
    }
    if (InterlockedCompareExchange(&g_stopping, 0, 0) != 0) {
        RestoreCallHook(endSceneCandidate);
        RestoreCallHook(resetCandidate);
        return false;
    }

    g_resetHook = resetCandidate;
    g_endSceneHook = endSceneCandidate;
    g_targetWindow.store(window);
    InterlockedExchange(&g_hookInstalled, 1);
    OutputDebugStringW(L"[OIOverlay] DirectX call-site hooks installed after Soku setup (InGameHostlist-style).\n");
    return true;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        &result[0], required, nullptr, nullptr) != required)
        return {};
    return result;
}

std::string WindowsFontPathUtf8(const wchar_t* fileName)
{
    wchar_t windowsDirectory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    std::wstring path(windowsDirectory, length);
    path += L"\\Fonts\\";
    path += fileName;
    return WideToUtf8(path);
}

bool InitializeImGuiForDevice(IDirect3DDevice9* device)
{
    if (!device)
        return false;

    IMGUI_CHECKVERSION();
    if (!ImGui::GetCurrentContext())
        ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags = 0;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    const std::string gothicPath = WindowsFontPathUtf8(L"msgothic.ttc");
    const std::string meiryoPath = WindowsFontPathUtf8(L"meiryo.ttc");

    // The built-in Japanese range is curated and may omit uncommon kanji used
    // in game character names. U+8403 is 萃; include it explicitly so ImGui
    // does not substitute '?' even though the source string is correct.
    ImFontGlyphRangesBuilder rangeBuilder;
    rangeBuilder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
    rangeBuilder.AddChar(static_cast<ImWchar>(0x8403));
    rangeBuilder.BuildRanges(&g_uiGlyphRanges);

    ImFontConfig fontConfig{};
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 2;
    if (!gothicPath.empty())
        g_uiFont = io.Fonts->AddFontFromFileTTF(gothicPath.c_str(), 16.0f, &fontConfig, g_uiGlyphRanges.Data);
    if (!g_uiFont && !meiryoPath.empty())
        g_uiFont = io.Fonts->AddFontFromFileTTF(meiryoPath.c_str(), 16.0f, &fontConfig, g_uiGlyphRanges.Data);
    if (!g_uiFont)
        g_uiFont = io.Fonts->AddFontDefault();
    io.FontDefault = g_uiFont;

    if (!ImGui_ImplDX9_Init(device)) {
        ImGui::DestroyContext();
        g_uiFont = nullptr;
        return false;
    }

    g_imguiDevice = device;
    g_lastFrame = std::chrono::steady_clock::now();
    OutputDebugStringW(L"[OIOverlay] ImGui DX9 backend initialized from EndScene.\n");
    return true;
}

void ShutdownImGuiContext()
{
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX9_Shutdown();
        ImGui::DestroyContext();
    }
    g_imguiDevice = nullptr;
    g_uiFont = nullptr;
    g_lastFrame = {};
}

void RenderOneFrame(IDirect3DDevice9* device)
{
    HWND targetWindow = g_targetWindow.load();
    if (InterlockedCompareExchange(&g_stopping, 0, 0) != 0 ||
        !device || !targetWindow || !IsWindow(targetWindow))
        return;

    if (g_imguiDevice != device) {
        // A different device is not silently rebound: doing so could release a stale backend.
        if (g_imguiDevice) {
            OutputDebugStringW(L"[OIOverlay] Render device changed; skipping frame to avoid using stale ImGui resources.\n");
            return;
        }
        if (!InitializeImGuiForDevice(device))
            return;
    }

    D3DVIEWPORT9 viewport{};
    if (FAILED(device->GetViewport(&viewport)) || viewport.Width == 0 || viewport.Height == 0)
        return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(viewport.Width), static_cast<float>(viewport.Height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    const auto now = std::chrono::steady_clock::now();
    float delta = std::chrono::duration<float>(now - g_lastFrame).count();
    if (!(delta > 0.0f) || delta > 0.25f)
        delta = 1.0f / 60.0f;
    io.DeltaTime = delta;
    g_lastFrame = now;

    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    const auto callback = g_renderCallback.load();
    if (callback) {
        __try {
            callback(static_cast<int>(viewport.Width), static_cast<int>(viewport.Height));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            OutputDebugStringW(L"[OIOverlay] Exception in UI callback; frame kept balanced.\n");
        }
    }
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

HRESULT STDMETHODCALLTYPE HookedEndScene(IDirect3DDevice9* device)
{
    InterlockedIncrement(&g_activeHooks);
    const auto original = g_originalEndScene.load();
    if (InterlockedCompareExchange(&g_stopping, 0, 0) == 0 && original)
        RenderOneFrame(device);
    if (original)
        (void)original(device);
    InterlockedDecrement(&g_activeHooks);

    // This is not a normal standalone COM vtable detour. InGameHostlist patches
    // this exact game call site and returns 0x008A0E14 from its wrapper because
    // the surrounding th123 instruction sequence expects that value in EAX.
    // Preserve that game-call-site contract rather than returning EndScene's HRESULT.
    return static_cast<HRESULT>(kEndSceneHookReturnValue);
}

HRESULT STDMETHODCALLTYPE HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
{
    InterlockedIncrement(&g_activeHooks);
    const auto original = g_originalReset.load();
    const bool hasContext = device && device == g_imguiDevice && ImGui::GetCurrentContext();
    if (hasContext && InterlockedCompareExchange(&g_stopping, 0, 0) == 0)
        ImGui_ImplDX9_InvalidateDeviceObjects();

    const HRESULT result = original ? original(device, parameters) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(result) && hasContext && InterlockedCompareExchange(&g_stopping, 0, 0) == 0)
        ImGui_ImplDX9_CreateDeviceObjects();

    InterlockedDecrement(&g_activeHooks);
    return result;
}

void RestoreSetupCallHook()
{
    if (InterlockedCompareExchange(&g_bootstrapInstalled, 0, 0) == 0 || !g_setupCallSite)
        return;

    uintptr_t currentTarget = 0;
    if (!ReadCallTarget(g_setupCallSite, currentTarget) ||
        currentTarget != reinterpret_cast<uintptr_t>(&HookedSokuSetup)) {
        OutputDebugStringW(L"[OIOverlay] Setup call was changed by another component; not overwriting it on shutdown.\n");
        return;
    }

    int32_t originalDisplacement = 0;
    if (!MakeRel32(g_setupCallSite, g_originalSetupTarget, &originalDisplacement))
        return;
    if (WriteCodeBytes(g_setupCallSite + 1, &originalDisplacement, sizeof(originalDisplacement)))
        InterlockedExchange(&g_bootstrapInstalled, 0);
}

bool __fastcall HookedSokuSetup(void** dxWindowHandle, void* /*edx*/, HWND* windowOut)
{
    InterlockedIncrement(&g_activeHooks);
    const auto original = g_originalSokuSetup.load();
    if (!original) {
        InterlockedDecrement(&g_activeHooks);
        return false;
    }

    // Follow InGameHostlist's ordering: original setup returns before touching the D3D device.
    const bool result = original(dxWindowHandle, windowOut);
    HWND window = (result && windowOut) ? *windowOut : nullptr;
    if (result && InterlockedCompareExchange(&g_stopping, 0, 0) == 0 && IsSameProcessWindow(window)) {
        g_targetWindow.store(window);
        IDirect3DDevice9* device = nullptr;
        if (TryReadGameDevice(&device) && device)
            InstallCallsiteHooks(device, window);
        else
            OutputDebugStringW(L"[OIOverlay] Soku setup returned without a readable D3D9 device.\n");
    }
    InterlockedDecrement(&g_activeHooks);
    return result;
}

} // namespace

namespace OIOverlayImGui
{
bool InstallBootstrap(RenderCallback callback)
{
    HookMutationGuard mutationGuard;
    g_renderCallback.store(callback);
    if (!callback || InterlockedCompareExchange(&g_stopping, 0, 0) != 0)
        return false;
    if (InterlockedCompareExchange(&g_bootstrapInstalled, 0, 0) != 0)
        return true;

    const uintptr_t callSite = GameAddress(kSokuSetupCallRva);
    if (!callSite || !IsExecutableImageAddress(callSite)) {
        OutputDebugStringW(L"[OIOverlay] Soku setup call is outside the expected executable image.\n");
        return false;
    }

    uintptr_t originalTarget = 0;
    if (!ReadCallTarget(callSite, originalTarget)) {
        OutputDebugStringW(L"[OIOverlay] Soku setup CALL signature mismatch; refusing to patch.\n");
        return false;
    }
    if (originalTarget == reinterpret_cast<uintptr_t>(&HookedSokuSetup))
        return false;

    int32_t newDisplacement = 0;
    if (!MakeRel32(callSite, reinterpret_cast<uintptr_t>(&HookedSokuSetup), &newDisplacement))
        return false;

    g_setupCallSite = callSite;
    g_originalSetupTarget = originalTarget;
    g_originalSokuSetup.store(reinterpret_cast<SetupFunction>(originalTarget));

    if (!WriteCodeBytes(callSite + 1, &newDisplacement, sizeof(newDisplacement))) {
        g_originalSokuSetup.store(nullptr);
        g_setupCallSite = 0;
        g_originalSetupTarget = 0;
        OutputDebugStringW(L"[OIOverlay] Could not patch Soku setup CALL.\n");
        return false;
    }
    InterlockedExchange(&g_bootstrapInstalled, 1);
    OutputDebugStringW(L"[OIOverlay] Soku setup bootstrap hook installed; waiting for original setup to return.\n");
    return true;
}

void SetTargetWindow(HWND targetWindow)
{
    if (IsSameProcessWindow(targetWindow))
        g_targetWindow.store(targetWindow);
}

bool IsHookInstalled()
{
    return InterlockedCompareExchange(&g_hookInstalled, 0, 0) != 0;
}

void Shutdown()
{
    if (InterlockedExchange(&g_stopping, 1) == 1)
        return;

    {
        HookMutationGuard mutationGuard;
        RestoreCallHook(g_endSceneHook);
        RestoreCallHook(g_resetHook);
        InterlockedExchange(&g_hookInstalled, 0);
        RestoreSetupCallHook();
    }

    while (InterlockedCompareExchange(&g_activeHooks, 0, 0) != 0)
        Sleep(1);
    ShutdownImGuiContext();
    g_targetWindow.store(nullptr);
    g_renderCallback.store(nullptr);
    g_originalEndScene.store(nullptr);
    g_originalReset.store(nullptr);
    g_originalSokuSetup.store(nullptr);
    OutputDebugStringW(L"[OIOverlay] DirectX call-site hooks shut down.\n");
}
} // namespace OIOverlayImGui
