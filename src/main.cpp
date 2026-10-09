// OIOverlay - OIViewer-style profile/record overlay for Touhou Hisoutensoku.
// The overlay is a transparent topmost window so it can also work on SceneID 2
// and can show the TenSokuKan warning even while BattleManager is not active.
// Default.db is opened READ ONLY. This module never writes to it.

#include <Windows.h>
#include <TlHelp32.h>
#include <SokuLib.hpp>
#include "TskDatabase.hpp"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr uintptr_t kPNetObjectAddress = 0x008986A0;
constexpr uintptr_t kSceneIdAddress = 0x008A0044;
constexpr uintptr_t kLeftProfileOffset = 0x04;
constexpr uintptr_t kRightProfileOffset = 0x24;
constexpr size_t kProfileSize = 0x20;
constexpr wchar_t kWindowClassName[] = L"OIOverlayModuleLayerWindow";
constexpr UINT kRefreshMessage = WM_APP + 9;

HMODULE g_module = nullptr;
HWND g_overlayWindow = nullptr;
HWND g_gameWindow = nullptr;
HHOOK g_keyboardHook = nullptr;
volatile LONG g_stopRequested = 0;
int g_sceneId = -1;
bool g_clientMode = false;
bool g_overlayEnabled = true;
bool g_previousF10Down = false;
bool g_tskRunning = false;
bool g_warningBlink = false;
bool g_inputEnabled = false;
bool g_suppressedKeys[256]{};
std::string g_opponentProfile;
std::string g_watchingP1Profile;
std::string g_watchingP2Profile;
std::string g_cachedNormalProfile;
std::string g_cachedWatchingP1;
std::string g_cachedWatchingP2;
std::string g_inputBuffer;
std::vector<std::wstring> g_infoLines;
std::wstring g_inputLine;
std::unique_ptr<TskDatabase> g_database;
std::wstring g_databasePath;
TskStats g_normalStats;
TskStats g_watchP1Stats;
TskStats g_watchP2Stats;
bool g_normalStatsValid = false;
bool g_watchP1StatsValid = false;
bool g_watchP2StatsValid = false;
std::uintmax_t g_dbSize = 0;
FILETIME g_dbWriteTime{};

using Clock = std::chrono::steady_clock;
Clock::time_point g_lastTskCheck{};
Clock::time_point g_lastDbCheck{};
Clock::time_point g_lastStatsRefresh{};
Clock::time_point g_lastWarningBlink{};

bool IsFileTimeEqual(const FILETIME& a, const FILETIME& b)
{
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

std::wstring GameDirectory()
{
    wchar_t fullPath[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, fullPath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    std::wstring path(fullPath, length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    return path.substr(0, slash);
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FindDefaultDatabase()
{
    const std::wstring root = GameDirectory();
    if (root.empty())
        return {};
    const std::wstring nested = root + L"\\th4_5888\\Default.db";
    if (FileExists(nested))
        return nested;
    const std::wstring rootDb = root + L"\\Default.db";
    if (FileExists(rootDb))
        return rootDb;
    return {};
}

bool ReadFileInfo(const std::wstring& path, std::uintmax_t& size, FILETIME& writeTime)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return false;
    ULARGE_INTEGER fileSize{};
    fileSize.LowPart = data.nFileSizeLow;
    fileSize.HighPart = data.nFileSizeHigh;
    size = static_cast<std::uintmax_t>(fileSize.QuadPart);
    writeTime = data.ftLastWriteTime;
    return true;
}

bool IsTskRunning()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"tsk.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

int ReadSceneId()
{
    return *reinterpret_cast<volatile int*>(kSceneIdAddress);
}

std::string ReadProfile(uintptr_t offset)
{
    const uint32_t networkObject = *reinterpret_cast<volatile uint32_t*>(kPNetObjectAddress);
    if (networkObject == 0)
        return {};
    const uintptr_t address = static_cast<uintptr_t>(networkObject) + offset;
    if (IsBadReadPtr(reinterpret_cast<const void*>(address), kProfileSize))
        return {};
    char buffer[kProfileSize + 1]{};
    std::memcpy(buffer, reinterpret_cast<const void*>(address), kProfileSize);
    buffer[kProfileSize] = '\0';
    std::string result(buffer);
    while (!result.empty() && (result.back() == '\0' || result.back() == ' ' || result.back() == '\t'))
        result.pop_back();
    return result;
}

std::wstring Cp932ToWide(const std::string& value)
{
    if (value.empty())
        return {};
    const int count = MultiByteToWideChar(932, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0)
        return L"";
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(932, 0, value.data(), static_cast<int>(value.size()), &result[0], count);
    return result;
}

std::wstring NarrowAsciiToWide(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}

std::wstring FormatDate(const std::string& value)
{
    // TskDatabase normalizes recognized timestamps to yyyy/MM/dd HH:mm:ss.
    return NarrowAsciiToWide(value);
}

std::wstring CharacterName(int id)
{
    switch (id) {
    case 0: return L"霊夢";
    case 1: return L"魔理沙";
    case 2: return L"咲夜";
    case 3: return L"アリス";
    case 4: return L"パチュリー";
    case 5: return L"妖夢";
    case 6: return L"レミリア";
    case 7: return L"幽々子";
    case 8: return L"紫";
    case 9: return L"萃香";
    case 10: return L"鈴仙";
    case 11: return L"文";
    case 12: return L"小町";
    case 13: return L"衣玖";
    case 14: return L"天子";
    case 15: return L"早苗";
    case 16: return L"チルノ";
    case 17: return L"美鈴";
    case 18: return L"空";
    case 19: return L"諏訪子";
    default: return L"---";
    }
}

std::wstring RateText(int wins, int losses)
{
    if (wins + losses <= 0)
        return L"---.-%";
    std::wostringstream out;
    out << std::fixed << std::setprecision(1)
        << (100.0 * wins / (wins + losses)) << L"%";
    return out.str();
}

std::wstring RecordLine(const wchar_t* label, int ourWins, int theirWins)
{
    std::wostringstream out;
    out << label << L"： "
        << std::setw(3) << ourWins << L"勝 "
        << std::setw(3) << theirWins << L"敗  ("
        << RateText(ourWins, theirWins) << L")";
    return out.str();
}

std::wstring WatchRateLine(const wchar_t* label, const TskStats& p1, const TskStats& p2)
{
    // 観戦では各プロフィール本人の勝率を表示する。
    // DBのP2側プロフィールの勝ち数が totalWins。
    auto rateFor = [](const TskStats& stats) -> std::wstring {
        if (!stats.found)
            return L"---.-%";
        return RateText(stats.totalWins, stats.totalLosses);
    };
    std::wstring a = rateFor(p1);
    std::wstring b = rateFor(p2);
    if (a.size() < 6) a.insert(a.begin(), 6 - a.size(), L' ');
    if (b.size() < 6) b.insert(b.begin(), 6 - b.size(), L' ');
    return a + L" /" + b + L" ： " + label;
}

std::wstring WatchingPlayerText(const wchar_t* side, const std::string& profile, const TskStats& stats, bool statsRead)
{
    std::wstring text = std::wstring(side) + L" : " + Cp932ToWide(profile) + L"\n";
    if (!statsRead || !stats.found) {
        text += L"メインキャラ : ---\n通算 : 0戦";
        return text;
    }
    text += L"メインキャラ : " + CharacterName(stats.mainCharacterId) + L"\n";
    text += L"通算 : " + std::to_wstring(stats.totalMatches) + L"戦";
    if (stats.hasUnrecordedWinningRound)
        text += L"\n[ラウンド未取得]";
    return text;
}

std::wstring BuildNormalInfo()
{
    if (g_opponentProfile.empty())
        return L"【キャラセレ中】\n\n対戦相手を取得中...";

    std::wstring text = L"【キャラセレ中】\n\n対戦相手 : " + Cp932ToWide(g_opponentProfile) + L"\n";
    if (!g_normalStatsValid || !g_normalStats.found) {
        text += L"\n対戦記録がありません";
        return text;
    }

    const TskStats& s = g_normalStats;
    text += L"メインキャラ : " + CharacterName(s.mainCharacterId) + L"\n";
    text += L"通算 : " + std::to_wstring(s.totalMatches) + L"戦\n";
    text += L"通算勝率 : " + RateText(s.totalLosses, s.totalWins) + L"\n\n";
    text += RecordLine(L"過去 30戦", s.last30.losses, s.last30.wins) + L"\n";
    text += RecordLine(L"過去100戦", s.last100.losses, s.last100.wins) + L"\n";
    text += RecordLine(L"過去1か月", s.lastMonth.losses, s.lastMonth.wins) + L"\n";
    if (s.hasUnrecordedWinningRound)
        text += L"[ラウンド未取得]\n";
    text += L"\n";
    if (!s.firstMatchDate.empty())
        text += L"初対戦　　　 : " + FormatDate(s.firstMatchDate) + L"\n";
    if (!s.lastMatchDateBeforeToday.empty())
        text += L"前回対戦　　 : " + FormatDate(s.lastMatchDateBeforeToday) + L"\n";
    if (!s.lastLossDate.empty())
        text += L"最後に勝った : " + FormatDate(s.lastLossDate) + L"\n";
    if (!s.lastWinDate.empty())
        text += L"最後に負けた : " + FormatDate(s.lastWinDate);
    return text;
}

std::wstring BuildWatchingInfo()
{
    std::wstring text = L"【観戦中】\n\n";
    if (g_watchingP1Profile.empty() || g_watchingP2Profile.empty())
        return text + L"1P / 2Pのプロファイルを取得中...";

    text += WatchingPlayerText(L"1P", g_watchingP1Profile, g_watchP1Stats, g_watchP1StatsValid);
    text += L"\n\n";
    text += WatchingPlayerText(L"2P", g_watchingP2Profile, g_watchP2Stats, g_watchP2StatsValid);
    text += L"\n\n";
    text += WatchRateLine(L"通算", g_watchP1Stats, g_watchP2Stats) + L"\n";
    // OIViewer's spectator view shows a profile's stored P2-perspective record rate.
    auto recentRate = [](const TskStats& s, int period) -> std::wstring {
        if (!s.found) return L"---.-%";
        const TskPeriodStats* p = period == 30 ? &s.last30 : period == 100 ? &s.last100 : &s.lastMonth;
        return RateText(p->wins, p->losses);
    };
    std::wstring r1 = recentRate(g_watchP1Stats, 30), r2 = recentRate(g_watchP2Stats, 30);
    if (r1.size() < 6) r1.insert(r1.begin(), 6 - r1.size(), L' ');
    if (r2.size() < 6) r2.insert(r2.begin(), 6 - r2.size(), L' ');
    text += r1 + L" /" + r2 + L" ： 過去 30戦\n";
    r1 = recentRate(g_watchP1Stats, 100); r2 = recentRate(g_watchP2Stats, 100);
    if (r1.size() < 6) r1.insert(r1.begin(), 6 - r1.size(), L' ');
    if (r2.size() < 6) r2.insert(r2.begin(), 6 - r2.size(), L' ');
    text += r1 + L" /" + r2 + L" ： 過去100戦\n";
    r1 = recentRate(g_watchP1Stats, -1); r2 = recentRate(g_watchP2Stats, -1);
    if (r1.size() < 6) r1.insert(r1.begin(), 6 - r1.size(), L' ');
    if (r2.size() < 6) r2.insert(r2.begin(), 6 - r2.size(), L' ');
    text += r1 + L" /" + r2 + L" ： 過去1か月";
    return text;
}

void SplitLines(const std::wstring& text, std::vector<std::wstring>& out)
{
    out.clear();
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        out.push_back(std::move(line));
        if (end == text.size()) break;
        start = end + 1;
    }
}

void SetClipboardText(const std::string& text)
{
    if (text.empty())
        return;
    const int chars = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (chars <= 0)
        return;
    std::wstring wide(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, &wide[0], chars);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, wide.size() * sizeof(wchar_t));
    if (!memory)
        return;
    void* target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        return;
    }
    std::memcpy(target, wide.c_str(), wide.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (!OpenClipboard(g_gameWindow)) {
        GlobalFree(memory);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory))
        GlobalFree(memory);
    CloseClipboard();
}

bool IsGameForeground()
{
    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

void AppendIpCharacter(char character)
{
    constexpr size_t kMaximumInputLength = 64;
    if (g_inputBuffer.size() < kMaximumInputLength)
        g_inputBuffer.push_back(character);
}

bool PasteClipboardIp()
{
    if (!OpenClipboard(g_overlayWindow))
        return false;

    HANDLE clipboardData = GetClipboardData(CF_UNICODETEXT);
    if (!clipboardData) {
        CloseClipboard();
        return false;
    }

    const wchar_t* source = static_cast<const wchar_t*>(GlobalLock(clipboardData));
    if (!source) {
        CloseClipboard();
        return false;
    }

    std::wstring value(source);
    GlobalUnlock(clipboardData);
    CloseClipboard();

    // IP:Port欄への貼り付けは数字・ピリオド・コロンだけを受け付ける。
    size_t first = 0;
    while (first < value.size() && iswspace(value[first])) ++first;
    size_t last = value.size();
    while (last > first && iswspace(value[last - 1])) --last;
    if (first == last || last - first > 64)
        return false;

    std::string normalized;
    normalized.reserve(last - first);
    for (size_t i = first; i < last; ++i) {
        const wchar_t c = value[i];
        if ((c >= L'0' && c <= L'9') || c == L'.' || c == L':')
            normalized.push_back(static_cast<char>(c));
        else
            return false;
    }

    constexpr size_t kMaximumInputLength = 64;
    if (g_inputBuffer.size() + normalized.size() > kMaximumInputLength)
        return false;
    g_inputBuffer += normalized;
    return true;
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM message, LPARAM data)
{
    if (code < 0 || data == 0)
        return CallNextHookEx(g_keyboardHook, code, message, data);

    const auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
    const DWORD vk = key->vkCode;
    const bool isDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool isUp = message == WM_KEYUP || message == WM_SYSKEYUP;

    // For keys consumed during input, consume key-up as well.
    if (isUp && vk < _countof(g_suppressedKeys) && g_suppressedKeys[vk]) {
        g_suppressedKeys[vk] = false;
        return 1;
    }

    if (!isDown || !g_inputEnabled || g_sceneId != 2 || !IsGameForeground())
        return CallNextHookEx(g_keyboardHook, code, message, data);

    bool consume = true;
    if (vk == VK_RETURN) {
        SetClipboardText(g_inputBuffer);
        g_inputBuffer.clear();
    } else if (vk == VK_BACK) {
        if (!g_inputBuffer.empty())
            g_inputBuffer.pop_back();
    } else if (vk == 'V' && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
        PasteClipboardIp();
    } else if (vk >= '0' && vk <= '9') {
        AppendIpCharacter(static_cast<char>(vk));
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        AppendIpCharacter(static_cast<char>('0' + (vk - VK_NUMPAD0)));
    } else if (vk == VK_OEM_PERIOD || vk == VK_DECIMAL) {
        AppendIpCharacter('.');
    } else if ((vk == VK_OEM_1 || vk == VK_OEM_PLUS) &&
               (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
        // US layout uses Shift+OEM_1; Japanese layout may use Shift+OEM_PLUS.
        AppendIpCharacter(':');
    } else {
        consume = false;
    }

    if (!consume)
        return CallNextHookEx(g_keyboardHook, code, message, data);

    if (vk < _countof(g_suppressedKeys))
        g_suppressedKeys[vk] = true;
    if (g_overlayWindow)
        InvalidateRect(g_overlayWindow, nullptr, FALSE);
    return 1; // Avoid letting the network menu react to text-entry keys.
}

BOOL CALLBACK FindGameWindowCallback(HWND window, LPARAM parameter)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window))
        return TRUE;
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(_countof(className)));
    if (wcscmp(className, kWindowClassName) == 0)
        return TRUE;
    if (GetWindow(window, GW_OWNER) != nullptr)
        return TRUE;
    RECT rect{};
    if (!GetWindowRect(window, &rect))
        return TRUE;
    const long long area = static_cast<long long>(rect.right - rect.left) * (rect.bottom - rect.top);
    HWND* found = reinterpret_cast<HWND*>(parameter);
    RECT previous{};
    long long previousArea = 0;
    if (*found && GetWindowRect(*found, &previous))
        previousArea = static_cast<long long>(previous.right - previous.left) * (previous.bottom - previous.top);
    if (area > previousArea)
        *found = window;
    return TRUE;
}

HWND FindGameWindow()
{
    HWND result = nullptr;
    EnumWindows(FindGameWindowCallback, reinterpret_cast<LPARAM>(&result));
    return result;
}

void RefreshDatabase()
{
    const std::wstring path = FindDefaultDatabase();
    if (path != g_databasePath) {
        g_databasePath = path;
        if (path.empty())
            g_database.reset();
        else
            g_database = std::make_unique<TskDatabase>(path);
        g_normalStatsValid = g_watchP1StatsValid = g_watchP2StatsValid = false;
        g_cachedNormalProfile.clear();
        g_cachedWatchingP1.clear();
        g_cachedWatchingP2.clear();
    }
}

bool DatabaseChanged()
{
    if (g_databasePath.empty())
        return true;
    std::uintmax_t size = 0;
    FILETIME writeTime{};
    if (!ReadFileInfo(g_databasePath, size, writeTime))
        return true;
    const bool changed = size != g_dbSize || !IsFileTimeEqual(writeTime, g_dbWriteTime);
    if (changed) {
        g_dbSize = size;
        g_dbWriteTime = writeTime;
    }
    return changed;
}

void RefreshStats(bool force)
{
    if (!force && Clock::now() - g_lastStatsRefresh < std::chrono::seconds(1))
        return;
    g_lastStatsRefresh = Clock::now();

    if (!g_database || !g_database->IsAvailable()) {
        g_normalStats = {};
        g_watchP1Stats = {};
        g_watchP2Stats = {};
        g_normalStatsValid = g_watchP1StatsValid = g_watchP2StatsValid = false;
        // Cache profiles even on a missing DB, avoiding a tight retry loop.
        g_cachedNormalProfile = g_opponentProfile;
        g_cachedWatchingP1 = g_watchingP1Profile;
        g_cachedWatchingP2 = g_watchingP2Profile;
        return;
    }

    if (force || g_cachedNormalProfile != g_opponentProfile) {
        g_normalStats = {};
        g_normalStatsValid = !g_opponentProfile.empty() &&
            g_database->GetOpponentStats(g_opponentProfile, g_normalStats);
        g_cachedNormalProfile = g_opponentProfile;
    }
    if (force || g_cachedWatchingP1 != g_watchingP1Profile) {
        g_watchP1Stats = {};
        g_watchP1StatsValid = !g_watchingP1Profile.empty() &&
            g_database->GetOpponentStats(g_watchingP1Profile, g_watchP1Stats);
        g_cachedWatchingP1 = g_watchingP1Profile;
    }
    if (force || g_cachedWatchingP2 != g_watchingP2Profile) {
        g_watchP2Stats = {};
        g_watchP2StatsValid = !g_watchingP2Profile.empty() &&
            g_database->GetOpponentStats(g_watchingP2Profile, g_watchP2Stats);
        g_cachedWatchingP2 = g_watchingP2Profile;
    }
}

void UpdateSceneState()
{
    const int scene = ReadSceneId();
    const bool changed = scene != g_sceneId;
    if (changed) {
        const int oldScene = g_sceneId;
        g_sceneId = scene;
        if (scene <= 7)
            g_clientMode = false;
        if (scene != 2) {
            // The input field is strictly a SceneID 2 feature.
            g_inputBuffer.clear();
            g_inputEnabled = false;
        } else {
            g_inputBuffer.clear();
            g_inputEnabled = true;
        }
        if (scene == 9)
            g_clientMode = true;
        if (oldScene == 2 && scene != 2)
            g_inputBuffer.clear();
    }
    if (scene <= 7) {
        g_clientMode = false;
    } else if (scene == 9) {
        g_clientMode = true;
    }

    if (scene == 12 || scene == 15) {
        const std::string p1 = ReadProfile(kLeftProfileOffset);
        const std::string p2 = ReadProfile(kRightProfileOffset);
        if (p1 != g_watchingP1Profile) {
            g_watchingP1Profile = p1;
            g_watchP1Stats = {};
            g_watchP1StatsValid = false;
        }
        if (p2 != g_watchingP2Profile) {
            g_watchingP2Profile = p2;
            g_watchP2Stats = {};
            g_watchP2StatsValid = false;
        }
    } else if (scene >= 8 && scene <= 14) {
        const uintptr_t offset = g_clientMode ? kLeftProfileOffset : kRightProfileOffset;
        const std::string opponent = ReadProfile(offset);
        if (!opponent.empty() && opponent != g_opponentProfile) {
            g_opponentProfile = opponent;
            g_normalStats = {};
            g_normalStatsValid = false;
        }
    }
}

void UpdateInfoText()
{
    if (!g_overlayEnabled) {
        g_infoLines.clear();
        return;
    }
    if (g_sceneId >= 8 && g_sceneId <= 11) {
        SplitLines(BuildNormalInfo(), g_infoLines);
        return;
    }
    if (g_sceneId == 12 || g_sceneId == 15) {
        SplitLines(BuildWatchingInfo(), g_infoLines);
        return;
    }
    g_infoLines.clear();
}

void UpdateInputText()
{
    if (!g_inputEnabled || g_sceneId != 2) {
        g_inputLine.clear();
        return;
    }
    g_inputLine = L"IP:Port ＞ " + NarrowAsciiToWide(g_inputBuffer) + L"_";
}

void DrawTextLine(HDC dc, const std::wstring& line, int x, int y, int width, int height, COLORREF color, HFONT font)
{
    SetTextColor(dc, color);
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT r{ x, y, x + width, y + height };
    DrawTextW(dc, line.c_str(), static_cast<int>(line.size()), &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void DrawPanel(HDC dc, const RECT& rect, COLORREF borderColor)
{
    HBRUSH background = CreateSolidBrush(RGB(12, 12, 18));
    FillRect(dc, &rect, background);
    DeleteObject(background);
    HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void PaintOverlay(HWND window)
{
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window, &ps);
    RECT client{};
    GetClientRect(window, &client);
    HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &client, clearBrush); // RGB(0,0,0) is the transparency key.
    DeleteObject(clearBrush);
    SetBkMode(dc, TRANSPARENT);

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    if (!g_infoLines.empty() && g_overlayEnabled) {
        const int panelWidth = std::max(200, std::min(width - 16, 650));
        const int lineHeight = 15;
        const int visibleLines = std::min(static_cast<int>(g_infoLines.size()), std::max(1, (height - 40) / lineHeight));
        const int panelHeight = visibleLines * lineHeight + 12;
        RECT panel{ 8, 8, 8 + panelWidth, 8 + panelHeight };
        DrawPanel(dc, panel, RGB(100, 130, 165));
        HFONT font = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
        for (int i = 0; i < visibleLines; ++i) {
            const COLORREF color = (i == 0) ? RGB(185, 220, 255) : RGB(245, 245, 245);
            DrawTextLine(dc, g_infoLines[static_cast<size_t>(i)], panel.left + 8, panel.top + 4 + i * lineHeight,
                panelWidth - 16, lineHeight, color, font);
        }
        DeleteObject(font);
    }

    if (g_inputEnabled && g_sceneId == 2 && !g_inputLine.empty()) {
        const int panelWidth = std::max(240, std::min(width - 16, 520));
        const int y = std::max(8, height - 56);
        RECT panel{ 8, y, 8 + panelWidth, y + 48 };
        DrawPanel(dc, panel, RGB(100, 160, 120));
        HFONT font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
        DrawTextLine(dc, g_inputLine, panel.left + 8, panel.top + 4, panelWidth - 16, 22, RGB(255, 255, 255), font);
        DrawTextLine(dc, L"数字・.・: を入力 / Enterでコピー（コピー後は空欄）", panel.left + 8, panel.top + 26,
            panelWidth - 16, 17, RGB(190, 220, 200), font);
        DeleteObject(font);
    }

    // This warning is independent of F10 and scene state by design.
    if (!g_tskRunning) {
        const int boxWidth = std::min(width - 20, 600);
        const int boxHeight = 100;
        const int left = std::max(10, (width - boxWidth) / 2);
        const int top = std::max(10, (height - boxHeight) / 2);
        RECT panel{ left, top, left + boxWidth, top + boxHeight };
        const COLORREF flash = g_warningBlink ? RGB(255, 55, 55) : RGB(255, 210, 40);
        DrawPanel(dc, panel, flash);
        HFONT font = CreateFontW(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
        SetTextColor(dc, flash);
        HGDIOBJ oldFont = SelectObject(dc, font);
        RECT line1{ panel.left + 8, panel.top + 12, panel.right - 8, panel.top + 52 };
        DrawTextW(dc, L"天則観（tsk.exe）が起動していません！", -1, &line1, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        HFONT smallFont = CreateFontW(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
        SelectObject(dc, smallFont);
        RECT line2{ panel.left + 8, panel.top + 56, panel.right - 8, panel.bottom - 8 };
        DrawTextW(dc, L"天則観を起動してから対戦してください", -1, &line2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
        DeleteObject(smallFont);
        DeleteObject(font);
    }

    EndPaint(window, &ps);
}

LRESULT CALLBACK OverlayWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_PAINT:
        PaintOverlay(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case kRefreshMessage:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

DWORD WINAPI OverlayThread(void*)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_module ? g_module : GetModuleHandleW(nullptr);
    wc.lpfnWndProc = OverlayWndProc;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&wc);

    g_overlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClassName, L"OIOverlay", WS_POPUP,
        0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_overlayWindow)
        return 1;
    SetLayeredWindowAttributes(g_overlayWindow, RGB(0, 0, 0), 0, LWA_COLORKEY);

    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, wc.hInstance, 0);
    g_databasePath = FindDefaultDatabase();
    if (!g_databasePath.empty())
        g_database = std::make_unique<TskDatabase>(g_databasePath);
    g_tskRunning = IsTskRunning();
    g_lastTskCheck = Clock::now();
    g_lastDbCheck = Clock::now();
    g_lastStatsRefresh = Clock::time_point{};
    g_lastWarningBlink = Clock::now();

    while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        g_gameWindow = FindGameWindow();
        const bool foreground = IsGameForeground();
        if (!g_gameWindow || IsIconic(g_gameWindow) || !foreground) {
            ShowWindow(g_overlayWindow, SW_HIDE);
            Sleep(50);
            continue;
        }

        RECT client{};
        if (GetClientRect(g_gameWindow, &client)) {
            POINT origin{ client.left, client.top };
            ClientToScreen(g_gameWindow, &origin);
            const int width = std::max(1, client.right - client.left);
            const int height = std::max(1, client.bottom - client.top);
            SetWindowPos(g_overlayWindow, HWND_TOPMOST, origin.x, origin.y, width, height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        const auto now = Clock::now();
        if (now - g_lastTskCheck >= std::chrono::milliseconds(500)) {
            g_tskRunning = IsTskRunning();
            g_lastTskCheck = now;
        }
        if (now - g_lastDbCheck >= std::chrono::milliseconds(500)) {
            RefreshDatabase();
            const bool changed = DatabaseChanged();
            if (changed)
                RefreshStats(true);
            g_lastDbCheck = now;
        }

        const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (f10Down && !g_previousF10Down) {
            g_overlayEnabled = !g_overlayEnabled;
            UpdateInfoText();
        }
        g_previousF10Down = f10Down;

        UpdateSceneState();
        if (g_sceneId != 2) {
            g_inputEnabled = false;
            g_inputBuffer.clear();
        } else {
            g_inputEnabled = true;
        }
        if (now - g_lastWarningBlink >= std::chrono::milliseconds(500)) {
            g_warningBlink = !g_warningBlink;
            g_lastWarningBlink = now;
        }
        const bool statsMustRefresh = g_database && (
            g_cachedNormalProfile != g_opponentProfile ||
            g_cachedWatchingP1 != g_watchingP1Profile ||
            g_cachedWatchingP2 != g_watchingP2Profile);
        RefreshStats(statsMustRefresh);
        UpdateInfoText();
        UpdateInputText();
        InvalidateRect(g_overlayWindow, nullptr, FALSE);
        UpdateWindow(g_overlayWindow);
        Sleep(35);
    }

    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    if (g_overlayWindow) {
        DestroyWindow(g_overlayWindow);
        g_overlayWindow = nullptr;
    }
    UnregisterClassW(kWindowClassName, wc.hInstance);
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16])
{
    return std::memcmp(hash, SokuLib::targetHash, sizeof(SokuLib::targetHash)) == 0;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE)
{
    g_module = hMyModule;
    HANDLE thread = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    if (!thread)
        return false;
    CloseHandle(thread);
    return true;
}

extern "C" int APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH)
        InterlockedExchange(&g_stopRequested, 1);
    return TRUE;
}

extern "C" __declspec(dllexport) int getPriority()
{
    return 0;
}
