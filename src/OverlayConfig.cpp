#include "OverlayConfig.hpp"

#include <cstdlib>
#include <cwchar>

namespace
{
constexpr wchar_t kDefaultTskDbPath[] =
    L"C:\\Program Files (x86)\\tasofro\\th123\\th4_5888\\Default.db";
constexpr wchar_t kDefaultPlayersDbPath[] =
    L"C:\\Program Files (x86)\\tasofro\\th123\\th4_5888\\tsk_OIViewer\\CombinedPlayers.db";

std::wstring GetIniPath(HMODULE module)
{
    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(module, modulePath, static_cast<DWORD>(_countof(modulePath)));
    if (length == 0 || length >= _countof(modulePath))
        return L"OIOverlay.ini";

    std::wstring path(modulePath, length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L"OIOverlay.ini";
    return path.substr(0, slash + 1) + L"OIOverlay.ini";
}

void ReadString(
    const wchar_t* section,
    const wchar_t* key,
    const wchar_t* defaultValue,
    const std::wstring& iniPath,
    std::wstring& output)
{
    wchar_t buffer[32768]{};
    GetPrivateProfileStringW(
        section, key, defaultValue, buffer,
        static_cast<DWORD>(_countof(buffer)), iniPath.c_str());
    output.assign(buffer);
}
}

OverlayConfig LoadOverlayConfig(HMODULE module)
{
    OverlayConfig config;
    config.iniPath = GetIniPath(module);

    ReadString(L"Paths", L"TskDb", kDefaultTskDbPath, config.iniPath, config.tskDbPath);
    ReadString(L"Paths", L"PlayersDb", kDefaultPlayersDbPath, config.iniPath, config.playersDbPath);

    std::wstring visible;
    ReadString(L"Overlay", L"DefaultVisible", L"1", config.iniPath, visible);
    wchar_t* visibleEnd = nullptr;
    const long visibleValue = std::wcstol(visible.c_str(), &visibleEnd, 10);
    config.defaultVisible = (visibleEnd == visible.c_str() || *visibleEnd != L'\0')
        ? true : visibleValue != 0;

    std::wstring key;
    ReadString(L"Overlay", L"ToggleKey", L"0x79", config.iniPath, key);
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(key.c_str(), &end, 0);
    if (key.empty() || end == key.c_str() || *end != L'\0' || value > 0xFFUL)
        config.toggleKey = VK_F10;
    else
        config.toggleKey = static_cast<UINT>(value); // 0 means disabled

    return config;
}
