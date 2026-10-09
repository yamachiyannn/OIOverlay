//
// OIOverlay
// Port of th123OIViewer features
//
// Features in this build:
//   * tsk.exe running warning
//   * read-only access to th4_5888/Default.db
//   * opponent profile detection from Hisoutensoku memory
//   * opponent statistics overlay
//   * menu SceneID IP:Port keyboard input
//   * Enter -> clipboard copy
//   * F10 -> overlay on/off
//
// The database is NEVER written to by this module.
//

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

#include <SokuLib.hpp>

#include "TskDatabase.hpp"


namespace GameMemory
{
    /*
     * Hisoutensoku 1.10a
     *
     * These are the same addresses used by the existing
     * th123OIViewer memory reader.
     */
    constexpr uintptr_t PNETOBJECT = 0x008986A0;
    constexpr uintptr_t SCENEID    = 0x008A0044;

    constexpr uintptr_t LPROFOFS = 0x04;
    constexpr uintptr_t RPROFOFS = 0x24;

    constexpr size_t PROFILE_SIZE = 0x20;
}


namespace OverlayPosition
{
    constexpr int WARNING_X = 18;
    constexpr int WARNING_Y = 12;

    constexpr int INFO_X = 18;
    constexpr int INFO_Y = 52;

    constexpr int INPUT_X = 18;
    constexpr int INPUT_Y = 410;
}


/*
 * ------------------------------------------------------------
 * Hooks / state
 * ------------------------------------------------------------
 */

static int (SokuLib::BattleManager::*ogBattleMgrOnProcess)();
static void (SokuLib::BattleManager::*ogBattleMgrOnRender)();

static bool initialized = false;
static bool overlayEnabled = true;
static bool previousF10State = false;

static int previousSceneId = -1;
static bool clientMode = false;

static std::string currentOpponentProfileCp932;
static std::string inputBuffer;

static TskStats currentStats;
static bool currentStatsValid = false;

static std::wstring databasePath;
static std::unique_ptr<TskDatabase> tskDatabase;

static bool tskRunning = false;

static std::chrono::steady_clock::time_point lastTskCheck;
static std::chrono::steady_clock::time_point lastDbCheck;
static std::chrono::steady_clock::time_point lastStatsRefresh;
static std::chrono::steady_clock::time_point lastOverlayRefresh;

static std::uintmax_t lastDbFileSize = 0;
static FILETIME lastDbWriteTime = {};

static bool statusNeedsRefresh = true;


/*
 * ------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------
 */

static SokuLib::SWRFont normalFont;
static SokuLib::SWRFont warningFont;

static SokuLib::DrawUtils::Sprite warningSprite;
static SokuLib::DrawUtils::Sprite infoSprite;
static SokuLib::DrawUtils::Sprite inputSprite;

static bool warningVisible = false;
static bool infoVisible = false;
static bool inputVisible = false;

static std::string lastWarningText;
static std::string lastInfoText;
static std::string lastInputText;


/*
 * ------------------------------------------------------------
 * UTF-8 -> Shift-JIS
 *
 * Game text resources traditionally use Japanese Windows code
 * page. Source files are compiled as UTF-8, then converted here.
 * ------------------------------------------------------------
 */

static std::string Utf8ToCp932(
    const std::string& utf8
)
{
    if (utf8.empty())
        return {};

    int wideSize =
        MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8.data(),
            static_cast<int>(utf8.size()),
            nullptr,
            0
        );

    if (wideSize <= 0)
        return {};

    std::wstring wide(
        wideSize,
        L'\0'
    );

    MultiByteToWideChar(
        CP_UTF8,
        0,
        utf8.data(),
        static_cast<int>(utf8.size()),
        &wide[0],
        wideSize
    );

    int cp932Size =
        WideCharToMultiByte(
            932,
            0,
            wide.c_str(),
            static_cast<int>(wide.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );

    if (cp932Size <= 0)
        return {};

    std::string result(
        cp932Size,
        '\0'
    );

    WideCharToMultiByte(
        932,
        0,
        wide.c_str(),
        static_cast<int>(wide.size()),
        &result[0],
        cp932Size,
        nullptr,
        nullptr
    );

    return result;
}


/*
 * ------------------------------------------------------------
 * Basic helpers
 * ------------------------------------------------------------
 */

static uint32_t ReadU32(
    uintptr_t address
)
{
    return *reinterpret_cast<
        volatile uint32_t*
    >(address);
}

static int ReadSceneId()
{
    return static_cast<int>(
        ReadU32(GameMemory::SCENEID)
    );
}

static bool FileExistsW(
    const std::wstring& path
)
{
    DWORD attr =
        GetFileAttributesW(
            path.c_str()
        );

    return
        attr != INVALID_FILE_ATTRIBUTES &&
        !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring GetGameDirectory()
{
    wchar_t buffer[MAX_PATH] = {};

    DWORD length =
        GetModuleFileNameW(
            nullptr,
            buffer,
            MAX_PATH
        );

    if (length == 0)
        return {};

    std::wstring fullPath(
        buffer,
        length
    );

    size_t slash =
        fullPath.find_last_of(
            L"\\/"
        );

    if (slash == std::wstring::npos)
        return {};

    return fullPath.substr(
        0,
        slash
    );
}


/*
 * ------------------------------------------------------------
 * Default.db
 *
 * User's actual directory layout:
 *
 * th123/
 * ├─ th123.exe
 * ├─ th4_5888/
 * │  ├─ tsk.exe
 * │  └─ Default.db
 * └─ Modules/
 *    └─ OIOverlay/
 *       └─ OIOverlay.dll
 * ------------------------------------------------------------
 */

static std::wstring FindDefaultDb()
{
    std::wstring gameDir =
        GetGameDirectory();

    if (gameDir.empty())
        return {};

    std::wstring expected =
        gameDir +
        L"\\th4_5888\\Default.db";

    if (FileExistsW(expected))
        return expected;

    /*
     * Fallback only.
     */
    expected =
        gameDir +
        L"\\Default.db";

    if (FileExistsW(expected))
        return expected;

    return {};
}


/*
 * ------------------------------------------------------------
 * File timestamp
 * ------------------------------------------------------------
 */

static bool GetFileInfo(
    const std::wstring& path,
    std::uintmax_t& size,
    FILETIME& writeTime
)
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};

    if (!GetFileAttributesExW(
            path.c_str(),
            GetFileExInfoStandard,
            &data
        )) {
        return false;
    }

    ULARGE_INTEGER fileSize = {};
    fileSize.LowPart =
        data.nFileSizeLow;
    fileSize.HighPart =
        data.nFileSizeHigh;

    size =
        static_cast<std::uintmax_t>(
            fileSize.QuadPart
        );

    writeTime =
        data.ftLastWriteTime;

    return true;
}

static bool SameFileTime(
    const FILETIME& a,
    const FILETIME& b
)
{
    return
        a.dwLowDateTime ==
            b.dwLowDateTime &&
        a.dwHighDateTime ==
            b.dwHighDateTime;
}


/*
 * ------------------------------------------------------------
 * tsk.exe
 * ------------------------------------------------------------
 */

static bool IsTskRunning()
{
    HANDLE snapshot =
        CreateToolhelp32Snapshot(
            TH32CS_SNAPPROCESS,
            0
        );

    if (snapshot ==
        INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize =
        sizeof(entry);

    bool found = false;

    if (Process32FirstW(
            snapshot,
            &entry
        )) {

        do {

            if (_wcsicmp(
                    entry.szExeFile,
                    L"tsk.exe"
                ) == 0) {

                found = true;
                break;
            }

        } while (Process32NextW(
            snapshot,
            &entry
        ));
    }

    CloseHandle(snapshot);

    return found;
}


/*
 * ------------------------------------------------------------
 * Scene helpers
 * ------------------------------------------------------------
 */

static bool IsMenuScene(
    int sceneId
)
{
    /*
     * OIViewer側で SceneID <= 7 のとき
     * clientMode をリセットしているため、
     * ここも同じ範囲をメニュー側として扱う。
     */
    return sceneId <= 7;
}

static bool IsNormalBattleScene(
    int sceneId
)
{
    return
        (sceneId >= 8 && sceneId <= 11) ||
        (sceneId >= 13 && sceneId <= 14);
}

static bool IsWatchingScene(
    int sceneId
)
{
    return
        sceneId == 12 ||
        sceneId == 15;
}


/*
 * ------------------------------------------------------------
 * Profile reader
 * ------------------------------------------------------------
 */

static std::string ReadProfile(
    uintptr_t offset
)
{
    uint32_t pnet =
        ReadU32(
            GameMemory::PNETOBJECT
        );

    if (pnet == 0)
        return {};

    char buffer[
        GameMemory::PROFILE_SIZE + 1
    ] = {};

    const char* address =
        reinterpret_cast<
            const char*
        >(
            static_cast<uintptr_t>(
                pnet
            ) + offset
        );

    std::memcpy(
        buffer,
        address,
        GameMemory::PROFILE_SIZE
    );

    buffer[
        GameMemory::PROFILE_SIZE
    ] = '\0';

    /*
     * profile name is stored as Shift-JIS /
     * game ANSI bytes. Preserve those bytes.
     */
    return std::string(buffer);
}


/*
 * ------------------------------------------------------------
 * DB state
 * ------------------------------------------------------------
 */

static void RefreshDatabaseObject()
{
    std::wstring newPath =
        FindDefaultDb();

    if (newPath.empty()) {
        tskDatabase.reset();
        databasePath.clear();
        currentStatsValid = false;
        return;
    }

    if (newPath == databasePath &&
        tskDatabase != nullptr) {
        return;
    }

    databasePath =
        newPath;

    tskDatabase =
        std::make_unique<
            TskDatabase
        >(databasePath);

    currentStatsValid = false;
}

static bool HasDatabaseChanged()
{
    if (databasePath.empty())
        return true;

    std::uintmax_t size = 0;
    FILETIME writeTime = {};

    if (!GetFileInfo(
            databasePath,
            size,
            writeTime
        )) {
        return true;
    }

    bool changed =
        size != lastDbFileSize ||
        !SameFileTime(
            writeTime,
            lastDbWriteTime
        );

    if (changed) {
        lastDbFileSize =
            size;

        lastDbWriteTime =
            writeTime;
    }

    return changed;
}


/*
 * ------------------------------------------------------------
 * Opponent detection
 * ------------------------------------------------------------
 */

static void ClearOpponent()
{
    currentOpponentProfileCp932.clear();
    currentStats = {};
    currentStatsValid = false;
}

static void UpdateOpponent(
    int sceneId
)
{
    if (IsMenuScene(sceneId)) {
        clientMode = false;
        return;
    }

    /*
     * Scene 9 = client.
     */
    if (sceneId == 9) {
        clientMode = true;
    }

    if (IsWatchingScene(sceneId)) {

        /*
         * 観戦では「相手」という一人に固定せず、
         * まずP2側の情報を取得する。
         * 実際の観戦時の両者表示は次段階で拡張できる。
         */
        std::string p2 =
            ReadProfile(
                GameMemory::RPROFOFS
            );

        if (!p2.empty() &&
            p2 !=
                currentOpponentProfileCp932) {

            currentOpponentProfileCp932 =
                p2;

            currentStatsValid = false;
        }

        return;
    }

    if (!IsNormalBattleScene(sceneId))
        return;

    std::string opponent;

    if (clientMode) {

        /*
         * Client:
         * 相手 = 1P
         */
        opponent =
            ReadProfile(
                GameMemory::LPROFOFS
            );

    }
    else {

        /*
         * Host:
         * 相手 = 2P
         */
        opponent =
            ReadProfile(
                GameMemory::RPROFOFS
            );
    }

    if (opponent.empty())
        return;

    if (opponent !=
        currentOpponentProfileCp932) {

        currentOpponentProfileCp932 =
            opponent;

        currentStats = {};
        currentStatsValid = false;

        lastStatsRefresh =
            std::chrono::steady_clock::time_point{};
    }
}


/*
 * ------------------------------------------------------------
 * Stats
 * ------------------------------------------------------------
 */

static void RefreshStatsIfNeeded()
{
    if (tskDatabase == nullptr ||
        currentOpponentProfileCp932.empty()) {
        return;
    }

    auto now =
        std::chrono::steady_clock::now();

    bool firstTime =
        lastStatsRefresh ==
        std::chrono::steady_clock::time_point{};

    bool databaseChanged =
        HasDatabaseChanged();

    bool intervalPassed =
        now - lastStatsRefresh >=
        std::chrono::seconds(1);

    if (!firstTime &&
        !databaseChanged &&
        !intervalPassed) {
        return;
    }

    currentStatsValid =
        tskDatabase->GetOpponentStats(
            currentOpponentProfileCp932,
            currentStats
        );

    lastStatsRefresh =
        now;
}


/*
 * ------------------------------------------------------------
 * Keyboard / clipboard
 * ------------------------------------------------------------
 */

static bool IsGameForeground()
{
    HWND foreground =
        GetForegroundWindow();

    if (!foreground)
        return false;

    DWORD processId = 0;

    GetWindowThreadProcessId(
        foreground,
        &processId
    );

    return
        processId ==
        GetCurrentProcessId();
}

static bool KeyPressed(int vk)
{
    return
        (GetAsyncKeyState(vk) & 1) != 0;
}

static char GetTypedCharacter(int vk)
{
    BYTE keyboardState[256] = {};

    if (!GetKeyboardState(
            keyboardState
        )) {
        return 0;
    }

    WCHAR chars[8] = {};

    UINT scan =
        MapVirtualKeyW(
            static_cast<UINT>(vk),
            MAPVK_VK_TO_VSC
        );

    int count =
        ToUnicode(
            static_cast<UINT>(vk),
            scan,
            keyboardState,
            chars,
            8,
            0
        );

    if (count != 1)
        return 0;

    wchar_t c = chars[0];

    if (c >= L'0' &&
        c <= L'9') {
        return static_cast<char>(c);
    }

    if (c == L'.')
        return '.';

    if (c == L':')
        return ':';

    return 0;
}

static void CopyIpPort()
{
    if (inputBuffer.empty())
        return;

    if (!OpenClipboard(nullptr))
        return;

    if (!EmptyClipboard()) {
        CloseClipboard();
        return;
    }

    const SIZE_T size =
        inputBuffer.size() + 1;

    HGLOBAL memory =
        GlobalAlloc(
            GMEM_MOVEABLE,
            size
        );

    if (memory == nullptr) {
        CloseClipboard();
        return;
    }

    void* data =
        GlobalLock(memory);

    if (data == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return;
    }

    std::memcpy(
        data,
        inputBuffer.c_str(),
        size
    );

    GlobalUnlock(memory);

    if (SetClipboardData(
            CF_TEXT,
            memory
        ) == nullptr) {

        GlobalFree(memory);
    }

    CloseClipboard();
}

static void PollMenuInput()
{
    if (!IsGameForeground())
        return;

    /*
     * Backspace
     */
    if (KeyPressed(VK_BACK)) {

        if (!inputBuffer.empty())
            inputBuffer.pop_back();
    }

    /*
     * Enter = copy
     */
    if (KeyPressed(VK_RETURN)) {
        CopyIpPort();
    }

    /*
     * 0-9
     */
    for (int vk = '0';
         vk <= '9';
         ++vk) {

        if (KeyPressed(vk)) {
            inputBuffer.push_back(
                static_cast<char>(vk)
            );
        }
    }

    /*
     * Numpad 0-9
     */
    for (int i = 0;
         i <= 9;
         ++i) {

        int vk =
            VK_NUMPAD0 + i;

        if (KeyPressed(vk)) {

            inputBuffer.push_back(
                static_cast<char>(
                    '0' + i
                )
            );
        }
    }

    /*
     * .
     */
    if (KeyPressed(
            VK_OEM_PERIOD
        )) {

        inputBuffer.push_back('.');
    }

    /*
     * : on Japanese keyboard:
     * Shift + ;
     */
    if (KeyPressed(VK_OEM_1)) {

        char c =
            GetTypedCharacter(
                VK_OEM_1
            );

        if (c == ':')
            inputBuffer.push_back(':');
    }
}


/*
 * ------------------------------------------------------------
 * Font
 * ------------------------------------------------------------
 */

static void CreateFonts()
{
    SokuLib::FontDescription normal = {};

    normal.r1 = 255;
    normal.g1 = 255;
    normal.b1 = 255;

    normal.r2 = 255;
    normal.g2 = 255;
    normal.b2 = 255;

    normal.height = 24;
    normal.weight = FW_BOLD;
    normal.italic = 0;
    normal.shadow = 4;

    normal.bufferSize = 1000000;

    normal.charSpaceX = 0;
    normal.charSpaceY = 0;

    normal.offsetX = 0;
    normal.offsetY = 0;
    normal.useOffset = 0;

    strcpy_s(
        normal.faceName,
        "MonoSpatialModSWR"
    );

    normalFont.create();
    normalFont.setIndirect(
        normal
    );


    SokuLib::FontDescription warning = normal;

    warning.r1 = 255;
    warning.g1 = 80;
    warning.b1 = 80;

    warning.r2 = 255;
    warning.g2 = 120;
    warning.b2 = 120;

    warning.height = 26;
    warning.weight = FW_BOLD;

    warningFont.create();
    warningFont.setIndirect(
        warning
    );
}


/*
 * ------------------------------------------------------------
 * Sprite text
 * ------------------------------------------------------------
 */

static void SetSpriteText(
    SokuLib::DrawUtils::Sprite& sprite,
    SokuLib::SWRFont& targetFont,
    const std::string& utf8Text,
    int x,
    int y
)
{
    if (utf8Text.empty())
        return;

    std::string text =
        Utf8ToCp932(
            utf8Text
        );

    if (text.empty())
        return;

    SokuLib::Vector2i realSize;

    sprite.texture.createFromText(
        text.c_str(),
        targetFont,
        { 820, 500 },
        &realSize
    );

    sprite.setPosition(
        SokuLib::Vector2i{
            x,
            y
        }
    );

    sprite.setSize(
        realSize.to<unsigned>()
    );

    sprite.rect.width =
        realSize.x;

    sprite.rect.height =
        realSize.y;
}


/*
 * ------------------------------------------------------------
 * Overlay formatting
 * ------------------------------------------------------------
 */

static std::string FormatPercent(
    double value
)
{
    std::ostringstream ss;

    ss << std::fixed
       << std::setprecision(1)
       << value
       << "%";

    return ss.str();
}

static std::string FormatPeriod(
    const char* label,
    const TskPeriodStats& stats
)
{
    std::ostringstream ss;

    ss << label
       << "："
       << std::setw(2)
       << stats.wins
       << "勝 "
       << std::setw(2)
       << stats.losses
       << "敗 "
       << "("
       << FormatPercent(
            stats.WinRate()
       )
       << ")";

    return ss.str();
}

static std::string GetMainCharacterName(
    int id
)
{
    switch (id) {
    case 0:  return "霊夢";
    case 1:  return "魔理沙";
    case 2:  return "咲夜";
    case 3:  return "アリス";
    case 4:  return "パチュリー";
    case 5:  return "妖夢";
    case 6:  return "レミリア";
    case 7:  return "幽々子";
    case 8:  return "紫";
    case 9:  return "萃香";
    case 10: return "鈴仙";
    case 11: return "文";
    case 12: return "小町";
    case 13: return "衣玖";
    case 14: return "天子";
    case 15: return "早苗";
    case 16: return "チルノ";
    case 17: return "美鈴";
    case 18: return "空";
    case 19: return "諏訪子";
    default: return "不明";
    }
}

static std::string BuildInfoText()
{
    if (currentOpponentProfileCp932.empty())
        return {};

    /*
     * Profile itself is CP932, so convert it back to UTF-8
     * for string composition, then SetSpriteText converts it
     * again to CP932 for the game font.
     */
    std::string profileUtf8;

    int wideSize =
        MultiByteToWideChar(
            932,
            0,
            currentOpponentProfileCp932.data(),
            static_cast<int>(
                currentOpponentProfileCp932.size()
            ),
            nullptr,
            0
        );

    if (wideSize > 0) {

        std::wstring wide(
            wideSize,
            L'\0'
        );

        MultiByteToWideChar(
            932,
            0,
            currentOpponentProfileCp932.data(),
            static_cast<int>(
                currentOpponentProfileCp932.size()
            ),
            &wide[0],
            wideSize
        );

        int utf8Size =
            WideCharToMultiByte(
                CP_UTF8,
                0,
                wide.c_str(),
                static_cast<int>(
                    wide.size()
                ),
                nullptr,
                0,
                nullptr,
                nullptr
            );

        if (utf8Size > 0) {

            profileUtf8.resize(
                utf8Size
            );

            WideCharToMultiByte(
                CP_UTF8,
                0,
                wide.c_str(),
                static_cast<int>(
                    wide.size()
                ),
                &profileUtf8[0],
                utf8Size,
                nullptr,
                nullptr
            );
        }
    }

    if (profileUtf8.empty())
        profileUtf8 =
            currentOpponentProfileCp932;


    std::ostringstream ss;

    ss
        << "■ 対戦情報\n"
        << "相手："
        << profileUtf8
        << "\n";

    if (!currentStatsValid) {

        ss
            << "対戦記録：取得できません\n";

        return ss.str();
    }

    if (!currentStats.found) {

        ss
            << "対戦記録がありません\n";

        return ss.str();
    }

    ss
        << "メインキャラ："
        << GetMainCharacterName(
            currentStats.mainCharacterId
        )
        << "\n";

    ss
        << "初回対戦："
        << currentStats.firstMatchDate
        << "\n";

    if (!currentStats.lastMatchDateBeforeToday.empty()) {

        ss
            << "最終対戦："
            << currentStats.lastMatchDateBeforeToday
            << "\n";
    }

    ss
        << "総対戦数："
        << currentStats.totalMatches
        << "戦\n";

    ss
        << "戦績："
        << currentStats.totalWins
        << "勝 "
        << currentStats.totalLosses
        << "敗 "
        << "("
        << FormatPercent(
            currentStats.TotalWinRate()
        )
        << ")\n";

    if (!currentStats.lastWinDate.empty()) {

        ss
            << "最終勝利："
            << currentStats.lastWinDate
            << "\n";
    }

    if (!currentStats.lastLossDate.empty()) {

        ss
            << "最終敗北："
            << currentStats.lastLossDate
            << "\n";
    }

    ss
        << FormatPeriod(
            "過去 30戦",
            currentStats.last30
        )
        << "\n";

    ss
        << FormatPeriod(
            "過去100戦",
            currentStats.last100
        )
        << "\n";

    ss
        << FormatPeriod(
            "過去1か月",
            currentStats.lastMonth
        );


    /*
     * Existing OIViewer-compatible warning condition.
     *
     * This is intentionally shown as an extra line rather
     * than hiding the record itself.
     */
    if (currentStats.hasUnrecordedWinningRound) {

        ss
            << "\n"
            << "[注意] この相手はまだこちらにラウンド敗北なし";
    }

    return ss.str();
}


/*
 * ------------------------------------------------------------
 * Overlay refresh
 * ------------------------------------------------------------
 */

static void RefreshOverlayText(
    int sceneId
)
{
    /*
     * TSK warning
     */
    std::string warning;

    if (!tskRunning) {

        warning =
            "[警告] 天則観(tsk.exe)が起動していません";

    }
    else if (
        tskDatabase == nullptr ||
        !tskDatabase->IsAvailable()
    ) {

        warning =
            "[警告] Default.dbが見つかりません";

    }

    if (warning != lastWarningText) {

        lastWarningText =
            warning;

        if (!warning.empty()) {

            SetSpriteText(
                warningSprite,
                warningFont,
                warning,
                OverlayPosition::WARNING_X,
                OverlayPosition::WARNING_Y
            );

            warningVisible =
                true;

        }
        else {
            warningVisible =
                false;
        }
    }


    /*
     * Info
     */
    std::string info;

    if (
        IsNormalBattleScene(sceneId) ||
        IsWatchingScene(sceneId)
    ) {

        info =
            BuildInfoText();
    }

    if (info != lastInfoText) {

        lastInfoText =
            info;

        if (!info.empty()) {

            SetSpriteText(
                infoSprite,
                normalFont,
                info,
                OverlayPosition::INFO_X,
                OverlayPosition::INFO_Y
            );

            infoVisible =
                true;

        }
        else {
            infoVisible =
                false;
        }
    }


    /*
     * IP input
     */
    std::string input;

    if (IsMenuScene(sceneId)) {

        input =
            "IP:Port ＞ " +
            inputBuffer +
            "\n"
            "数字・.・: を入力 / Enter でコピー";
    }

    if (input != lastInputText) {

        lastInputText =
            input;

        if (!input.empty()) {

            SetSpriteText(
                inputSprite,
                normalFont,
                input,
                OverlayPosition::INPUT_X,
                OverlayPosition::INPUT_Y
            );

            inputVisible =
                true;

        }
        else {
            inputVisible =
                false;
        }
    }
}


/*
 * ------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------
 */

static void InitializeRuntime()
{
    if (initialized)
        return;

    CreateFonts();

    RefreshDatabaseObject();

    tskRunning =
        IsTskRunning();

    initialized =
        true;

    lastTskCheck =
        std::chrono::steady_clock::now();

    lastDbCheck =
        std::chrono::steady_clock::now();

    lastStatsRefresh =
        std::chrono::steady_clock::time_point{};

    lastOverlayRefresh =
        std::chrono::steady_clock::time_point{};

    statusNeedsRefresh =
        true;
}


/*
 * ------------------------------------------------------------
 * BattleManager::onProcess
 * ------------------------------------------------------------
 */

static int __fastcall CBattleManager_OnProcess(
    SokuLib::BattleManager* This
)
{
    if (!initialized) {
        InitializeRuntime();
    }

    auto now =
        std::chrono::steady_clock::now();


    /*
     * F10 overlay toggle.
     */
    bool f10State =
        (GetAsyncKeyState(VK_F10) &
         0x8000) != 0;

    if (
        f10State &&
        !previousF10State
    ) {

        overlayEnabled =
            !overlayEnabled;
    }

    previousF10State =
        f10State;


    int sceneId =
        ReadSceneId();


    /*
     * Scene transitions
     */
    if (sceneId != previousSceneId) {

        /*
         * Menu -> battle
         */
        if (
            IsMenuScene(previousSceneId) &&
            (
                IsNormalBattleScene(sceneId) ||
                IsWatchingScene(sceneId)
            )
        ) {
            inputBuffer.clear();
        }

        /*
         * Battle -> menu:
         * keep input available for next entry,
         * just like a small editable overlay.
         */

        previousSceneId =
            sceneId;

        statusNeedsRefresh =
            true;
    }


    /*
     * Menu keyboard input
     */
    if (IsMenuScene(sceneId)) {
        PollMenuInput();
    }


    /*
     * tsk status
     */
    if (
        now - lastTskCheck >=
        std::chrono::milliseconds(500)
    ) {

        bool newTskState =
            IsTskRunning();

        if (newTskState !=
            tskRunning) {

            tskRunning =
                newTskState;

            statusNeedsRefresh =
                true;
        }

        lastTskCheck =
            now;
    }


    /*
     * DB location / live file updates
     */
    if (
        now - lastDbCheck >=
        std::chrono::milliseconds(500)
    ) {

        RefreshDatabaseObject();

        if (
            tskDatabase != nullptr &&
            HasDatabaseChanged()
        ) {
            statusNeedsRefresh =
                true;
        }

        lastDbCheck =
            now;
    }


    /*
     * Opponent memory reader
     */
    UpdateOpponent(sceneId);


    /*
     * Refresh DB stats.
     *
     * This happens on opponent change or when Default.db
     * changes, so the game does not have to restart after
     * tsk.exe writes a new record.
     */
    RefreshStatsIfNeeded();


    /*
     * Overlay text itself only gets regenerated periodically.
     */
    if (
        statusNeedsRefresh ||
        now - lastOverlayRefresh >=
            std::chrono::milliseconds(150)
    ) {

        RefreshOverlayText(
            sceneId
        );

        lastOverlayRefresh =
            now;

        statusNeedsRefresh =
            false;
    }


    return
        (This->*ogBattleMgrOnProcess)();
}


/*
 * ------------------------------------------------------------
 * BattleManager::onRender
 * ------------------------------------------------------------
 */

static int __fastcall CBattleManager_OnRender(
    SokuLib::BattleManager* This
)
{
    /*
     * Original render first.
     */
    (This->*ogBattleMgrOnRender)();


    /*
     * TSK warning intentionally survives F10 OFF.
     */
    if (warningVisible) {
        warningSprite.draw();
    }


    if (!overlayEnabled)
        return 0;


    if (infoVisible) {
        infoSprite.draw();
    }

    if (inputVisible &&
        IsMenuScene(
            ReadSceneId()
        )) {

        inputSprite.draw();
    }

    return 0;
}


/*
 * ------------------------------------------------------------
 * SWRSToys entry points
 * ------------------------------------------------------------
 */

extern "C"
__declspec(dllexport)
bool CheckVersion(
    const BYTE hash[16]
)
{
    return
        std::memcmp(
            hash,
            SokuLib::targetHash,
            sizeof(
                SokuLib::targetHash
            )
        ) == 0;
}


extern "C"
__declspec(dllexport)
bool Initialize(
    HMODULE hMyModule,
    HMODULE hParentModule
)
{
    DWORD oldProtect = 0;

#ifdef _DEBUG
    FILE* consoleFile = nullptr;

    AllocConsole();

    freopen_s(
        &consoleFile,
        "CONOUT$",
        "w",
        stdout
    );

    freopen_s(
        &consoleFile,
        "CONOUT$",
        "w",
        stderr
    );

    puts(
        "OIOverlay: Initialize"
    );
#endif


    /*
     * VTable is placed in a read-only section.
     * Keep the same hook installation method as the
     * currently working OIOverlay test build.
     */
    VirtualProtect(
        (PVOID)RDATA_SECTION_OFFSET,
        RDATA_SECTION_SIZE,
        PAGE_EXECUTE_WRITECOPY,
        &oldProtect
    );


    ogBattleMgrOnRender =
        SokuLib::TamperDword(
            &SokuLib::VTable_BattleManager.onRender,
            CBattleManager_OnRender
        );

    ogBattleMgrOnProcess =
        SokuLib::TamperDword(
            &SokuLib::VTable_BattleManager.onProcess,
            CBattleManager_OnProcess
        );


    VirtualProtect(
        (PVOID)RDATA_SECTION_OFFSET,
        RDATA_SECTION_SIZE,
        oldProtect,
        &oldProtect
    );


    FlushInstructionCache(
        GetCurrentProcess(),
        nullptr,
        0
    );


#ifdef _DEBUG
    puts(
        "OIOverlay: Hooks installed"
    );
#endif

    return true;
}


/*
 * ------------------------------------------------------------
 * DLL entry point
 * ------------------------------------------------------------
 */

extern "C"
int APIENTRY DllMain(
    HMODULE hModule,
    DWORD fdwReason,
    LPVOID lpReserved
)
{
    return TRUE;
}


/*
 * ------------------------------------------------------------
 * Mod priority
 * ------------------------------------------------------------
 */

extern "C"
__declspec(dllexport)
int getPriority()
{
    return 0;
}
