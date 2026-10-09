#pragma once

#include <string>
#include <vector>

struct TskMatchRecord {
    std::string timestampText;

    long long timestampKey = 0;
    bool hasParsedTimestamp = false;
    long long epochSeconds = 0;

    std::string p1NameCp932;
    std::string p2NameCp932;

    int p1CharacterId = -1;
    int p2CharacterId = -1;

    int p1Rounds = 0;
    int p2Rounds = 0;
};

struct TskPeriodStats {
    int matches = 0;
    int wins = 0;
    int losses = 0;

    double WinRate() const;
};

struct TskStats {
    bool found = false;

    std::string profileNameCp932;

    int totalMatches = 0;
    int totalWins = 0;
    int totalLosses = 0;

    TskPeriodStats last30;
    TskPeriodStats last100;
    TskPeriodStats lastMonth;

    int mainCharacterId = -1;
    int mainCharacterMatches = 0;

    std::string firstMatchDate;
    std::string lastMatchDateBeforeToday;
    std::string lastWinDate;
    std::string lastLossDate;

    /*
     * 既存 OIViewer の警告ロジックに合わせる。
     * 対象プロファイルの記録が全て
     *   p1 = 0ラウンド
     *   p2 = 2ラウンド
     * の場合 true。
     */
    bool hasUnrecordedWinningRound = false;

    double TotalWinRate() const;
};

class TskDatabase {
public:
    explicit TskDatabase(const std::wstring& path);

    bool IsAvailable() const;

    const std::wstring& GetPath() const;

    bool GetOpponentStats(
        const std::string& profileCp932,
        TskStats& stats
    ) const;

private:
    std::wstring dbPath;
};

const char* GetCharacterNameUtf8(int id);
const char* GetCharacterNameCp932(int id);
