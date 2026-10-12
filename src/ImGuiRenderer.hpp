#pragma once

#include <Windows.h>

namespace OIOverlayImGui
{
    // Install a version-checked one-call hook at the game's Direct3D setup call.
    // The original setup routine is called first; only after it returns do we
    // inspect the device and install its per-device EndScene/Reset hooks.
    // This avoids probing a not-yet-created device from a background thread.
    using RenderCallback = void (*)(int displayWidth, int displayHeight);

    bool InstallBootstrap(RenderCallback callback);
    void SetTargetWindow(HWND targetWindow);
    bool IsHookInstalled();
    void Shutdown();
}
