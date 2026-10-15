// OIOverlay - OIViewer-style profile/record overlay for Touhou Hisoutensoku.
#ifndef NOMINMAX
#define NOMINMAX
#endif
// UI is drawn into Hisoutensoku's Direct3D 9 frame (no persistent overlay windows).
// This also allows the SceneID 2 IP helper and TSK warning to be shown outside battle.
// Default.db is opened READ ONLY. This module never writes to it.

#include <Windows.h>
#include <TlHelp32.h>
#include <SokuLib.hpp>
#include "TskDatabase.hpp"
#include "CombinedPlayersDatabase.hpp"
#include "IpPortParser.hpp"
#include "OverlayConfig.hpp"
#include "ImGuiRenderer.hpp"
#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cwctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <iomanip>
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

HMODULE g_module = nullptr;
HWND g_gameWindow = nullptr;
HHOOK g_keyboardHook = nullptr;
volatile LONG g_stopRequested = 0;
std::atomic<int> g_sceneId{-1};
bool g_clientMode = false;
std::atomic<bool> g_overlayEnabled{true};
// Tracks the configurable overlay-toggle key and F10 separately. F10 has
// scene-specific actions (title history / reversal counter) that must not
// also toggle the normal statistics panel.
bool g_previousAsyncF10Down = false;
bool g_previousF10Down = false;
bool g_overlayManuallyDisabledForSession = false;
int g_displayPage = 0;
bool g_titleHistoryVisible = false;
std::vector<std::wstring> g_titleHistoryLines;
int g_reversalCounterCount = 0;
bool g_reversalCounterVisible = false;
std::wstring g_reversalCounterStatus;
std::vector<std::wstring> g_reversalCounterHistory;
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

// Keyboard/clipboard diagnostics. Status values: -1 = not attempted,
// 0 = empty input, 1 = success, 2 = failure.
std::atomic<unsigned long long> g_inputHookEvents{0};
std::atomic<DWORD> g_inputLastVirtualKey{0};
std::atomic<unsigned long long> g_inputPollEvents{0};
std::atomic<DWORD> g_inputLastPolledVirtualKey{0};
std::atomic<int> g_clipboardStatus{-1};
std::atomic<int> g_pasteStatus{-1};
std::atomic<DWORD> g_clipboardError{ERROR_SUCCESS};
std::unique_ptr<TskDatabase> g_database;
std::unique_ptr<CombinedPlayersDatabase> g_playersDatabase;
OverlayConfig g_config;
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
Clock::time_point g_lastCharacterRefresh{};
Clock::time_point g_lastWindowSearch{};
Clock::time_point g_lastKeyboardHookCheck{};
Clock::time_point g_lastPageFlip{};

static int (SokuLib::BattleManager::*g_originalBattleManagerOnProcess)() = nullptr;

void UpdateInputText();
void UpdateInfoText();
std::wstring CharacterName(int id);
std::wstring RateText(int wins, int losses);
void RefreshTitleHistory();
void RegisterReversalCounter();

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

// Returns the directory containing this module (not th123.exe). RLcounter.txt
// intentionally lives beside OIOverlay.dll, matching the requested install layout.
std::wstring OverlayModuleDirectory()
{
    wchar_t fullPath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(g_module, fullPath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    const std::wstring path(fullPath, length);
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
if (!g_config.tskDbPath.empty())
return g_config.tskDbPath;
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

std::wstring ProfileDisplayName(const std::string& profile)
{
if (g_playersDatabase && !profile.empty()) {
std::wstring displayName;
std::vector<std::string> profiles;
if (g_playersDatabase->ResolveProfile(profile, displayName, profiles) && !displayName.empty())
return displayName;
}
return Cp932ToWide(profile);
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

std::wstring ShortRunDate(const std::string& value)
{
    int year = 0, month = 0, day = 0;
    if (sscanf_s(value.c_str(), "%d/%d/%d", &year, &month, &day) == 3 ||
        sscanf_s(value.c_str(), "%d-%d-%d", &year, &month, &day) == 3) {
        return std::to_wstring(month) + L"/" + std::to_wstring(day);
    }
    return value.empty() ? L"--/--" : NarrowAsciiToWide(value);
}

std::wstring RunLine(const TskRunStats& run, bool includeProfile)
{
    std::wostringstream out;
    out << ShortRunDate(run.endDateText) << L"　";
    if (includeProfile)
        out << ProfileDisplayName(run.profileNameCp932) << L"　";
    out << std::setw(2) << run.wins << L"勝 "
        << std::setw(3) << run.losses << L"敗　("
        << RateText(run.wins, run.losses) << L")";
    return out.str();
}

std::wstring MainCharacterRecordLine(const TskStats& stats)
{
    std::wostringstream out;
    out << CharacterName(stats.mainCharacterId) << L"　"
        << std::setw(2) << stats.mainCharacterWins << L"勝 "
        << std::setw(3) << stats.mainCharacterLosses << L"敗　("
        << RateText(stats.mainCharacterWins, stats.mainCharacterLosses) << L")";
    return out.str();
}

std::string WideToUtf8String(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        &result[0], required, nullptr, nullptr) != required)
        return {};
    return result;
}

std::wstring CurrentCounterTimestamp()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream out;
    out << now.wYear << L"/" << now.wMonth << L"/" << now.wDay << L"_"
        << std::setfill(L'0') << std::setw(2) << now.wHour << L":"
        << std::setw(2) << now.wMinute << L":"
        << std::setw(2) << now.wSecond;
    return out.str();
}

bool AppendCounterText(const std::wstring& line)
{
    const std::wstring directory = OverlayModuleDirectory();
    if (directory.empty())
        return false;
    const std::wstring path = directory + L"\\RLcounter.txt";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    const bool gotSize = GetFileSizeEx(file, &size) != FALSE;
    bool ok = gotSize;
    if (ok && size.QuadPart == 0) {
        static const BYTE utf8Bom[] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        ok = WriteFile(file, utf8Bom, sizeof(utf8Bom), &written, nullptr) != FALSE &&
            written == sizeof(utf8Bom);
    }
    if (ok) {
        const std::string utf8 = WideToUtf8String(line + L"\r\n");
        DWORD written = 0;
        ok = !utf8.empty() && WriteFile(file, utf8.data(),
            static_cast<DWORD>(utf8.size()), &written, nullptr) != FALSE &&
            written == utf8.size();
    }
    CloseHandle(file);
    return ok;
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
std::wstring rate;
if (wins + losses <= 0) {
    rate = L"---.-%";
} else {
    std::wostringstream out;
    out << std::fixed << std::setprecision(1)
        << (100.0 * wins / (wins + losses)) << L"%";
    rate = out.str();
}
// Keep the percentage column aligned with the no-record marker (---.-%).
if (rate.size() < 6)
    rate.insert(rate.begin(), 6 - rate.size(), L' ');
return rate;
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
return RateText(stats.totalLosses, stats.totalWins);
};
std::wstring a = rateFor(p1);
std::wstring b = rateFor(p2);
if (a.size() < 6) a.insert(a.begin(), 6 - a.size(), L' ');
if (b.size() < 6) b.insert(b.begin(), 6 - b.size(), L' ');
return a + L" /" + b + L" ： " + label;
}

std::wstring WatchingPlayerText(const wchar_t* side, const std::string& profile, const TskStats& stats, bool statsRead)
{
std::wstring text = std::wstring(side) + L" : " + ProfileDisplayName(profile) + L"\n";
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
return RateText(p->losses, p->wins);
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

std::wstring BuildNormalSecondaryInfo()
{
    std::wstring text = L"【メインキャラ限定 / 連戦履歴】\n\n";
    if (g_opponentProfile.empty())
        return text + L"対戦相手を取得中...";
    if (!g_normalStatsValid || !g_normalStats.found)
        return text + L"対戦記録がありません";

    const TskStats& stats = g_normalStats;
    text += MainCharacterRecordLine(stats) + L"\n\n";
    if (stats.recentRuns.empty()) {
        text += L"連戦履歴なし";
    } else {
        for (size_t i = 0; i < stats.recentRuns.size(); ++i) {
            text += RunLine(stats.recentRuns[i], false);
            if (i + 1 < stats.recentRuns.size())
                text += L"\n";
        }
    }
    return text;
}

void AppendWatchingSecondaryPlayer(
    std::wstring& text,
    const wchar_t* label,
    const TskStats& stats,
    bool statsValid)
{
    text += std::wstring(label) + L" ";
    if (!statsValid || !stats.found) {
        text += L"対戦記録がありません\n";
        return;
    }
    text += MainCharacterRecordLine(stats) + L"\n";
    if (stats.recentRuns.empty()) {
        text += L"  連戦履歴なし\n";
        return;
    }
    for (size_t i = 0; i < stats.recentRuns.size(); ++i) {
        text += L"  " + RunLine(stats.recentRuns[i], false);
        text += L"\n";
    }
}

std::wstring BuildWatchingSecondaryInfo()
{
    std::wstring text = L"【観戦中：メインキャラ限定 / 連戦履歴】\n\n";
    if (g_watchingP1Profile.empty() || g_watchingP2Profile.empty())
        return text + L"1P / 2Pのプロファイルを取得中...";
    AppendWatchingSecondaryPlayer(text, L"1P", g_watchP1Stats, g_watchP1StatsValid);
    text += L"\n";
    AppendWatchingSecondaryPlayer(text, L"2P", g_watchP2Stats, g_watchP2StatsValid);
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
// Empty Enter is intentionally a no-op: do not touch the clipboard and
// do not show the transient "Copied" feedback.
g_clipboardStatus.store(0);
std::lock_guard<std::mutex> lock(g_inputMutex);
g_copiedFeedback = false;
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

std::string normalized;
if (!ExtractFirstValidIpPort(value, normalized))
return false;

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
if (!g_playersDatabase && !g_config.playersDbPath.empty())
g_playersDatabase = std::make_unique<CombinedPlayersDatabase>(g_config.playersDbPath);
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
g_normalStatsValid = false;
if (!g_opponentProfile.empty()) {
std::wstring displayName;
std::vector<std::string> profiles;
if (g_playersDatabase && g_playersDatabase->ResolveProfile(g_opponentProfile, displayName, profiles))
g_normalStatsValid = g_database->GetOpponentStats(profiles, g_normalStats);
else
g_normalStatsValid = g_database->GetOpponentStats(g_opponentProfile, g_normalStats);
}
g_cachedNormalProfile = g_opponentProfile;
}
if (force || g_cachedWatchingP1 != g_watchingP1Profile) {
g_watchP1Stats = {};
g_watchP1StatsValid = false;
if (!g_watchingP1Profile.empty()) {
std::wstring displayName;
std::vector<std::string> profiles;
if (g_playersDatabase && g_playersDatabase->ResolveProfile(g_watchingP1Profile, displayName, profiles))
g_watchP1StatsValid = g_database->GetOpponentStats(profiles, g_watchP1Stats);
else
g_watchP1StatsValid = g_database->GetOpponentStats(g_watchingP1Profile, g_watchP1Stats);
}
g_cachedWatchingP1 = g_watchingP1Profile;
}
if (force || g_cachedWatchingP2 != g_watchingP2Profile) {
g_watchP2Stats = {};
g_watchP2StatsValid = false;
if (!g_watchingP2Profile.empty()) {
std::wstring displayName;
std::vector<std::string> profiles;
if (g_playersDatabase && g_playersDatabase->ResolveProfile(g_watchingP2Profile, displayName, profiles))
g_watchP2StatsValid = g_database->GetOpponentStats(profiles, g_watchP2Stats);
else
g_watchP2StatsValid = g_database->GetOpponentStats(g_watchingP2Profile, g_watchP2Stats);
}
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

    if (scene <= 7) {
        g_displayPage = 0;
        g_titleHistoryVisible = false;
        g_titleHistoryLines.clear();
    } else if (IsNormalInfoScene(scene) || scene == 12 || scene == 15) {
        g_displayPage = 0;
        g_lastPageFlip = Clock::now();
        g_titleHistoryVisible = false;
        g_titleHistoryLines.clear();
    } else if (scene != 2) {
        g_titleHistoryVisible = false;
        g_titleHistoryLines.clear();
    }

    // A new character-select / spectator-loading session starts enabled by default.
    // Once F10 disables the overlay, keep it disabled until the game returns to SceneID <= 7.
    if (scene <= 7) {
        g_overlayManuallyDisabledForSession = false;
    } else if ((scene == 8 || scene == 9 || scene == 12) && oldScene <= 7) {
        if (!g_overlayManuallyDisabledForSession) {
            g_overlayEnabled.store(g_config.defaultVisible);
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

void UpdateInfoText()
{
    std::vector<std::wstring> next;
    if (g_overlayEnabled) {
        if (IsNormalInfoScene(g_sceneId))
            SplitLines(g_displayPage == 0 ? BuildNormalInfo() : BuildNormalSecondaryInfo(), next);
        else if (g_sceneId == 12 || g_sceneId == 15)
            SplitLines(g_displayPage == 0 ? BuildWatchingInfo() : BuildWatchingSecondaryInfo(), next);
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
    }
}

struct RenderSnapshot
{
    std::vector<std::wstring> infoLines;
    std::vector<std::wstring> titleHistoryLines;
    std::vector<std::wstring> reversalCounterLines;
    std::wstring inputLine;
    int sceneId = -1;
    int warningState = 0;
    bool overlayEnabled = true;
    bool inputVisible = false;
    bool warningBlink = false;
    bool titleHistoryVisible = false;
    bool reversalCounterVisible = false;
};

std::mutex g_renderSnapshotMutex;
RenderSnapshot g_renderSnapshot;

void PublishRenderSnapshot()
{
    RenderSnapshot next;
    next.infoLines = g_infoLines;
    next.titleHistoryLines = g_titleHistoryLines;
    next.titleHistoryVisible = g_titleHistoryVisible;
    next.reversalCounterVisible = g_reversalCounterVisible;
    next.reversalCounterLines.push_back(L"リバサルミネスカウンター : " + std::to_wstring(g_reversalCounterCount));
    if (!g_reversalCounterStatus.empty())
        next.reversalCounterLines.push_back(g_reversalCounterStatus);
    const size_t reversalFirst = g_reversalCounterHistory.size() > 5
        ? g_reversalCounterHistory.size() - 5 : 0;
    for (size_t i = g_reversalCounterHistory.size(); i > reversalFirst; --i)
        next.reversalCounterLines.push_back(g_reversalCounterHistory[i - 1]);
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        next.inputLine = g_inputLine;
    }
    next.sceneId = g_sceneId.load();
    next.warningState = static_cast<int>(g_tskWarningState);
    next.overlayEnabled = g_overlayEnabled.load();
    next.inputVisible = g_inputEnabled.load() && next.sceneId == 2;
    next.warningBlink = g_warningBlink;
    std::lock_guard<std::mutex> lock(g_renderSnapshotMutex);
    g_renderSnapshot = std::move(next);
}

std::string WideToUtf8ForUi(const std::wstring& value)
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

struct UiTextLine
{
    std::string text;
    ImU32 color;
    float height;
};

void AddPanel(ImDrawList* draw, const ImVec2& min, const ImVec2& max,
    ImU32 background, ImU32 border)
{
    draw->AddRectFilled(min, max, background, 4.0f);
    draw->AddRect(min, max, border, 4.0f, 0, 1.0f);
}

void DrawInfoPanel(ImDrawList* draw, int screenWidth, int screenHeight,
    const std::vector<std::wstring>& sourceLines, bool alignTop = false)
{
    if (sourceLines.empty() || screenWidth <= 24 || screenHeight <= 24)
        return;

    const float fontSize = 12.0f;
    const float lineBaseHeight = 14.0f;
    const float maxPanelWidth = static_cast<float>((std::max)(120, (std::min)(screenWidth - 24, 620)));
    const float maxTextWidth = (std::max)(60.0f, maxPanelWidth - 18.0f);
    std::vector<UiTextLine> lines;
    lines.reserve(sourceLines.size());
    float widest = 0.0f;

    ImFont* font = ImGui::GetFont();
    for (size_t i = 0; i < sourceLines.size(); ++i) {
        std::string line = WideToUtf8ForUi(sourceLines[i]);
        const ImVec2 unwrapped = font->CalcTextSizeA(fontSize, 1000000.0f, 0.0f,
            line.c_str(), line.c_str() + line.size());
        const ImVec2 wrapped = font->CalcTextSizeA(fontSize, maxTextWidth, maxTextWidth,
            line.c_str(), line.c_str() + line.size());
        widest = (std::max)(widest, (std::min)(unwrapped.x, maxTextWidth));
        const float height = (std::max)(lineBaseHeight, wrapped.y);
        lines.push_back({ std::move(line), i == 0 ? IM_COL32(185, 220, 255, 255) : IM_COL32(245, 245, 245, 255), height });
    }

    const float panelWidth = (std::min)(maxPanelWidth, (std::max)(150.0f, widest + 18.0f));
    const float textWidth = (std::max)(40.0f, panelWidth - 16.0f);
    float contentHeight = 0.0f;
    for (auto& line : lines) {
        const ImVec2 wrapped = font->CalcTextSizeA(fontSize, textWidth, textWidth,
            line.text.c_str(), line.text.c_str() + line.text.size());
        line.height = (std::max)(lineBaseHeight, wrapped.y);
        contentHeight += line.height;
    }
    const float panelHeight = contentHeight + 12.0f;
    const float left = 12.0f;
    const float top = alignTop
        ? 12.0f
        : (std::max)(8.0f, (static_cast<float>(screenHeight) - panelHeight) / 2.0f);

    // The near-black fill at alpha 165/255 looked too opaque in-game.
    // Lower alpha leaves more of the match visible while retaining readable text.
    AddPanel(draw, ImVec2(left, top), ImVec2(left + panelWidth, top + panelHeight),
        IM_COL32(8, 8, 12, 100), IM_COL32(90, 115, 145, 155));

    float y = top + 5.0f;
    for (const auto& line : lines) {
        if (!line.text.empty())
            draw->AddText(ImGui::GetFont(), fontSize, ImVec2(left + 8.0f, y), line.color,
                line.text.c_str(), line.text.c_str() + line.text.size(), textWidth);
        y += line.height;
    }
}

void DrawInputPanel(ImDrawList* draw, int screenWidth, int screenHeight, const std::wstring& inputLine)
{
    if (screenWidth <= 24 || screenHeight <= 24 || inputLine.empty())
        return;

    const std::string inputUtf8 = WideToUtf8ForUi(inputLine);
    const std::string help = "1~9 / . / : / BS / Enter / Ctrl+V";
    // The atlas font is baked at 16 px. Drawing it at 12 px resamples the glyphs
    // and makes this small UI look softer and smaller than the old OIOv2 panel.
    // Draw at the baked size so the glyphs stay crisp, and give the second line
    // enough vertical room.
    const float fontSize = 16.0f;
    const float maxPanelWidth = static_cast<float>((std::max)(120, (std::min)(screenWidth - 24, 520)));
    ImFont* font = ImGui::GetFont();
    const float measured = (std::max)(
        font->CalcTextSizeA(fontSize, 1000000.0f, 0.0f, inputUtf8.c_str(), inputUtf8.c_str() + inputUtf8.size()).x,
        font->CalcTextSizeA(fontSize, 1000000.0f, 0.0f, help.c_str(), help.c_str() + help.size()).x);
    const float panelWidth = (std::min)(maxPanelWidth, (std::max)(220.0f, measured + 20.0f));
    const float panelHeight = 54.0f;
    const float left = 12.0f;
    const float top = (std::max)(8.0f, static_cast<float>(screenHeight) - panelHeight - 14.0f);

    AddPanel(draw, ImVec2(left, top), ImVec2(left + panelWidth, top + panelHeight),
        IM_COL32(10, 10, 10, 175), IM_COL32(90, 145, 110, 200));
    draw->AddText(ImGui::GetFont(), fontSize, ImVec2(left + 8.0f, top + 4.0f),
        IM_COL32(255, 255, 255, 255), inputUtf8.c_str());
    draw->AddText(ImGui::GetFont(), fontSize, ImVec2(left + 8.0f, top + 29.0f),
        IM_COL32(190, 220, 200, 255), help.c_str());
}

void DrawWarningPanel(ImDrawList* draw, int screenWidth, int screenHeight, int warningState, bool blink)
{
    if (warningState == 0 || screenWidth <= 32 || screenHeight <= 32)
        return;

    const bool notRunning = warningState == static_cast<int>(TskWarningState::NotRunning);
    const std::string title = notRunning
        ? WideToUtf8ForUi(L"天則観（tsk.exe）が起動していません！")
        : WideToUtf8ForUi(L"天則観が非想天則を検知していません");
    const std::string subtitle = notRunning
        ? WideToUtf8ForUi(L"天則観を起動してから対戦してください")
        : WideToUtf8ForUi(L"非想天則を再起動してください");
    const ImU32 flash = blink ? IM_COL32(255, 55, 55, 255) : IM_COL32(255, 210, 40, 255);
    const float titleSize = 22.0f;
    const float subtitleSize = 16.0f;
    const float textWidth = (std::max)(160.0f, static_cast<float>(screenWidth) - 44.0f);
    const ImVec2 titleExtent = ImGui::GetFont()->CalcTextSizeA(titleSize, textWidth, textWidth,
        title.c_str(), title.c_str() + title.size());
    const ImVec2 subtitleExtent = ImGui::GetFont()->CalcTextSizeA(subtitleSize, textWidth, textWidth,
        subtitle.c_str(), subtitle.c_str() + subtitle.size());
    const float panelWidth = (std::min)(static_cast<float>(screenWidth - 20),
        (std::max)(320.0f, (std::max)(titleExtent.x, subtitleExtent.x) + 36.0f));
    const float panelHeight = 102.0f;
    const float left = (std::max)(10.0f, (static_cast<float>(screenWidth) - panelWidth) / 2.0f);
    const float top = (std::max)(10.0f, (static_cast<float>(screenHeight) - panelHeight) / 2.0f);
    const ImU32 background = notRunning ? IM_COL32(12, 12, 18, 255) : IM_COL32(14, 14, 22, 145);

    AddPanel(draw, ImVec2(left, top), ImVec2(left + panelWidth, top + panelHeight), background, flash);
    draw->AddText(ImGui::GetFont(), titleSize, ImVec2(left + 12.0f, top + 15.0f), flash,
        title.c_str(), title.c_str() + title.size(), panelWidth - 24.0f);
    draw->AddText(ImGui::GetFont(), subtitleSize, ImVec2(left + 12.0f, top + 59.0f),
        IM_COL32(255, 255, 255, 255), subtitle.c_str(), subtitle.c_str() + subtitle.size(), panelWidth - 24.0f);
}

void RenderGameUi(int screenWidth, int screenHeight)
{
    // This callback runs on the game render thread. The snapshot is produced by
    // OverlayThread; avoid reading its non-atomic HWND there. The UI is drawn only
    // into the game's own swap chain, so it cannot become a desktop overlay.
    RenderSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(g_renderSnapshotMutex);
        snapshot = g_renderSnapshot;
    }

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw)
        return;

    // Render in this order so the TSK warning remains visible above the other panels.
    if (snapshot.titleHistoryVisible && snapshot.sceneId == 2) {
        DrawInfoPanel(draw, screenWidth, screenHeight, snapshot.titleHistoryLines, true);
    } else if (snapshot.overlayEnabled) {
        DrawInfoPanel(draw, screenWidth, screenHeight, snapshot.infoLines);
    }
    if (snapshot.reversalCounterVisible && (snapshot.sceneId == 13 || snapshot.sceneId == 14))
        DrawInfoPanel(draw, screenWidth, screenHeight, snapshot.reversalCounterLines, true);
    if (snapshot.inputVisible)
        DrawInputPanel(draw, screenWidth, screenHeight, snapshot.inputLine);
    DrawWarningPanel(draw, screenWidth, screenHeight, snapshot.warningState, snapshot.warningBlink);
}

void RefreshTitleHistory()
{
    std::vector<std::wstring> lines;
    lines.push_back(L"【連戦履歴：直近10回】");
    if (!g_database || !g_database->IsAvailable()) {
        lines.push_back(L"天則観の対戦記録を読み込めません");
        g_titleHistoryLines = std::move(lines);
        return;
    }

    std::vector<TskRunStats> runs;
    if (!g_database->GetRecentRuns(10, 30, runs)) {
        lines.push_back(L"連戦履歴の読み込みに失敗しました");
    } else if (runs.empty()) {
        lines.push_back(L"表示できる連戦履歴がありません");
    } else {
        for (const auto& run : runs)
            lines.push_back(RunLine(run, true));
    }
    g_titleHistoryLines = std::move(lines);
}

void RegisterReversalCounter()
{
    if (g_sceneId != 13 && g_sceneId != 14)
        return;

    const uintptr_t offset = g_clientMode ? kLeftProfileOffset : kRightProfileOffset;
    std::string opponent = ReadProfile(offset);
    if (opponent.empty())
        opponent = g_opponentProfile;
    if (opponent.empty())
        opponent = "";

    std::wstring opponentWide = opponent.empty() ? L"不明" : Cp932ToWide(opponent);
    const std::wstring line = CurrentCounterTimestamp() + L"_[" + opponentWide + L"]";
    g_reversalCounterVisible = true;
    g_reversalCounterStatus.clear();

    if (!AppendCounterText(line)) {
        g_reversalCounterStatus = L"RLcounter.txt への保存に失敗しました";
        OutputDebugStringW(L"[OIOverlay] Failed to append RLcounter.txt next to the module.\n");
        return;
    }

    ++g_reversalCounterCount;
    g_reversalCounterHistory.push_back(line);
    if (g_reversalCounterHistory.size() > 50)
        g_reversalCounterHistory.erase(g_reversalCounterHistory.begin(), g_reversalCounterHistory.end() - 50);
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
}
}
}

int __fastcall HookBattleManagerOnProcess(SokuLib::BattleManager* self)
{
    // ToggleKey is polled only by OverlayThread. Keep this compatibility hook
    // pass-through to preserve the existing module setup.
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
    // The keyboard hook is diagnostic only; key polling remains active independently
    // of BattleManager so the SceneID 2 IP helper works on the title/menu screen.
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
    g_lastPageFlip = Clock::now();
    g_lastWindowSearch = Clock::time_point{};

    PublishRenderSnapshot();

    while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0) {
        const auto now = Clock::now();

        // Poll keys outside the BattleManager hook, so this still works in the
        // title/menu and full-screen battle scenes. F10 has dedicated actions on
        // SceneID 2 and 13/14; those actions must not also toggle the info panel.
        const bool toggleDown = g_config.toggleKey != 0 &&
            (GetAsyncKeyState(static_cast<int>(g_config.toggleKey)) & 0x8000) != 0;
        const bool togglePressed = toggleDown && !g_previousAsyncF10Down;
        const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const bool f10Pressed = f10Down && !g_previousF10Down;
        const int currentScene = g_sceneId.load();
        const bool gameForeground = IsGameForeground();

        if (gameForeground && f10Pressed && currentScene == 2) {
            g_titleHistoryVisible = !g_titleHistoryVisible;
            if (g_titleHistoryVisible)
                RefreshTitleHistory();
        } else if (gameForeground && f10Pressed && (currentScene == 13 || currentScene == 14)) {
            RegisterReversalCounter();
        } else if (gameForeground && togglePressed && currentScene > 7 &&
            currentScene != 13 && currentScene != 14 && currentScene != 2) {
            const bool enable = !g_overlayEnabled.load();
            g_overlayEnabled.store(enable);
            if (!enable)
                g_overlayManuallyDisabledForSession = true;
            else {
                g_displayPage = 0;
                g_lastPageFlip = now;
            }
            UpdateInfoText();
        }
        g_previousAsyncF10Down = toggleDown;
        g_previousF10Down = f10Down;

        if ((IsNormalInfoScene(currentScene) || currentScene == 12 || currentScene == 15) &&
            now - g_lastPageFlip >= std::chrono::seconds(1)) {
            g_displayPage = 1 - g_displayPage;
            g_lastPageFlip = now;
            UpdateInfoText();
        }

        if (now - g_lastWindowSearch >= std::chrono::milliseconds(250) || !g_gameWindow) {
            HWND found = FindGameWindow();
            if (found != g_gameWindow) {
                g_gameWindow = found;
                OIOverlayImGui::SetTargetWindow(found);
            }
            g_lastWindowSearch = now;
        }

        // DirectX hooking is deliberately NOT attempted from this worker thread.
        // It is installed by the game-setup call hook after Soku's own D3D9
        // initialization has returned (see InstallBootstrap in ImGuiRenderer.cpp).

        const bool canShow = g_gameWindow && !IsIconic(g_gameWindow) && IsGameForeground();
        if (!canShow) {
            PublishRenderSnapshot();
            Sleep(35);
            continue;
        }

        if (now - g_lastKeyboardHookCheck >= std::chrono::seconds(2)) {
            if (!g_keyboardHook)
                InstallKeyboardHook();
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
                }
            g_lastTskCheck = now;
        }

        const bool sceneChanged = UpdateSceneState();
        if (sceneChanged) {
            if (IsNormalInfoScene(g_sceneId) || g_sceneId == 12 || g_sceneId == 15)
                RefreshStats(true);
            UpdateInfoText();
            UpdateInputText();
            g_lastCharacterRefresh = now;
        }

        // Keep existing IP parsing/copy/paste behavior, including the Alt+Enter guard.
        PollIpInputFromGameFrame();
        UpdateInputText();

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
        }

        PublishRenderSnapshot();
        Sleep(8);
    }

    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    OIOverlayImGui::Shutdown();
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
g_config = LoadOverlayConfig(hMyModule);
g_overlayEnabled.store(g_config.defaultVisible);
if (!g_config.playersDbPath.empty())
g_playersDatabase = std::make_unique<CombinedPlayersDatabase>(g_config.playersDbPath);
if (!InstallBattleManagerProcessHook())
return false;
// The bootstrap only patches a verified x86 CALL site. Actual D3D9 device
// hooking happens after the game's original setup routine has returned.
if (!OIOverlayImGui::InstallBootstrap(RenderGameUi)) {
    OutputDebugStringW(L"[OIOverlay] D3D9 bootstrap hook unavailable; in-game drawing is disabled, core module remains loaded.\n");
}
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