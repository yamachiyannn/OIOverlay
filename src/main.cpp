// OIOverlay - OIViewer-style profile/record overlay for Touhou Hisoutensoku.
#ifndef NOMINMAX
#define NOMINMAX
#endif
// The overlay is a transparent topmost window so it can also work on SceneID 2
// and can show the TenSokuKan warning even while BattleManager is not active.
// Default.db is opened READ ONLY. This module never writes to it.

#include <Windows.h>
#include <TlHelp32.h>
#include <SokuLib.hpp>
#include "TskDatabase.hpp"

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cwctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <functional>
#include <memory>
#include <mutex>
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
HWND g_overlayWindow = nullptr; // opponent-information layer
HWND g_inputWindow = nullptr;
HWND g_warningWindow = nullptr;
HWND g_gameWindow = nullptr;
enum class OverlayLayer : LONG_PTR { Info = 1, Input = 2, Warning = 3 };
HHOOK g_keyboardHook = nullptr;
volatile LONG g_stopRequested = 0;
std::atomic<int> g_sceneId{-1};
bool g_clientMode = false;
std::atomic<bool> g_overlayEnabled{true};
bool g_previousAsyncF10Down = false;
bool g_overlayManuallyDisabledForSession = false;
bool g_tskRunning = false;
enum class TskWarningState { None, NotRunning, GameNotDetected };
TskWarningState g_tskWarningState = TskWarningState::None;
bool g_warningBlink = false;
std::atomic<bool> g_inputEnabled{false};
bool g_previousPolledKeyDown[256]{};
std::mutex g_inputMutex;
std::string g_opponentProfile;
std::string g_watchingP1Profile;
std::string g_watchingP2Profile;
std::string g_cachedNormalProfile;
std::string g_cachedWatchingP1;
std::string g_cachedWatchingP2;
std::string g_inputBuffer;
std::vector<std::wstring> g_infoLines;
std::wstring g_inputLine;
bool g_copiedFeedback = false;
std::chrono::steady_clock::time_point g_copiedFeedbackUntil{};

// Visible keyboard/clipboard diagnostics. Status values: -1 = not attempted,
// 0 = empty input, 1 = success, 2 = failure. Atomics are shared by the
// low-level keyboard callback and the overlay window thread.
std::atomic<unsigned long long> g_inputHookEvents{0};
std::atomic<DWORD> g_inputLastVirtualKey{0};
std::atomic<unsigned long long> g_inputPollEvents{0};
std::atomic<DWORD> g_inputLastPolledVirtualKey{0};
std::atomic<int> g_clipboardStatus{-1};
std::atomic<int> g_pasteStatus{-1};
std::atomic<DWORD> g_clipboardError{ERROR_SUCCESS};
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
RECT g_lastGameClientRect{};
RECT g_lastGameWindowRect{};
LONG_PTR g_lastGameWindowStyle = 0;
LONG_PTR g_lastGameWindowExStyle = 0;
HWND g_lastLocatedGameWindow = nullptr;
bool g_haveLastGameClientRect = false;
struct LayerBounds { int offsetX = 0; int offsetY = 0; int width = 0; int height = 0; bool valid = false; };
LayerBounds g_infoLayerBounds;
LayerBounds g_inputLayerBounds;
LayerBounds g_warningLayerBounds;
bool g_overlayWasVisible = false;

using Clock = std::chrono::steady_clock;
Clock::time_point g_lastTskCheck{};
Clock::time_point g_lastDbCheck{};
Clock::time_point g_lastStatsRefresh{};
Clock::time_point g_lastWarningBlink{};
Clock::time_point g_lastCharacterRefresh{};
Clock::time_point g_lastWindowSearch{};
Clock::time_point g_lastGeometryCheck{};
Clock::time_point g_lastKeyboardHookCheck{};

static int (SokuLib::BattleManager::*g_originalBattleManagerOnProcess)() = nullptr;

void RequestWindowRedraw(HWND window);
void RequestInfoRedraw();
void RequestInputRedraw();
void RequestWarningRedraw();
void RequestRedraw(); // all layers: startup, focus return or viewport geometry change
void UpdateInputText();
void UpdateInfoText();

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

// Read game-owned memory through the OS API rather than directly dereferencing a
// game pointer. During spectator-to-match transitions the game can temporarily
// replace these objects; an unreadable address must not crash th123.exe.
bool TryReadGameMemory(uintptr_t address, void* destination, size_t size)
{
    if (address < 0x10000 || !destination || size == 0)
        return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
        destination, size, &bytesRead) != FALSE && bytesRead == size;
}

bool TryReadSceneId(int& scene)
{
    int value = -1;
    if (!TryReadGameMemory(kSceneIdAddress, &value, sizeof(value)))
        return false;
    // Hisoutensoku's scene IDs used by this module are in the range 0..15.
    // Ignore transient garbage rather than treating it as a real transition.
    if (value < 0 || value > 31)
        return false;
    scene = value;
    return true;
}

std::string ReadProfile(uintptr_t offset)
{
    uint32_t networkObject = 0;
    if (!TryReadGameMemory(kPNetObjectAddress, &networkObject, sizeof(networkObject)))
        return {};
    // The target is a 32-bit game; reject null and obviously invalid pointers.
    if (networkObject < 0x10000 || networkObject > 0x7fff0000u)
        return {};
    const uintptr_t address = static_cast<uintptr_t>(networkObject) + offset;
    char buffer[kProfileSize + 1]{};
    if (!TryReadGameMemory(address, buffer, kProfileSize))
        return {};
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

// OIV Form1's normal-match overlay is visible only during character select/loading.
// Scenes 13-14 are deliberately excluded; spectator scenes are handled separately.
bool IsNormalInfoScene(int scene)
{
    return scene >= 8 && scene <= 11;
}

std::wstring BuildNormalInfo()
{
    // Keep the visible text in sync with Form1.ShowOpponentInfo.
    if (g_opponentProfile.empty())
        return L"【対戦中】\n\n対戦相手を取得中...";

    std::wstring text = L"【対戦中】\n\n対戦相手 : " + Cp932ToWide(g_opponentProfile) + L"\n";
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

bool SetClipboardText(const std::string& text)
{
    if (text.empty()) {
        g_clipboardError.store(ERROR_SUCCESS);
        return false;
    }
    const int chars = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (chars <= 0) {
        g_clipboardError.store(ERROR_NO_UNICODE_TRANSLATION);
        return false;
    }
    std::wstring wide(static_cast<size_t>(chars), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, &wide[0], chars) <= 0) {
        g_clipboardError.store(ERROR_NO_UNICODE_TRANSLATION);
        return false;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, wide.size() * sizeof(wchar_t));
    if (!memory) {
        g_clipboardError.store(GetLastError());
        return false;
    }
    void* target = GlobalLock(memory);
    if (!target) {
        const DWORD error = GetLastError();
        GlobalFree(memory);
        g_clipboardError.store(error);
        return false;
    }
    std::memcpy(target, wide.c_str(), wide.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (!OpenClipboard(nullptr)) {
        const DWORD error = GetLastError();
        GlobalFree(memory);
        g_clipboardError.store(error);
        return false;
    }
    if (!EmptyClipboard()) {
        const DWORD error = GetLastError();
        CloseClipboard();
        GlobalFree(memory);
        g_clipboardError.store(error);
        return false;
    }
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        const DWORD error = GetLastError();
        CloseClipboard();
        GlobalFree(memory);
        g_clipboardError.store(error);
        return false;
    }
    // Ownership of memory transfers to the system after successful SetClipboardData.
    CloseClipboard();
    g_clipboardError.store(ERROR_SUCCESS);
    return true;
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

bool IsAltDown()
{
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
}

bool IsControlDown()
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
}

std::string GetInputBufferSnapshot()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    return g_inputBuffer;
}

void ClearInputBuffer()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_inputBuffer.clear();
}

void AppendIpCharacter(char character)
{
    constexpr size_t kMaximumInputLength = 64;
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_copiedFeedback = false;
    if (g_inputBuffer.size() < kMaximumInputLength)
        g_inputBuffer.push_back(character);
}

void RemoveLastIpCharacter()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_copiedFeedback = false;
    if (!g_inputBuffer.empty())
        g_inputBuffer.pop_back();
}

void CopyInputToClipboardAndClear()
{
    const std::string value = GetInputBufferSnapshot();
    if (value.empty()) {
        g_clipboardStatus.store(0);
        ClearInputBuffer();
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_copiedFeedback = true;
            g_copiedFeedbackUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
        return;
    }

    if (SetClipboardText(value)) {
        g_clipboardStatus.store(1);
        ClearInputBuffer();
        {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            g_copiedFeedback = true;
            g_copiedFeedbackUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
    } else {
        // Preserve the text so the user can retry if Windows denies clipboard access.
        g_clipboardStatus.store(2);
    }
}

bool PasteClipboardIp()
{
    if (!OpenClipboard(nullptr))
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
        // Ignore unsupported characters instead of rejecting the entire paste.
    }

    constexpr size_t kMaximumInputLength = 64;
    if (normalized.size() > kMaximumInputLength)
        normalized.resize(kMaximumInputLength);
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_copiedFeedback = false;
    g_inputBuffer = normalized;
    return true;
}

bool IsAltPressed(const KBDLLHOOKSTRUCT& key)
{
    return IsAltDown() || (key.flags & LLKHF_ALTDOWN) != 0;
}

bool TryGetIpCharacter(DWORD vk, const KBDLLHOOKSTRUCT& key, char& result)
{
    if (vk >= '0' && vk <= '9' && (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0) {
        result = static_cast<char>(vk);
        return true;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        result = static_cast<char>('0' + (vk - VK_NUMPAD0));
        return true;
    }
    if (vk == VK_DECIMAL) {
        result = '.';
        return true;
    }

    BYTE state[256]{};
    if (GetKeyboardState(state)) {
        state[vk] |= 0x80;
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) state[VK_SHIFT] |= 0x80;
        else state[VK_SHIFT] &= 0x7f;
        if (IsControlDown()) state[VK_CONTROL] |= 0x80;
        if (IsAltPressed(key)) state[VK_MENU] |= 0x80;
        wchar_t translated[8]{};
        const UINT scan = key.scanCode ? key.scanCode : MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        const int count = ToUnicodeEx(vk, scan, state, translated,
            static_cast<int>(_countof(translated)), 0, GetKeyboardLayout(0));
        if (count == 1) {
            const wchar_t c = translated[0];
            if ((c >= L'0' && c <= L'9') || c == L'.' || c == L':') {
                result = static_cast<char>(c);
                return true;
            }
        }
    }

    if (vk == VK_OEM_PERIOD && (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0) {
        result = '.';
        return true;
    }
    if ((vk == VK_OEM_1 || vk == VK_OEM_PLUS) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        result = ':';
        return true;
    }
    return false;
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM message, LPARAM data)
{
    if (code >= 0 && data != 0 &&
        (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
        g_inputEnabled && g_sceneId == 2) {
        const auto& key = *reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        g_inputHookEvents.fetch_add(1);
        g_inputLastVirtualKey.store(key.vkCode);
    }

    // Diagnostic only. Actual IP input polling runs on the overlay thread,
    // so it also works on the title/menu screen before BattleManager is active.
    return CallNextHookEx(g_keyboardHook, code, message, data);
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

bool UpdateSceneState()
{
    int scene = -1;
    if (!TryReadSceneId(scene))
        return false;
    if (scene == g_sceneId)
        return false;

    const int oldScene = g_sceneId;
    g_sceneId = scene;

    // A new character-select / spectator-loading session starts enabled by default.
    // Once F10 disables the overlay, keep it disabled until the game returns to SceneID <= 7.
    if (scene <= 7) {
        g_overlayManuallyDisabledForSession = false;
    } else if ((scene == 8 || scene == 9 || scene == 12) && oldScene <= 7) {
        if (!g_overlayManuallyDisabledForSession) {
            g_overlayEnabled.store(true);
            RequestInfoRedraw();
        }
    }

    if (scene <= 7) {
        g_clientMode = false;
        g_opponentProfile.clear();
        g_normalStats = {};
        g_normalStatsValid = false;
        g_cachedNormalProfile.clear();
    } else if (scene == 9) {
        // OIV remembers client mode until the scene returns below 8.
        g_clientMode = true;
    }

    // SceneID 2 only. Any transition out clears the pending input immediately.
    if (scene == 2) {
        ClearInputBuffer();
        g_inputEnabled = true;
    } else {
        ClearInputBuffer();
        g_inputEnabled = false;
    }

    if (scene == 12 || scene == 15) {
        // Spectator profiles/stats are re-read only at scene transitions.
        const std::string p1 = ReadProfile(kLeftProfileOffset);
        const std::string p2 = ReadProfile(kRightProfileOffset);
        if (p1 != g_watchingP1Profile) {
            g_watchingP1Profile = p1;
            g_watchP1Stats = {};
            g_watchP1StatsValid = false;
            g_cachedWatchingP1.clear();
        }
        if (p2 != g_watchingP2Profile) {
            g_watchingP2Profile = p2;
            g_watchP2Stats = {};
            g_watchP2StatsValid = false;
            g_cachedWatchingP2.clear();
        }
    } else if (scene >= 8 && scene <= 11) {
        const uintptr_t offset = g_clientMode ? kLeftProfileOffset : kRightProfileOffset;
        const std::string opponent = ReadProfile(offset);
        if (opponent != g_opponentProfile) {
            g_opponentProfile = opponent;
            g_normalStats = {};
            g_normalStatsValid = false;
            g_cachedNormalProfile.clear();
        }
        if (oldScene <= 7 && opponent.empty())
            g_opponentProfile.clear();
    }

    return true;
}

void RequestWindowRedraw(HWND window)
{
    if (window && IsWindow(window))
        PostMessageW(window, kRefreshMessage, 0, 0);
}

void RequestInfoRedraw() { RequestWindowRedraw(g_overlayWindow); }
void RequestInputRedraw() { RequestWindowRedraw(g_inputWindow); }
void RequestWarningRedraw() { RequestWindowRedraw(g_warningWindow); }

void RepositionVisibleLayer(HWND window, const LayerBounds& bounds)
{
    if (!window || !bounds.valid || !IsWindowVisible(window) || !g_haveLastGameClientRect)
        return;
    SetWindowPos(window, nullptr,
        g_lastGameClientRect.left + bounds.offsetX,
        g_lastGameClientRect.top + bounds.offsetY,
        bounds.width, bounds.height,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

void RaiseOverlayLayers()
{
    // Keep each surface attached to the game's client area even when Alt+Enter
    // changes the display mode between content redraws.
    RepositionVisibleLayer(g_overlayWindow, g_infoLayerBounds);
    RepositionVisibleLayer(g_inputWindow, g_inputLayerBounds);
    RepositionVisibleLayer(g_warningWindow, g_warningLayerBounds);

    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (g_overlayWindow && IsWindow(g_overlayWindow))
        SetWindowPos(g_overlayWindow, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (g_inputWindow && IsWindow(g_inputWindow))
        SetWindowPos(g_inputWindow, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (g_warningWindow && IsWindow(g_warningWindow))
        SetWindowPos(g_warningWindow, HWND_TOPMOST, 0, 0, 0, 0, flags);
}

void RequestRedraw()
{
    RequestInfoRedraw();
    RequestInputRedraw();
    RequestWarningRedraw();
}

void UpdateInfoText()
{
    std::vector<std::wstring> next;
    if (g_overlayEnabled) {
        if (IsNormalInfoScene(g_sceneId))
            SplitLines(BuildNormalInfo(), next);
        else if (g_sceneId == 12 || g_sceneId == 15)
            SplitLines(BuildWatchingInfo(), next);
    }
    if (next != g_infoLines)
        g_infoLines = std::move(next);
}

void UpdateInputText()
{
    std::wstring next;
    std::lock_guard<std::mutex> lock(g_inputMutex);
    if (g_copiedFeedback && std::chrono::steady_clock::now() >= g_copiedFeedbackUntil)
        g_copiedFeedback = false;
    if (g_inputEnabled && g_sceneId == 2) {
        if (g_copiedFeedback)
            next = L"IP:Port ＞ Copied";
        else
            next = L"IP:Port ＞ " + NarrowAsciiToWide(g_inputBuffer) + L"_";
    }
    if (next != g_inputLine) {
        g_inputLine = std::move(next);
        RequestInputRedraw();
    }
}

void DrawTextLine(HDC dc, const std::wstring& line, int x, int y, int width, int height, COLORREF color, HFONT font,
    UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX)
{
    SetTextColor(dc, color);
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT r{ x, y, x + width, y + height };
    DrawTextW(dc, line.c_str(), static_cast<int>(line.size()), &r, format);
    SelectObject(dc, oldFont);
}

struct AlphaFill {
    COLORREF color;
    BYTE alpha;
};

int ColorDistance(COLORREF a, COLORREF b)
{
    return abs(static_cast<int>(GetRValue(a)) - static_cast<int>(GetRValue(b))) +
        abs(static_cast<int>(GetGValue(a)) - static_cast<int>(GetGValue(b))) +
        abs(static_cast<int>(GetBValue(a)) - static_cast<int>(GetBValue(b)));
}

void DrawPanel(HDC dc, const RECT& rect, COLORREF backgroundColor, COLORREF borderColor,
    std::vector<AlphaFill>& fills, BYTE backgroundAlpha)
{
    HBRUSH background = CreateSolidBrush(backgroundColor);
    FillRect(dc, &rect, background);
    DeleteObject(background);
    HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    for (const auto& fill : fills) {
        if (fill.color == backgroundColor)
            return;
    }
    fills.push_back({ backgroundColor, backgroundAlpha });
}

int TextPixelWidth(HDC dc, const std::wstring& text)
{
    if (text.empty()) return 0;
    SIZE size{};
    if (!GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size)) return 0;
    return static_cast<int>(size.cx);
}

std::vector<std::wstring> WrapLinesToWidth(HDC dc, const std::vector<std::wstring>& source, int maxWidth)
{
    std::vector<std::wstring> output;
    for (const auto& line : source) {
        if (line.empty()) {
            output.emplace_back();
            continue;
        }
        size_t offset = 0;
        while (offset < line.size()) {
            const int remaining = static_cast<int>(line.size() - offset);
            int fit = 0;
            SIZE size{};
            if (!GetTextExtentExPointW(dc, line.c_str() + offset, remaining, maxWidth, &fit, nullptr, &size))
                fit = remaining;
            if (fit <= 0) fit = 1;
            output.emplace_back(line.substr(offset, static_cast<size_t>(fit)));
            offset += static_cast<size_t>(fit);
        }
    }
    return output;
}

void DrawAndRegisterPanel(HDC dc, const RECT& rect, COLORREF bg, BYTE alpha, COLORREF border,
    std::vector<AlphaFill>& fills)
{
    DrawPanel(dc, rect, bg, border, fills, alpha);
}

void ApplyPerPixelAlpha(DWORD* pixels, int width, int height, const std::vector<AlphaFill>& fills)
{
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < count; ++i) {
        const COLORREF color = static_cast<COLORREF>(pixels[i] & 0x00ffffffu);
        if (color == RGB(0, 0, 0)) {
            pixels[i] = 0;
            continue;
        }
        const AlphaFill* best = nullptr;
        int bestDistance = 100000;
        for (const auto& fill : fills) {
            const int distance = ColorDistance(color, fill.color);
            if (distance < bestDistance) { bestDistance = distance; best = &fill; }
        }
        BYTE alpha = (best && bestDistance <= 24) ? best->alpha : 255;
        const BYTE r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
        const BYTE pr = static_cast<BYTE>((static_cast<unsigned>(r) * alpha + 127) / 255);
        const BYTE pg = static_cast<BYTE>((static_cast<unsigned>(g) * alpha + 127) / 255);
        const BYTE pb = static_cast<BYTE>((static_cast<unsigned>(b) * alpha + 127) / 255);
        pixels[i] = (static_cast<DWORD>(alpha) << 24) | (static_cast<DWORD>(pr) << 16) |
            (static_cast<DWORD>(pg) << 8) | static_cast<DWORD>(pb);
    }
}

using LayerPainter = void(*)(HDC, std::vector<AlphaFill>&);

void RenderLayerSurface(HWND window, int x, int y, int width, int height,
    const std::function<void(HDC, std::vector<AlphaFill>&)>& draw)
{
    if (!window || width <= 0 || height <= 0)
        return;
    HDC screenDc = GetDC(nullptr);
    if (!screenDc)
        return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* rawPixels = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    HDC dc = CreateCompatibleDC(screenDc);
    if (!dib || !dc || !rawPixels) {
        if (dib) DeleteObject(dib);
        if (dc) DeleteDC(dc);
        ReleaseDC(nullptr, screenDc);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, dib);
    std::memset(rawPixels, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    SetBkMode(dc, TRANSPARENT);
    std::vector<AlphaFill> fills;
    draw(dc, fills);
    ApplyPerPixelAlpha(static_cast<DWORD*>(rawPixels), width, height, fills);

    POINT destination{ x, y };
    POINT source{ 0, 0 };
    SIZE size{ width, height };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (UpdateLayeredWindow(window, screenDc, &destination, &size, dc, &source, 0, &blend, ULW_ALPHA)) {
        LayerBounds* savedBounds = nullptr;
        if (window == g_overlayWindow) savedBounds = &g_infoLayerBounds;
        else if (window == g_inputWindow) savedBounds = &g_inputLayerBounds;
        else if (window == g_warningWindow) savedBounds = &g_warningLayerBounds;
        if (savedBounds && g_haveLastGameClientRect) {
            savedBounds->offsetX = x - g_lastGameClientRect.left;
            savedBounds->offsetY = y - g_lastGameClientRect.top;
            savedBounds->width = width;
            savedBounds->height = height;
            savedBounds->valid = true;
        }
        if (!IsWindowVisible(window))
            ShowWindow(window, SW_SHOWNOACTIVATE);
        // Alt+Enter / fullscreen transitions can disturb the Z-order of
        // non-activating layered windows. Reassert this layer's topmost status
        // whenever it is redrawn; if the warning is active, put it back on top.
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (window != g_warningWindow && g_warningWindow && IsWindowVisible(g_warningWindow) &&
            g_tskWarningState != TskWarningState::None) {
            SetWindowPos(g_warningWindow, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }

    SelectObject(dc, oldBitmap);
    DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(nullptr, screenDc);
}

void PaintInfoLayer(HWND window)
{
    // WM_PAINT can occur independently of our posted refresh message. Rebuild
    // the same info text used by both normal and spectator views here as well,
    // so entering SceneID 8-11 cannot leave the info layer with stale/empty lines.
    UpdateInfoText();
    const int gameWidth = static_cast<int>(g_lastGameClientRect.right - g_lastGameClientRect.left);
    const int gameHeight = static_cast<int>(g_lastGameClientRect.bottom - g_lastGameClientRect.top);
    if (!g_haveLastGameClientRect || !g_overlayEnabled || g_infoLines.empty() || gameWidth <= 0 || gameHeight <= 0) {
        ShowWindow(window, SW_HIDE);
        return;
    }

    HDC measureDc = GetDC(nullptr);
    if (!measureDc) return;
    HFONT font = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
    HGDIOBJ oldFont = SelectObject(measureDc, font);
    const int maxPanelWidth = (std::max)(120, (std::min)(gameWidth - 24, 620));
    const auto lines = WrapLinesToWidth(measureDc, g_infoLines, (std::max)(50, maxPanelWidth - 18));
    int widest = 0;
    for (const auto& line : lines) widest = (std::max)(widest, TextPixelWidth(measureDc, line));
    const int panelWidth = (std::min)(maxPanelWidth, (std::max)(150, widest + 18));
    int lineHeight = 14;
    const int availableHeight = (std::max)(40, gameHeight - 16);
    if (!lines.empty() && static_cast<int>(lines.size()) * lineHeight + 12 > availableHeight)
        lineHeight = (std::max)(9, (availableHeight - 12) / static_cast<int>(lines.size()));
    SelectObject(measureDc, oldFont);
    DeleteObject(font);
    ReleaseDC(nullptr, measureDc);

    int fontHeight = lineHeight < 14 ? (std::max)(8, lineHeight - 2) : 12;
    const int panelHeight = static_cast<int>(lines.size()) * lineHeight + 12;
    // Center the information window vertically rather than keeping it near the top third.
    int top = (std::max)(22, (gameHeight - panelHeight) / 2);
    if (top + panelHeight > gameHeight - 8) top = (std::max)(8, gameHeight - panelHeight - 8);
    const int left = 12;
    const COLORREF background = RGB(8, 8, 12);
    RenderLayerSurface(window, g_lastGameClientRect.left + left, g_lastGameClientRect.top + top,
        panelWidth, panelHeight, [=](HDC dc, std::vector<AlphaFill>& fills) {
            HFONT drawFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
            RECT panel{ 0, 0, panelWidth, panelHeight };
            DrawAndRegisterPanel(dc, panel, background, 165, RGB(90, 115, 145), fills);
            for (size_t i = 0; i < lines.size(); ++i) {
                const COLORREF color = i == 0 ? RGB(185, 220, 255) : RGB(245, 245, 245);
                DrawTextLine(dc, lines[i], 8, 5 + static_cast<int>(i) * lineHeight,
                    panelWidth - 16, lineHeight, color, drawFont);
            }
            DeleteObject(drawFont);
        });
}

void PaintInputLayer(HWND window)
{
    const int gameWidth = static_cast<int>(g_lastGameClientRect.right - g_lastGameClientRect.left);
    const int gameHeight = static_cast<int>(g_lastGameClientRect.bottom - g_lastGameClientRect.top);
    if (!g_haveLastGameClientRect || !g_inputEnabled || g_sceneId != 2 || gameWidth <= 0 || gameHeight <= 0) {
        ShowWindow(window, SW_HIDE);
        return;
    }

    std::wstring inputLine;
    size_t inputLength = 0;
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        inputLine = g_inputLine;
        inputLength = g_inputBuffer.size();
    }

    const std::wstring help = L"1~9 / . / : / BS / Enter / Ctrl+V";
    const COLORREF background = RGB(10, 10, 10);
    HDC measureDc = GetDC(nullptr);
    if (!measureDc) return;
    HFONT font = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
    HGDIOBJ oldFont = SelectObject(measureDc, font);
    const int maxPanelWidth = (std::max)(120, (std::min)(gameWidth - 24, 520));
    int requestedWidth = TextPixelWidth(measureDc, inputLine);
    requestedWidth = (std::max)(requestedWidth, TextPixelWidth(measureDc, help));

    const int panelWidth = (std::min)(maxPanelWidth, (std::max)(190, requestedWidth + 18));
    SelectObject(measureDc, oldFont);
    DeleteObject(font);
    ReleaseDC(nullptr, measureDc);

    const int panelHeight = 42;
    const int left = 12;
    const int top = (std::max)(8, gameHeight - panelHeight - 14);
    RenderLayerSurface(window, g_lastGameClientRect.left + left, g_lastGameClientRect.top + top,
        panelWidth, panelHeight, [=](HDC dc, std::vector<AlphaFill>& fills) {
            HFONT drawFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
            RECT panel{ 0, 0, panelWidth, panelHeight };
            DrawAndRegisterPanel(dc, panel, background, 190, RGB(90, 145, 110), fills);
            DrawTextLine(dc, inputLine, 8, 2, panelWidth - 16, 18, RGB(255, 255, 255), drawFont);
            DrawTextLine(dc, help, 8, 20, panelWidth - 16, 18, RGB(190, 220, 200), drawFont);
            DeleteObject(drawFont);
        });
}

void PaintWarningLayer(HWND window)
{
    const int gameWidth = static_cast<int>(g_lastGameClientRect.right - g_lastGameClientRect.left);
    const int gameHeight = static_cast<int>(g_lastGameClientRect.bottom - g_lastGameClientRect.top);
    if (!g_haveLastGameClientRect || g_tskWarningState == TskWarningState::None || gameWidth <= 0 || gameHeight <= 0) {
        ShowWindow(window, SW_HIDE);
        return;
    }

    const bool notRunning = g_tskWarningState == TskWarningState::NotRunning;
    const std::wstring title = notRunning ? L"天則観（tsk.exe）が起動していません！" : L"天則観が非想天則を検知していません";
    const std::wstring subtitle = notRunning ? L"天則観を起動してから対戦してください" : L"非想天則を再起動してください";
    const COLORREF flash = g_warningBlink ? RGB(255, 55, 55) : RGB(255, 210, 40);
    HDC measureDc = GetDC(nullptr);
    if (!measureDc) return;
    HFONT titleFont = CreateFontW(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
    HFONT subtitleFont = CreateFontW(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
    HGDIOBJ oldFont = SelectObject(measureDc, titleFont);
    const int titleWidth = TextPixelWidth(measureDc, title);
    SelectObject(measureDc, subtitleFont);
    const int subtitleWidth = TextPixelWidth(measureDc, subtitle);
    SelectObject(measureDc, oldFont);
    DeleteObject(subtitleFont);
    DeleteObject(titleFont);
    ReleaseDC(nullptr, measureDc);

    const int boxWidth = (std::min)(gameWidth - 20, (std::max)(320, (std::max)(titleWidth, subtitleWidth) + 36));
    const int boxHeight = 102;
    const int left = (std::max)(10, (gameWidth - boxWidth) / 2);
    const int top = (std::max)(10, (gameHeight - boxHeight) / 2);
    const COLORREF warningBackground = notRunning ? RGB(12, 12, 18) : RGB(14, 14, 22);
    const BYTE warningAlpha = notRunning ? 255 : 145;
    RenderLayerSurface(window, g_lastGameClientRect.left + left, g_lastGameClientRect.top + top,
        boxWidth, boxHeight, [=](HDC dc, std::vector<AlphaFill>& fills) {
            HFONT tf = CreateFontW(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
            HFONT sf = CreateFontW(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, SHIFTJIS_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"MS Gothic");
            RECT panel{ 0, 0, boxWidth, boxHeight };
            DrawAndRegisterPanel(dc, panel, warningBackground, warningAlpha, flash, fills);
            DrawTextLine(dc, title, 12, 12, boxWidth - 24, 38, flash, tf,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            DrawTextLine(dc, subtitle, 12, 56, boxWidth - 24, 28, RGB(255, 255, 255), sf,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            DeleteObject(sf);
            DeleteObject(tf);
        });
}

void PaintOverlay(HWND window)
{
    if (window == g_overlayWindow) PaintInfoLayer(window);
    else if (window == g_inputWindow) PaintInputLayer(window);
    else if (window == g_warningWindow) PaintWarningLayer(window);
}

LRESULT CALLBACK OverlayWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(window, &ps);
        EndPaint(window, &ps);
        PaintOverlay(window);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case kRefreshMessage:
        if (window == g_inputWindow)
            UpdateInputText();
        else if (window == g_overlayWindow)
            UpdateInfoText();
        PaintOverlay(window);
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

void PollIpInputFromGameFrame()
{
    // Poll both top-row and numpad keys once per game frame. Edge detection
    // avoids repeated characters when a key is held down.
    static const DWORD keysToPoll[] = {
        '0','1','2','3','4','5','6','7','8','9',
        VK_NUMPAD0,VK_NUMPAD1,VK_NUMPAD2,VK_NUMPAD3,VK_NUMPAD4,
        VK_NUMPAD5,VK_NUMPAD6,VK_NUMPAD7,VK_NUMPAD8,VK_NUMPAD9,
        VK_DECIMAL, VK_OEM_PERIOD, VK_OEM_1, VK_OEM_PLUS,
        VK_BACK, VK_RETURN, 'V'
    };

    const bool inputMode = g_inputEnabled.load() && g_sceneId.load() == 2 && IsGameForeground();
    for (DWORD vk : keysToPoll) {
        const bool down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
        const bool pressed = down && !g_previousPolledKeyDown[vk];
        g_previousPolledKeyDown[vk] = down;
        if (!pressed || !inputMode)
            continue;

        g_inputPollEvents.fetch_add(1);
        g_inputLastPolledVirtualKey.store(vk);

        // Alt+Enter must be left entirely to the game for fullscreen switching.
        if (vk == VK_RETURN && IsAltDown())
            continue;

        bool handled = false;
        if (vk == VK_RETURN) {
            CopyInputToClipboardAndClear();
            handled = true;
        } else if (vk == 'V' && IsControlDown()) {
            g_pasteStatus.store(PasteClipboardIp() ? 1 : 2);
            handled = true;
        } else if (IsControlDown()) {
            // Do not interpret modified shortcuts as IP characters.
            continue;
        } else if (vk == VK_BACK) {
            RemoveLastIpCharacter();
            handled = true;
        } else {
            KBDLLHOOKSTRUCT key{};
            key.vkCode = vk;
            key.scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            char character = 0;
            if (TryGetIpCharacter(vk, key, character)) {
                AppendIpCharacter(character);
                handled = true;
            }
        }

        if (handled) {
            UpdateInputText();
            RequestInputRedraw();
        }
    }
}

int __fastcall HookBattleManagerOnProcess(SokuLib::BattleManager* self)
{
    // F10 is polled asynchronously by the overlay thread so it also works while
    // loading spectator matches, before BattleManager::onProcess is running.
    if (g_originalBattleManagerOnProcess)
        return (self->*g_originalBattleManagerOnProcess)();
    return 0;
}

bool InstallBattleManagerProcessHook()
{
    void* slot = static_cast<void*>(&SokuLib::VTable_BattleManager.onProcess);
    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtection)) {
        OutputDebugStringW(L"[OIOverlay] Could not make BattleManager::onProcess writable.\n");
        return false;
    }

    g_originalBattleManagerOnProcess = SokuLib::TamperDword(
        &SokuLib::VTable_BattleManager.onProcess, HookBattleManagerOnProcess);

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(DWORD), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(DWORD));
    if (!g_originalBattleManagerOnProcess) {
        OutputDebugStringW(L"[OIOverlay] BattleManager::onProcess hook failed.\n");
        return false;
    }
    return true;
}

bool InstallKeyboardHook()
{
    if (g_keyboardHook)
        return true;
    SetLastError(ERROR_SUCCESS);
    HMODULE hookModule = g_module ? g_module : GetModuleHandleW(nullptr);
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hookModule, 0);
    if (!hook) {
        OutputDebugStringW(L"[OIOverlay] WH_KEYBOARD_LL install failed; will retry.\n");
        return false;
    }
    g_keyboardHook = hook;
    return true;
}

DWORD WINAPI OverlayThread(void*)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_module ? g_module : GetModuleHandleW(nullptr);
    wc.lpfnWndProc = OverlayWndProc;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&wc);

    const DWORD style = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    auto createLayer = [&](OverlayLayer layer) -> HWND {
        HWND window = CreateWindowExW(style, kWindowClassName, L"OIOverlay", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (window)
            SetWindowLongPtrW(window, GWLP_USERDATA, static_cast<LONG_PTR>(layer));
        return window;
    };
    g_overlayWindow = createLayer(OverlayLayer::Info);
    g_inputWindow = createLayer(OverlayLayer::Input);
    g_warningWindow = createLayer(OverlayLayer::Warning);
    if (!g_overlayWindow || !g_inputWindow || !g_warningWindow) {
        if (g_overlayWindow) DestroyWindow(g_overlayWindow);
        if (g_inputWindow) DestroyWindow(g_inputWindow);
        if (g_warningWindow) DestroyWindow(g_warningWindow);
        g_overlayWindow = g_inputWindow = g_warningWindow = nullptr;
        UnregisterClassW(kWindowClassName, wc.hInstance);
        return 1;
    }

    // Per-pixel transparency is applied by UpdateLayeredWindow, not SetLayeredWindowAttributes.
    // The low-level hook is diagnostic only; input is polled from the game's frame hook.
    InstallKeyboardHook();
    g_lastKeyboardHookCheck = Clock::now();
    g_databasePath = FindDefaultDatabase();
    if (!g_databasePath.empty())
        g_database = std::make_unique<TskDatabase>(g_databasePath);

    g_tskRunning = IsTskRunning();
    g_tskWarningState = g_tskRunning ? TskWarningState::None : TskWarningState::NotRunning;
    g_lastTskCheck = Clock::now();
    g_lastDbCheck = Clock::now();
    g_lastWarningBlink = Clock::now();
    g_lastCharacterRefresh = Clock::now();
    g_lastWindowSearch = Clock::time_point{};
    g_lastGeometryCheck = Clock::time_point{};
    RequestRedraw();

    while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const auto now = Clock::now();

        // Async F10 polling: do not depend on BattleManager::onProcess, which may
        // not run during character-select or spectator loading. Ignore SceneID <= 7.
        const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (g_sceneId.load() > 7 && IsGameForeground() && f10Down && !g_previousAsyncF10Down) {
            const bool enable = !g_overlayEnabled.load();
            g_overlayEnabled.store(enable);
            if (!enable)
                g_overlayManuallyDisabledForSession = true;
            RequestInfoRedraw();
        }
        g_previousAsyncF10Down = f10Down;

        if (now - g_lastWindowSearch >= std::chrono::milliseconds(35) || !g_gameWindow) {
            HWND found = FindGameWindow();
            if (found != g_gameWindow) {
                g_gameWindow = found;
                g_haveLastGameClientRect = false;
            }
            g_lastWindowSearch = now;
        }

        const bool canShow = g_gameWindow && !IsIconic(g_gameWindow) && IsGameForeground();
        if (!canShow) {
            if (g_overlayWasVisible) {
                ShowWindow(g_overlayWindow, SW_HIDE);
                ShowWindow(g_inputWindow, SW_HIDE);
                ShowWindow(g_warningWindow, SW_HIDE);
                g_overlayWasVisible = false;
            }
            Sleep(35);
            continue;
        }
        if (!g_overlayWasVisible) {
            g_overlayWasVisible = true;
            RequestRedraw();
        }

        if (now - g_lastGeometryCheck >= std::chrono::milliseconds(35)) {
            RECT client{};
            RECT windowRect{};
            if (GetClientRect(g_gameWindow, &client) && GetWindowRect(g_gameWindow, &windowRect)) {
                POINT origin{ client.left, client.top };
                ClientToScreen(g_gameWindow, &origin);
                const int w = static_cast<int>((std::max)(static_cast<LONG>(1), static_cast<LONG>(client.right - client.left)));
                const int h = static_cast<int>((std::max)(static_cast<LONG>(1), static_cast<LONG>(client.bottom - client.top)));
                const LONG_PTR windowStyle = GetWindowLongPtrW(g_gameWindow, GWL_STYLE);
                const LONG_PTR windowExStyle = GetWindowLongPtrW(g_gameWindow, GWL_EXSTYLE);
                const bool changed = !g_haveLastGameClientRect || g_lastLocatedGameWindow != g_gameWindow ||
                    g_lastGameClientRect.left != origin.x || g_lastGameClientRect.top != origin.y ||
                    g_lastGameClientRect.right != origin.x + w || g_lastGameClientRect.bottom != origin.y + h ||
                    g_lastGameWindowRect.left != windowRect.left || g_lastGameWindowRect.top != windowRect.top ||
                    g_lastGameWindowRect.right != windowRect.right || g_lastGameWindowRect.bottom != windowRect.bottom ||
                    g_lastGameWindowStyle != windowStyle || g_lastGameWindowExStyle != windowExStyle;
                if (changed) {
                    g_lastGameClientRect = RECT{ origin.x, origin.y, origin.x + w, origin.y + h };
                    g_lastGameWindowRect = windowRect;
                    g_lastGameWindowStyle = windowStyle;
                    g_lastGameWindowExStyle = windowExStyle;
                    g_lastLocatedGameWindow = g_gameWindow;
                    g_haveLastGameClientRect = true;
                    // A mode/style/position change is a real viewport event; re-render each independent layer.
                    RequestRedraw();
                }
            }
            // Match the earlier working overlay's fast placement cadence. The three
            // layers stay independent; only their Z-order is touched on stable frames.
            RaiseOverlayLayers();
            g_lastGeometryCheck = now;
        }

        if (now - g_lastKeyboardHookCheck >= std::chrono::seconds(2)) {
            if (!g_keyboardHook && InstallKeyboardHook())
                RequestInputRedraw();
            g_lastKeyboardHookCheck = now;
        }

        if (now - g_lastTskCheck >= std::chrono::milliseconds(300)) {
            const bool nowRunning = IsTskRunning();
            if (nowRunning != g_tskRunning) {
                const bool wasRunning = g_tskRunning;
                g_tskRunning = nowRunning;
                g_tskWarningState = !nowRunning ? TskWarningState::NotRunning
                    : (wasRunning ? TskWarningState::None : TskWarningState::GameNotDetected);
                g_warningBlink = false;
                g_lastWarningBlink = now;
                RequestWarningRedraw();
            }
            g_lastTskCheck = now;
        }

        const bool sceneChanged = UpdateSceneState();
        if (sceneChanged) {
            if (IsNormalInfoScene(g_sceneId) || g_sceneId == 12 || g_sceneId == 15)
                RefreshStats(true);
            UpdateInfoText();
            UpdateInputText();
            RequestInfoRedraw();
            RequestInputRedraw();
            g_lastCharacterRefresh = now;
        }

        // Poll IP input independently of BattleManager::onProcess so it works
        // on SceneID 2 even when the game is at the title/menu screen.
        PollIpInputFromGameFrame();
        UpdateInputText();

        // Only the information layer refreshes once per second in character-select.
        // Spectator records are re-read only on SceneID transitions.
        if (g_overlayEnabled && IsNormalInfoScene(g_sceneId) &&
            (g_infoLines.empty() || now - g_lastCharacterRefresh >= std::chrono::seconds(1))) {
            const uintptr_t offset = g_clientMode ? kLeftProfileOffset : kRightProfileOffset;
            const std::string opponent = ReadProfile(offset);
            if (!opponent.empty() && opponent != g_opponentProfile) {
                g_opponentProfile = opponent;
                g_normalStats = {};
                g_normalStatsValid = false;
                g_cachedNormalProfile.clear();
            }
            RefreshStats(true);
            UpdateInfoText();
            RequestInfoRedraw();
            g_lastCharacterRefresh = now;
        }

        if (now - g_lastDbCheck >= std::chrono::milliseconds(500)) {
            RefreshDatabase();
            (void)DatabaseChanged();
            g_lastDbCheck = now;
        }

        if (g_tskWarningState != TskWarningState::None &&
            now - g_lastWarningBlink >= std::chrono::milliseconds(500)) {
            g_warningBlink = !g_warningBlink;
            g_lastWarningBlink = now;
            RequestWarningRedraw();
        }
        Sleep(8);
    }

    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    if (g_overlayWindow) DestroyWindow(g_overlayWindow);
    if (g_inputWindow) DestroyWindow(g_inputWindow);
    if (g_warningWindow) DestroyWindow(g_warningWindow);
    g_overlayWindow = g_inputWindow = g_warningWindow = nullptr;
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
    if (!InstallBattleManagerProcessHook())
        return false;
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
