#pragma once

#include <Windows.h>
#include <string>

struct OverlayConfig
{
    std::wstring tskDbPath;
    std::wstring playersDbPath;
    bool defaultVisible = true;
    UINT toggleKey = VK_F10;
    std::wstring iniPath;
};

// Reads OIOverlay.ini from the directory containing the overlay DLL.
OverlayConfig LoadOverlayConfig(HMODULE module);
