// OIOverlay - OIViewer-style profile/record overlay for Touhou Hisoutensoku.
#ifndef NOMINMAX
#define NOMINMAX
#endif
// UI is drawn into Hisoutensoku's Direct3D 9 frame (no persistent overlay windows).
// This also allows the SceneID 2 IP helper and TSK warning to be shown outside battle.
// Default.db is opened READ ONLY. This module never writes to it.

#include <Windows.h>
#include <mmsystem.h>
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
enum class ConnectionRole { Unknown, Host, Client };
ConnectionRole g_connectionRole = ConnectionRole::Unknown;
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
bool g_reversalCounterLiveVisible = false;
bool g_reversalCounterSummaryVisible = false;
int g_reversalCounterSummaryCount = 0;
std::wstring g_reversalCounterStatus;
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
std::wstring g_inputBuffer;
size_t g_inputCursor = 0;
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
// A single F10 is a candidate RL count. A second F10 within three seconds
// converts that candidate into a clip marker instead of a count.
bool g_reversalCounterPending = false;
bool g_reversalCounterPendingClipEligible = false;
Clock::time_point g_reversalCounterPendingSince{};
enum class ReversalSessionState { None, Battle, Spectator };
ReversalSessionState g_reversalSessionState = ReversalSessionState::None;
std::string g_reversalBattleOpponentProfile;

std::mutex g_reversalCounterMutex;
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
void RegisterReversalCounter(Clock::time_point now);
void ConfirmPendingReversalCounter();
bool AppendReversalSummary(int count);

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
        std::wostringstream out;
        out << std::setfill(L'0') << std::setw(2) << month << L"/"
            << std::setw(2) << day;
        return out.str();
    }
    return value.empty() ? L"--/--" : NarrowAsciiToWide(value);
}

std::wstring RunLine(const TskRunStats& run, bool includeProfile)
{
    std::wostringstream out;
    out << ShortRunDate(run.endDateText) << L"  "
        << std::setw(2) << run.wins << L"勝 "
        << std::setw(2) << run.losses << L"敗　("
        << RateText(run.wins, run.losses) << L")";
    if (includeProfile)
        out << L"　[" << ProfileDisplayName(run.profileNameCp932) << L"]";
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

std::wstring CurrentCounterDate()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream out;
    out << now.wYear << L"/" << now.wMonth << L"/" << now.wDay;
    return out.str();
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
    std::wstring text = L"【対戦中】\n\n";
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
    std::wstring text = L"【観戦中】\n\n";
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

bool SetClipboardText(const std::wstring& text)
{
    if (text.empty()) {
        g_clipboardError.store(ERROR_SUCCESS);
        return false;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
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
    std::memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
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
    // Ownership transfers to the clipboard on success.
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

std::wstring GetInputBufferSnapshot()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    return g_inputBuffer;
}

void ClearInputBuffer()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_inputBuffer.clear();
    g_inputCursor = 0;
}

void AppendIpCharacter(char character)
{
    constexpr size_t kMaximumInputLength = 64;
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_copiedFeedback = false;
    if (g_inputBuffer.size() < kMaximumInputLength && g_inputCursor <= g_inputBuffer.size()) {
        g_inputBuffer.insert(g_inputCursor, 1, static_cast<wchar_t>(static_cast<unsigned char>(character)));
        ++g_inputCursor;
    }
}

bool IsHighSurrogate(wchar_t value)
{
    return value >= 0xD800 && value <= 0xDBFF;
}

bool IsLowSurrogate(wchar_t value)
{
    return value >= 0xDC00 && value <= 0xDFFF;
}

void MoveIpCursorLeft()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_copiedFeedback = false;
    if (g_inputCursor > 0) {
        --g_inputCursor;
        if (g_inputCursor > 0 && g_inputCursor < g_inputBuffer.size() &&
            IsLowSurrogate(g_inputBuffer[g_inputCursor]) &&
            IsHighSurrogate(g_inputBuffer[g_inputCursor - 1])) {
            --g_inputCursor;
        }
    }
}

void MoveIpCursorRight()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_copiedFeedback = false;
    if (g_inputCursor < g_inputBuffer.size()) {
        if (IsHighSurrogate(g_inputBuffer[g_inputCursor]) &&
            g_inputCursor + 1 < g_inputBuffer.size() &&
            IsLowSurrogate(g_inputBuffer[g_inputCursor + 1])) {
            g_inputCursor += 2;
        } else {
            ++g_inputCursor;
        }
    }
}

void RemoveIpCharacterBeforeCursor()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_copiedFeedback = false;
    if (g_inputCursor > 0 && g_inputCursor <= g_inputBuffer.size()) {
        size_t eraseStart = g_inputCursor - 1;
        size_t eraseLength = 1;
        if (IsLowSurrogate(g_inputBuffer[eraseStart]) && eraseStart > 0 &&
            IsHighSurrogate(g_inputBuffer[eraseStart - 1])) {
            --eraseStart;
            eraseLength = 2;
        }
        g_inputBuffer.erase(eraseStart, eraseLength);
        g_inputCursor = eraseStart;
    }
}

void RemoveIpCharacterAtCursor()
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_copiedFeedback = false;
    if (g_inputCursor < g_inputBuffer.size()) {
        const size_t eraseLength = IsHighSurrogate(g_inputBuffer[g_inputCursor]) &&
            g_inputCursor + 1 < g_inputBuffer.size() &&
            IsLowSurrogate(g_inputBuffer[g_inputCursor + 1]) ? 2 : 1;
        g_inputBuffer.erase(g_inputCursor, eraseLength);
    }
}

void CopyInputToClipboardAndClear()
{
    const std::wstring value = GetInputBufferSnapshot();
    if (value.empty()) {
        // Empty Enter is intentionally a no-op: do not touch the clipboard.
        g_clipboardStatus.store(0);
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_copiedFeedback = false;
        return;
    }

    if (SetClipboardText(value)) {
        g_clipboardStatus.store(1);
        ClearInputBuffer();
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_copiedFeedback = true;
        g_copiedFeedbackUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    } else {
        // Preserve text and caret if Windows denies clipboard access.
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
    const std::wstring clipboardText(source);
    GlobalUnlock(clipboardData);
    CloseClipboard();

    // Prefer a valid IPv4:port token wherever one appears in the clipboard.
    // If none exists, preserve the original input contract: only ASCII digits,
    // dots and colons are accepted, in their original order. Do not inject
    // arbitrary Unicode or other punctuation into the IP:Port field.
    std::string normalized;
    std::wstring replacement;
    if (ExtractFirstValidIpPort(clipboardText, normalized)) {
        replacement.assign(normalized.begin(), normalized.end());
    } else {
        replacement.reserve(clipboardText.size());
        for (wchar_t ch : clipboardText) {
            if ((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L':')
                replacement.push_back(ch);
        }
    }

    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_copiedFeedback = false;
    g_inputBuffer = std::move(replacement);
    g_inputCursor = g_inputBuffer.size();
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

bool CaptureBattleOpponentProfile()
{
    uintptr_t offset = kRightProfileOffset;
    if (g_connectionRole == ConnectionRole::Client)
        offset = kLeftProfileOffset;
    else if (g_connectionRole == ConnectionRole::Unknown)
        offset = kRightProfileOffset; // Scene 13/14 without a preceding Scene 9 is host-side.

    const std::string opponent = ReadProfile(offset);
    if (opponent.empty())
        return false;
    g_opponentProfile = opponent;
    g_reversalBattleOpponentProfile = opponent;
    return true;
}

bool AppendReversalSummary(int count)
{
    const std::string profile = !g_reversalBattleOpponentProfile.empty()
        ? g_reversalBattleOpponentProfile : g_opponentProfile;
    const std::wstring opponent = profile.empty() ? L"不明" : ProfileDisplayName(profile);
    const std::wstring line = CurrentCounterDate() + L"_RL: " + std::to_wstring(count) + L"_[" + opponent + L"]";
    return AppendCounterText(line);
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

{
    std::lock_guard<std::mutex> counterLock(g_reversalCounterMutex);
    const bool oldWasBattleScene = (oldScene == 13 || oldScene == 14);
    const bool newBattleScene = (scene == 13 || scene == 14);
    const bool newSpectatorScene = (scene == 12 || scene == 15);

    // Spectator mode is not a reversal-counter session. If the game returns
    // from spectator mode to <=7, do not show or write a battle result.
    if (newSpectatorScene) {
        g_reversalSessionState = ReversalSessionState::Spectator;
        g_reversalCounterPending = false;
        g_reversalCounterPendingClipEligible = false;
        g_reversalCounterLiveVisible = false;
        g_reversalCounterSummaryVisible = false;
        g_reversalCounterCount = 0;
        g_reversalCounterSummaryCount = 0;
        g_reversalCounterStatus.clear();
        g_reversalBattleOpponentProfile.clear();
    } else if (scene <= 7) {
        if (g_reversalSessionState == ReversalSessionState::Battle && oldScene > 7) {
            // Only a real battle -> <=7 transition finalizes and logs the run.
            // This intentionally includes RL: 0.
            if (g_reversalCounterPending)
                ConfirmPendingReversalCounter();
            g_reversalCounterSummaryCount = g_reversalCounterCount;
            g_reversalCounterSummaryVisible = true;
            g_reversalCounterLiveVisible = false;
            if (!AppendReversalSummary(g_reversalCounterCount)) {
                g_reversalCounterStatus = L"RLcounter.txt への保存に失敗しました";
                OutputDebugStringW(L"[OIOverlay] Failed to append RL summary to RLcounter.txt.\n");
            }
        } else {
            // In particular, spectator -> <=7 must not leave an RL result behind.
            g_reversalCounterPending = false;
            g_reversalCounterPendingClipEligible = false;
            g_reversalCounterLiveVisible = false;
            g_reversalCounterSummaryVisible = false;
            g_reversalCounterCount = 0;
            g_reversalCounterSummaryCount = 0;
            g_reversalCounterStatus.clear();
        }
        g_reversalSessionState = ReversalSessionState::None;
        g_reversalBattleOpponentProfile.clear();
    } else if (newBattleScene && g_reversalSessionState != ReversalSessionState::Battle) {
        // A battle entered from title/menu or spectator starts a fresh RL session.
        g_reversalSessionState = ReversalSessionState::Battle;
        g_reversalCounterCount = 0;
        g_reversalCounterSummaryCount = 0;
        g_reversalCounterSummaryVisible = false;
        g_reversalCounterLiveVisible = true;
        g_reversalCounterStatus.clear();
        g_reversalCounterPending = false;
        g_reversalCounterPendingClipEligible = false;
        g_reversalBattleOpponentProfile.clear();
        CaptureBattleOpponentProfile();
    } else if (newBattleScene) {
        // Scene 13 -> 14 (or 14 -> 13) is the same battle; preserve the count.
        CaptureBattleOpponentProfile();
    }

    // A double-tap can only mark a clip while still in the same battle scene.
    if (oldWasBattleScene && !newBattleScene && g_reversalCounterPending)
        g_reversalCounterPendingClipEligible = false;
}

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

    if (scene <= 7) {
        g_overlayManuallyDisabledForSession = false;
    } else if ((scene == 8 || scene == 9 || scene == 12) && oldScene <= 7) {
        if (!g_overlayManuallyDisabledForSession) {
            g_overlayEnabled.store(g_config.defaultVisible);
        }
    }

if (scene <= 7) {
g_clientMode = false;
g_connectionRole = ConnectionRole::Unknown;
g_opponentProfile.clear();
g_normalStats = {};
g_normalStatsValid = false;
g_cachedNormalProfile.clear();
} else if (scene == 9) {
g_clientMode = true;
g_connectionRole = ConnectionRole::Client;
} else if ((scene == 8 || scene == 10 || scene == 11) && g_connectionRole == ConnectionRole::Unknown) {
// Scene 8/10/11 do not reliably identify the side after a previous match.
// Only infer host when no earlier Scene 9 established client mode.
g_clientMode = false;
g_connectionRole = ConnectionRole::Host;
}

if (scene == 2) {
ClearInputBuffer();
g_inputEnabled = true;
} else {
ClearInputBuffer();
g_inputEnabled = false;
}

if (scene == 12 || scene == 15) {
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
const uintptr_t offset = g_connectionRole == ConnectionRole::Client ? kLeftProfileOffset : kRightProfileOffset;
const std::string opponent = ReadProfile(offset);
if (!opponent.empty() && opponent != g_opponentProfile) {
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

bool CanFlipToSecondaryPage(int scene)
{
    if (IsNormalInfoScene(scene)) {
        // A first-time opponent (or a run hidden as still active) has no
        // meaningful page-2 run history; keep the compact primary page stable.
        return g_normalStatsValid && g_normalStats.found &&
            g_normalStats.totalMatches > 0 && !g_normalStats.recentRuns.empty();
    }
    if (scene == 12 || scene == 15)
        return g_watchP1StatsValid || g_watchP2StatsValid;
    return false;
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
        if (g_copiedFeedback) {
            next = L"IP:Port ＞ Copied";
        } else {
            next = L"IP:Port ＞ ";
            const size_t cursor = (std::min)(g_inputCursor, g_inputBuffer.size());
            next.append(g_inputBuffer, 0, cursor);
            next += L"_";
            next.append(g_inputBuffer, cursor, std::wstring::npos);
        }
    }
    if (next != g_inputLine) {
        g_inputLine = std::move(next);
    }
}

struct RenderSnapshot
{
    std::vector<std::wstring> infoLines;
    std::vector<std::wstring> alternateInfoLines;
    std::vector<std::wstring> titleHistoryLines;
    std::wstring inputLine;
    int sceneId = -1;
    int warningState = 0;
    bool overlayEnabled = true;
    bool inputVisible = false;
    bool warningBlink = false;
    bool titleHistoryVisible = false;
    bool secondaryInfoPage = false;
    bool reversalCounterLiveVisible = false;
    bool reversalCounterSummaryVisible = false;
    int reversalCounterCount = 0;
    int reversalCounterSummaryCount = 0;
    std::wstring reversalCounterStatus;
};

std::mutex g_renderSnapshotMutex;
RenderSnapshot g_renderSnapshot;

void PublishRenderSnapshot()
{
    RenderSnapshot next;
    next.infoLines = g_infoLines;
    if (g_overlayEnabled && (IsNormalInfoScene(g_sceneId) || g_sceneId == 12 || g_sceneId == 15)) {
        std::vector<std::wstring> alternate;
        if (IsNormalInfoScene(g_sceneId))
            SplitLines(g_displayPage == 0 ? BuildNormalSecondaryInfo() : BuildNormalInfo(), alternate);
        else
            SplitLines(g_displayPage == 0 ? BuildWatchingSecondaryInfo() : BuildWatchingInfo(), alternate);
        next.alternateInfoLines = std::move(alternate);
    }
    next.titleHistoryLines = g_titleHistoryLines;
    next.titleHistoryVisible = g_titleHistoryVisible;
    next.secondaryInfoPage = g_displayPage != 0;
    {
        std::lock_guard<std::mutex> counterLock(g_reversalCounterMutex);
        next.reversalCounterLiveVisible = g_reversalCounterLiveVisible;
        next.reversalCounterSummaryVisible = g_reversalCounterSummaryVisible;
        next.reversalCounterCount = g_reversalCounterCount;
        next.reversalCounterSummaryCount = g_reversalCounterSummaryCount;
        next.reversalCounterStatus = g_reversalCounterStatus;
    }
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

void DrawInfoPanel(
    ImDrawList* draw,
    int screenWidth,
    int screenHeight,
    const std::vector<std::wstring>& sourceLines,
    const std::vector<std::wstring>* stableLayoutLines = nullptr,
    bool alignTop = false,
    bool opaqueBackground = false)
{
    if (sourceLines.empty() || screenWidth <= 24 || screenHeight <= 24)
        return;

    const float fontSize = 12.0f;
    const float lineBaseHeight = 14.0f;
    const float maxPanelWidth = static_cast<float>((std::max)(120, (std::min)(screenWidth - 24, 620)));
    const float maxTextWidth = (std::max)(60.0f, maxPanelWidth - 18.0f);
    ImFont* font = ImGui::GetFont();

    auto prepareLines = [&](const std::vector<std::wstring>& source, std::vector<UiTextLine>& output, float& widest) {
        output.clear();
        output.reserve(source.size());
        for (size_t i = 0; i < source.size(); ++i) {
            std::string line = WideToUtf8ForUi(source[i]);
            const ImVec2 unwrapped = font->CalcTextSizeA(fontSize, 1000000.0f, 0.0f,
                line.c_str(), line.c_str() + line.size());
            widest = (std::max)(widest, (std::min)(unwrapped.x, maxTextWidth));
            output.push_back({ std::move(line), i == 0 ? IM_COL32(185, 220, 255, 255) : IM_COL32(245, 245, 245, 255), lineBaseHeight });
        }
    };

    std::vector<UiTextLine> lines;
    std::vector<UiTextLine> layoutLines;
    float widest = 0.0f;
    prepareLines(sourceLines, lines, widest);
    if (stableLayoutLines != nullptr && !stableLayoutLines->empty())
        prepareLines(*stableLayoutLines, layoutLines, widest);

    const float panelWidth = (std::min)(maxPanelWidth, (std::max)(150.0f, widest + 18.0f));
    const float textWidth = (std::max)(40.0f, panelWidth - 16.0f);
    auto contentHeightFor = [&](std::vector<UiTextLine>& measuredLines) {
        float result = 0.0f;
        for (auto& line : measuredLines) {
            const ImVec2 wrapped = font->CalcTextSizeA(fontSize, textWidth, textWidth,
                line.text.c_str(), line.text.c_str() + line.text.size());
            line.height = (std::max)(lineBaseHeight, wrapped.y);
            result += line.height;
        }
        return result;
    };

    const float currentContentHeight = contentHeightFor(lines);
    const float otherContentHeight = layoutLines.empty() ? 0.0f : contentHeightFor(layoutLines);
    // Both statistics pages share the same anchor and the larger of their
    // widths/heights, so flipping pages never makes the panel jump.
    const float panelHeight = (std::max)(currentContentHeight, otherContentHeight) + 12.0f;
    const float left = 12.0f;
    const float top = alignTop
        ? 12.0f
        : (std::max)(8.0f, (static_cast<float>(screenHeight) - panelHeight) / 2.0f);

    const bool useOpaqueBackground = alignTop || opaqueBackground;
    const ImU32 panelBackground = useOpaqueBackground
        ? IM_COL32(8, 8, 12, 255)  // Run-history pages fully cover text behind the panel.
        : IM_COL32(8, 8, 12, 100);
    const ImU32 panelBorder = useOpaqueBackground
        ? IM_COL32(90, 115, 145, 230)
        : IM_COL32(90, 115, 145, 155);
    AddPanel(draw, ImVec2(left, top), ImVec2(left + panelWidth, top + panelHeight),
        panelBackground, panelBorder);

    float y = top + 5.0f;
    for (const auto& line : lines) {
        if (!line.text.empty())
            draw->AddText(font, fontSize, ImVec2(left + 8.0f, y), line.color,
                line.text.c_str(), line.text.c_str() + line.text.size(), textWidth);
        y += line.height;
    }
}

void DrawInputPanel(ImDrawList* draw, int screenWidth, int screenHeight, const std::wstring& inputLine)
{
    if (screenWidth <= 24 || screenHeight <= 24 || inputLine.empty())
        return;

    const std::string inputUtf8 = WideToUtf8ForUi(inputLine);
    const std::string help = "0~9 / . / : / BSDel / Enter / CtrlV";
    const float fontSize = 16.0f;
    const float maxPanelWidth = static_cast<float>((std::max)(120, (std::min)(screenWidth - 24, 520)));
    ImFont* font = ImGui::GetFont();
    const ImVec2 inputUnwrapped = font->CalcTextSizeA(fontSize, 100000.0f, 0.0f,
        inputUtf8.c_str(), inputUtf8.c_str() + inputUtf8.size());
    const ImVec2 helpUnwrapped = font->CalcTextSizeA(fontSize, 100000.0f, 0.0f,
        help.c_str(), help.c_str() + help.size());
    const float panelWidth = (std::min)(maxPanelWidth,
        (std::max)(220.0f, (std::max)(inputUnwrapped.x, helpUnwrapped.x) + 20.0f));
    const float textWidth = (std::max)(60.0f, panelWidth - 16.0f);
    const ImVec2 inputWrapped = font->CalcTextSizeA(fontSize, textWidth, textWidth,
        inputUtf8.c_str(), inputUtf8.c_str() + inputUtf8.size());
    const float helpHeight = (std::max)(fontSize, helpUnwrapped.y);
    const float maxFieldHeight = (std::max)(fontSize, static_cast<float>(screenHeight) - helpHeight - 48.0f);
    const float inputHeight = (std::min)((std::max)(fontSize, inputWrapped.y), maxFieldHeight);
    const float panelHeight = (std::min)(inputHeight + helpHeight + 17.0f, static_cast<float>(screenHeight - 24));
    const float left = 12.0f;
    const float top = (std::max)(8.0f, static_cast<float>(screenHeight) - panelHeight - 14.0f);

    AddPanel(draw, ImVec2(left, top), ImVec2(left + panelWidth, top + panelHeight),
        IM_COL32(10, 10, 10, 175), IM_COL32(90, 145, 110, 200));
    const ImVec4 inputClip(left + 5.0f, top + 3.0f, left + panelWidth - 5.0f,
        (std::min)(top + 4.0f + inputHeight, top + panelHeight - helpHeight - 8.0f));
    draw->AddText(font, fontSize, ImVec2(left + 8.0f, top + 4.0f),
        IM_COL32(255, 255, 255, 255), inputUtf8.c_str(), inputUtf8.c_str() + inputUtf8.size(), textWidth, &inputClip);
    const float helpTop = (std::min)(top + 8.0f + inputHeight, top + panelHeight - helpHeight - 4.0f);
    draw->AddText(font, fontSize, ImVec2(left + 8.0f, helpTop),
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

void DrawLiveReversalCounter(ImDrawList* draw, int screenWidth, int count, const std::wstring& status)
{
    if (screenWidth <= 64)
        return;
    const float fontSize = 14.0f;
    const std::string label = "RL: " + std::to_string(count);
    ImFont* font = ImGui::GetFont();
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, 100000.0f, 0.0f,
        label.c_str(), label.c_str() + label.size());
    const float paddingX = 5.0f;
    const float paddingY = 3.0f;
    const float statusHeight = status.empty() ? 0.0f : 13.0f;
    const float panelWidth = textSize.x + paddingX * 2.0f;
    const float panelHeight = textSize.y + paddingY * 2.0f + statusHeight;
    const float left = static_cast<float>(screenWidth) - panelWidth - 12.0f;
    const float top = 12.0f;
    AddPanel(draw, ImVec2(left, top), ImVec2(left + panelWidth, top + panelHeight),
        IM_COL32(8, 8, 12, 125), IM_COL32(90, 115, 145, 175));
    draw->AddText(font, fontSize, ImVec2(left + paddingX, top + paddingY),
        IM_COL32(255, 255, 255, 255), label.c_str());
    if (!status.empty()) {
        const std::string statusUtf8 = WideToUtf8ForUi(status);
        draw->AddText(font, 9.0f, ImVec2(left + paddingX, top + paddingY + textSize.y),
            IM_COL32(255, 130, 130, 255), statusUtf8.c_str());
    }
}

void DrawReversalCounterSummary(ImDrawList* draw, int screenWidth, int screenHeight, int count)
{
    if (screenWidth <= 24 || screenHeight <= 24)
        return;
    const std::string label = "RL: " + std::to_string(count);
    const std::string hint = "F10: 非表示";
    const float fontSize = 48.0f;
    const float hintFontSize = 10.0f;
    ImFont* font = ImGui::GetFont();
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, 100000.0f, 0.0f,
        label.c_str(), label.c_str() + label.size());
    const ImVec2 hintSize = font->CalcTextSizeA(hintFontSize, 100000.0f, 0.0f,
        hint.c_str(), hint.c_str() + hint.size());
    const float paddingX = 28.0f;
    const float paddingY = 18.0f;
    const float hintGap = 3.0f;
    const float panelWidth = (std::max)(textSize.x, hintSize.x) + paddingX * 2.0f;
    const float panelHeight = textSize.y + paddingY * 2.0f + hintGap + hintSize.y;
    const float left = (static_cast<float>(screenWidth) - panelWidth) / 2.0f;
    const float top = (static_cast<float>(screenHeight) - panelHeight) / 2.0f;
    AddPanel(draw, ImVec2(left, top), ImVec2(left + panelWidth, top + panelHeight),
        IM_COL32(8, 8, 12, 165), IM_COL32(125, 165, 205, 220));
    const float textLeft = left + (panelWidth - textSize.x) / 2.0f;
    draw->AddText(font, fontSize, ImVec2(textLeft, top + paddingY),
        IM_COL32(255, 255, 255, 255), label.c_str());
    const float hintLeft = left + (panelWidth - hintSize.x) / 2.0f;
    draw->AddText(font, hintFontSize,
        ImVec2(hintLeft, top + paddingY + textSize.y + hintGap),
        IM_COL32(205, 215, 230, 235), hint.c_str());
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
        DrawInfoPanel(draw, screenWidth, screenHeight, snapshot.titleHistoryLines, nullptr, true);
    } else if (snapshot.overlayEnabled) {
        DrawInfoPanel(draw, screenWidth, screenHeight, snapshot.infoLines,
            snapshot.alternateInfoLines.empty() ? nullptr : &snapshot.alternateInfoLines,
            false, snapshot.secondaryInfoPage);
    }
    if (snapshot.reversalCounterLiveVisible &&
        (snapshot.sceneId == 8 || snapshot.sceneId == 9 || snapshot.sceneId == 10 ||
         snapshot.sceneId == 11 || snapshot.sceneId == 13 || snapshot.sceneId == 14))
        DrawLiveReversalCounter(draw, screenWidth, snapshot.reversalCounterCount, snapshot.reversalCounterStatus);
    if (snapshot.reversalCounterSummaryVisible && snapshot.sceneId <= 7 &&
        !(snapshot.sceneId == 2 && snapshot.titleHistoryVisible))
        DrawReversalCounterSummary(draw, screenWidth, screenHeight, snapshot.reversalCounterSummaryCount);
    if (snapshot.inputVisible)
        DrawInputPanel(draw, screenWidth, screenHeight, snapshot.inputLine);
    DrawWarningPanel(draw, screenWidth, screenHeight, snapshot.warningState, snapshot.warningBlink);
}

void RefreshTitleHistory()
{
    std::vector<std::wstring> lines;
    lines.push_back(L"【連戦履歴】");
    if (!g_database || !g_database->IsAvailable()) {
        lines.push_back(L"天則観の対戦記録を読み込めません");
        g_titleHistoryLines = std::move(lines);
        return;
    }

    std::vector<TskRunStats> runs;
    if (!g_database->GetRecentRuns(20, 0, runs)) {
        lines.push_back(L"連戦履歴の読み込みに失敗しました");
    } else if (runs.empty()) {
        lines.push_back(L"表示できる連戦履歴がありません");
    } else {
        for (const auto& run : runs)
            lines.push_back(RunLine(run, true));
    }
    g_titleHistoryLines = std::move(lines);
}

void PlayReversalNoticeIfAvailable()
{
    // OIOverlay is loaded into th123.exe, so WinMM plays the optional WAV from
    // the game's process without launching another application/window.
    // A missing file is normal and intentionally silent.
    const std::wstring directory = OverlayModuleDirectory();
    if (directory.empty())
        return;
    const std::wstring wavPath = directory + L"\\RLnotice.wav";
    if (!FileExists(wavPath))
        return;
    if (!PlaySoundW(wavPath.c_str(), nullptr,
        SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
        OutputDebugStringW(L"[OIOverlay] RLnotice.wav exists but could not be played.\n");
    }
}

void ConfirmPendingReversalCounter()
{
    if (!g_reversalCounterPending)
        return;

    g_reversalCounterPending = false;
    g_reversalCounterPendingClipEligible = false;
    // The count was already incremented on the first press. Confirmation only
    // plays the optional notice; it must not touch RLcounter.txt.
    PlayReversalNoticeIfAvailable();
}

void UpdatePendingReversalCounter(Clock::time_point now)
{
    std::lock_guard<std::mutex> counterLock(g_reversalCounterMutex);
    if (g_reversalCounterPending &&
        now - g_reversalCounterPendingSince >= std::chrono::seconds(3)) {
        ConfirmPendingReversalCounter();
    }
}

void RegisterReversalCounter(Clock::time_point now)
{
    std::lock_guard<std::mutex> counterLock(g_reversalCounterMutex);
    if (g_sceneId != 13 && g_sceneId != 14)
        return;
    if (g_reversalSessionState != ReversalSessionState::Battle)
        return;

    std::string opponent = g_reversalBattleOpponentProfile;
    if (opponent.empty())
        opponent = g_opponentProfile;
    if (opponent.empty())
        CaptureBattleOpponentProfile();
    if (opponent.empty())
        opponent = g_reversalBattleOpponentProfile;
    if (opponent.empty())
        opponent = g_opponentProfile;
    const std::wstring opponentWide = opponent.empty() ? L"不明" : Cp932ToWide(opponent);

    // A second press within three seconds is a clip marker, not an RL count.
    // Undo the candidate count from the first press, then append only this
    // second press's timestamp to RLcounter.txt.
    if (g_reversalCounterPending && g_reversalCounterPendingClipEligible &&
        now - g_reversalCounterPendingSince < std::chrono::seconds(3)) {
        if (g_reversalCounterCount > 0)
            --g_reversalCounterCount;
        g_reversalCounterPending = false;
        g_reversalCounterPendingClipEligible = false;
        const std::wstring line = CurrentCounterTimestamp() + L"_[" + opponentWide + L"]";
        g_reversalCounterStatus.clear();
        if (!AppendCounterText(line)) {
            g_reversalCounterStatus = L"RLcounter.txt への保存に失敗しました";
            OutputDebugStringW(L"[OIOverlay] Failed to append RLcounter.txt next to the module.\n");
        }
        g_reversalCounterLiveVisible = true;
        return;
    }

    // If the last candidate has expired (or belongs to a previous match),
    // confirm it before beginning a fresh candidate for this press.
    if (g_reversalCounterPending)
        ConfirmPendingReversalCounter();

    ++g_reversalCounterCount;
    g_reversalCounterLiveVisible = true;
    g_reversalCounterStatus.clear();
    g_reversalCounterPending = true;
    g_reversalCounterPendingClipEligible = true;
    g_reversalCounterPendingSince = now;
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
VK_BACK, VK_DELETE, VK_LEFT, VK_RIGHT, VK_RETURN, 'V'
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
    // Do not reinterpret Ctrl-based shortcuts as text/caret movement.
    continue;
} else if (vk == VK_LEFT) {
    MoveIpCursorLeft();
    handled = true;
} else if (vk == VK_RIGHT) {
    MoveIpCursorRight();
    handled = true;
} else if (vk == VK_BACK) {
    RemoveIpCharacterBeforeCursor();
    handled = true;
} else if (vk == VK_DELETE) {
    RemoveIpCharacterAtCursor();
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

        bool dismissedReversalSummary = false;
        if (gameForeground && f10Pressed && currentScene <= 7) {
            std::lock_guard<std::mutex> counterLock(g_reversalCounterMutex);
            if (g_reversalCounterSummaryVisible) {
                // While the post-match count is visible, F10 means "hide this result".
                // The title-history toggle remains available once the result is hidden.
                g_reversalCounterSummaryVisible = false;
                dismissedReversalSummary = true;
            }
        }
        if (!dismissedReversalSummary && gameForeground && f10Pressed && currentScene == 2) {
            g_titleHistoryVisible = !g_titleHistoryVisible;
            if (g_titleHistoryVisible)
                RefreshTitleHistory();
        } else if (!dismissedReversalSummary && gameForeground && f10Pressed &&
            (currentScene == 12 || currentScene == 15)) {
            // Spectator F10 only toggles the information overlay. It never touches RL.
            const bool enable = !g_overlayEnabled.load();
            g_overlayEnabled.store(enable);
            if (!enable)
                g_overlayManuallyDisabledForSession = true;
            else {
                g_displayPage = 0;
                g_lastPageFlip = now;
            }
            UpdateInfoText();
        } else if (!dismissedReversalSummary && gameForeground && f10Pressed &&
            (currentScene == 13 || currentScene == 14)) {
            RegisterReversalCounter(now);
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

        UpdatePendingReversalCounter(now);

        if ((IsNormalInfoScene(currentScene) || currentScene == 12 || currentScene == 15) &&
            now - g_lastPageFlip >= std::chrono::seconds(3)) {
            if (CanFlipToSecondaryPage(currentScene)) {
                g_displayPage = 1 - g_displayPage;
            } else {
                g_displayPage = 0;
            }
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

        // Once a battle starts, keep a stable opponent profile for RL logging.
        // Do not rely on later character-select SceneIDs to recover host/client side.
        if (g_reversalSessionState == ReversalSessionState::Battle &&
            (g_sceneId == 13 || g_sceneId == 14) &&
            g_reversalBattleOpponentProfile.empty()) {
            CaptureBattleOpponentProfile();
        }

        // Keep existing IP parsing/copy/paste behavior, including the Alt+Enter guard.
        PollIpInputFromGameFrame();
        UpdateInputText();

        if (g_overlayEnabled && IsNormalInfoScene(g_sceneId) &&
            (g_infoLines.empty() || now - g_lastCharacterRefresh >= std::chrono::seconds(1))) {
            const uintptr_t offset = g_clientMode ? kLeftProfileOffset : kRightProfileOffset;
            const std::string opponent = ReadProfile(offset);
            if (opponent != g_opponentProfile) {
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