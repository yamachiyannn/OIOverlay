#include "TskDatabase.hpp"

#include <Windows.h>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
        return {};

    int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (size <= 0)
        return {};

    std::string result(size, '\0');

    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        &result[0],
        size,
        nullptr,
        nullptr
    );

    return result;
}

std::string BlobToString(sqlite3_stmt* stmt, int column)
{
    const unsigned char* blob =
        static_cast<const unsigned char*>(
            sqlite3_column_blob(stmt, column)
        );

    int size =
        sqlite3_column_bytes(stmt, column);

    if (blob == nullptr || size <= 0)
        return {};

    return std::string(
        reinterpret_cast<const char*>(blob),
        static_cast<size_t>(size)
    );
}

bool EqualCp932Blob(
    sqlite3_stmt* stmt,
    int column,
    const std::string& target
)
{
    std::string actual =
        BlobToString(stmt, column);

    return actual == target;
}

bool IsLeap(int year)
{
    return
        (year % 400 == 0) ||
        (year % 4 == 0 && year % 100 != 0);
}

bool BuildEpoch(
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second,
    long long& epoch
)
{
    if (year < 1970 ||
        year > 2200 ||
        month < 1 ||
        month > 12 ||
        day < 1 ||
        day > 31 ||
        hour < 0 ||
        hour > 23 ||
        minute < 0 ||
        minute > 59 ||
        second < 0 ||
        second > 59) {
        return false;
    }

    int days = 0;

    for (int y = 1970; y < year; ++y) {
        days += IsLeap(y) ? 366 : 365;
    }

    static const int daysPerMonth[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    for (int m = 1; m < month; ++m) {
        days += daysPerMonth[m - 1];

        if (m == 2 && IsLeap(year))
            days++;
    }

    days += day - 1;

    epoch =
        static_cast<long long>(days) * 86400LL +
        static_cast<long long>(hour) * 3600LL +
        static_cast<long long>(minute) * 60LL +
        second;

    return true;
}

bool ParseTimestampText(
    const std::string& text,
    long long& key,
    long long& epoch
)
{
    int y = 0;
    int mo = 0;
    int d = 0;
    int h = 0;
    int mi = 0;
    int s = 0;

    /*
     * 通常の SQLite timestamp:
     * YYYY-MM-DD HH:MM:SS
     */
    if (sscanf_s(
            text.c_str(),
            "%d-%d-%d %d:%d:%d",
            &y, &mo, &d, &h, &mi, &s
        ) == 6) {

        if (!BuildEpoch(
                y, mo, d, h, mi, s, epoch
            )) {
            return false;
        }

        key =
            static_cast<long long>(y) * 10000000000LL +
            static_cast<long long>(mo) * 100000000LL +
            static_cast<long long>(d) * 1000000LL +
            static_cast<long long>(h) * 10000LL +
            static_cast<long long>(mi) * 100LL +
            s;

        return true;
    }

    /*
     * YYYY/MM/DD HH:MM:SS
     */
    if (sscanf_s(
            text.c_str(),
            "%d/%d/%d %d:%d:%d",
            &y, &mo, &d, &h, &mi, &s
        ) == 6) {

        if (!BuildEpoch(
                y, mo, d, h, mi, s, epoch
            )) {
            return false;
        }

        key =
            static_cast<long long>(y) * 10000000000LL +
            static_cast<long long>(mo) * 100000000LL +
            static_cast<long long>(d) * 1000000LL +
            static_cast<long long>(h) * 10000LL +
            static_cast<long long>(mi) * 100LL +
            s;

        return true;
    }

    return false;
}

long long NowEpochSeconds()
{
    return static_cast<long long>(std::time(nullptr));
}

bool IsBeforeToday(
    long long epochSeconds
)
{
    if (epochSeconds <= 0)
        return false;

    std::time_t nowTime =
        std::time(nullptr);

    std::tm localNow = {};
    if (localtime_s(&localNow, &nowTime) != 0)
        return false;

    /*
     * Today 00:00 in the machine's local time zone.
     */
    std::tm today = localNow;
    today.tm_hour = 0;
    today.tm_min = 0;
    today.tm_sec = 0;

    const std::time_t todayEpoch =
        std::mktime(&today);

    return epochSeconds <
        static_cast<long long>(todayEpoch);
}

bool IsWithinLastMonth(
    long long epochSeconds
)
{
    if (epochSeconds <= 0)
        return false;

    long long now =
        NowEpochSeconds();

    /*
     * 30日相当の期間として扱う。
     * 表示用の「過去1か月」であり、
     * 元UIの用途と同じく直近期間を示す。
     */
    constexpr long long monthSeconds =
        30LL * 24LL * 60LL * 60LL;

    return
        epochSeconds >= now - monthSeconds &&
        epochSeconds <= now;
}

TskMatchRecord ReadRecord(
    sqlite3_stmt* stmt
)
{
    TskMatchRecord record;

    const unsigned char* timestamp =
        sqlite3_column_text(stmt, 0);

    if (timestamp != nullptr) {
        record.timestampText =
            reinterpret_cast<const char*>(
                timestamp
            );
    }
    else if (sqlite3_column_type(stmt, 0) ==
             SQLITE_INTEGER) {

        long long value =
            sqlite3_column_int64(stmt, 0);

        record.timestampText =
            std::to_string(value);
    }

    if (!record.timestampText.empty()) {

        if (ParseTimestampText(
                record.timestampText,
                record.timestampKey,
                record.epochSeconds
            )) {

            record.hasParsedTimestamp = true;

        }
        else if (
            sqlite3_column_type(stmt, 0) ==
            SQLITE_INTEGER
        ) {

            long long value =
                sqlite3_column_int64(stmt, 0);

            /*
             * Unix seconds
             */
            if (value >= 1000000000LL &&
                value <= 9999999999LL) {

                record.epochSeconds = value;
                record.timestampKey = value;
                record.hasParsedTimestamp = true;

            }
            /*
             * Unix milliseconds
             */
            else if (value >= 1000000000000LL &&
                     value <= 9999999999999LL) {

                record.epochSeconds =
                    value / 1000LL;

                record.timestampKey =
                    value;

                record.hasParsedTimestamp = true;
            }
            /*
             * YYYYMMDDHHMMSS
             */
            else if (value >= 19700101000000LL &&
                     value <= 22000101000000LL) {

                int second =
                    static_cast<int>(value % 100);
                value /= 100;

                int minute =
                    static_cast<int>(value % 100);
                value /= 100;

                int hour =
                    static_cast<int>(value % 100);
                value /= 100;

                int day =
                    static_cast<int>(value % 100);
                value /= 100;

                int month =
                    static_cast<int>(value % 100);
                value /= 100;

                int year =
                    static_cast<int>(value);

                long long epoch = 0;

                if (BuildEpoch(
                        year,
                        month,
                        day,
                        hour,
                        minute,
                        second,
                        epoch
                    )) {

                    record.epochSeconds =
                        epoch;

                    record.timestampKey =
                        static_cast<long long>(year) *
                            10000000000LL +
                        static_cast<long long>(month) *
                            100000000LL +
                        static_cast<long long>(day) *
                            1000000LL +
                        static_cast<long long>(hour) *
                            10000LL +
                        static_cast<long long>(minute) *
                            100LL +
                        second;

                    record.hasParsedTimestamp = true;
                }
            }
        }
    }

    record.p1NameCp932 =
        BlobToString(stmt, 1);

    record.p1CharacterId =
        sqlite3_column_int(stmt, 2);

    record.p1Rounds =
        sqlite3_column_int(stmt, 3);

    record.p2NameCp932 =
        BlobToString(stmt, 4);

    record.p2CharacterId =
        sqlite3_column_int(stmt, 5);

    record.p2Rounds =
        sqlite3_column_int(stmt, 6);

    return record;
}

void AddPeriodResult(
    const TskMatchRecord& record,
    TskPeriodStats& stats
)
{
    stats.matches++;

    if (record.p2Rounds >= 2) {
        stats.wins++;
    }

    if (record.p1Rounds >= 2) {
        stats.losses++;
    }
}

}


double TskPeriodStats::WinRate() const
{
    if (matches <= 0)
        return 0.0;

    return
        static_cast<double>(wins) *
        100.0 /
        static_cast<double>(matches);
}

double TskStats::TotalWinRate() const
{
    if (totalMatches <= 0)
        return 0.0;

    return
        static_cast<double>(totalWins) *
        100.0 /
        static_cast<double>(totalMatches);
}


TskDatabase::TskDatabase(
    const std::wstring& path
)
    : dbPath(path)
{
}

bool TskDatabase::IsAvailable() const
{
    DWORD attr =
        GetFileAttributesW(
            dbPath.c_str()
        );

    return
        attr != INVALID_FILE_ATTRIBUTES &&
        !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

const std::wstring& TskDatabase::GetPath() const
{
    return dbPath;
}

bool TskDatabase::GetOpponentStats(
    const std::string& profileCp932,
    TskStats& stats
) const
{
    stats = {};
    stats.profileNameCp932 =
        profileCp932;

    if (profileCp932.empty())
        return false;

    if (!IsAvailable())
        return false;

    std::string dbPathUtf8 =
        WideToUtf8(dbPath);

    if (dbPathUtf8.empty())
        return false;

    sqlite3* db = nullptr;

    int rc =
        sqlite3_open_v2(
            dbPathUtf8.c_str(),
            &db,
            SQLITE_OPEN_READONLY |
            SQLITE_OPEN_FULLMUTEX,
            nullptr
        );

    if (rc != SQLITE_OK || db == nullptr) {
        if (db)
            sqlite3_close(db);

        return false;
    }

    sqlite3_busy_timeout(
        db,
        200
    );

    const char* sql =
        "SELECT "
        "timestamp, "
        "CAST(p1name AS BLOB), "
        "p1id, "
        "p1win, "
        "CAST(p2name AS BLOB), "
        "p2id, "
        "p2win "
        "FROM trackrecord123 "
        "ORDER BY timestamp ASC";

    sqlite3_stmt* stmt = nullptr;

    rc =
        sqlite3_prepare_v2(
            db,
            sql,
            -1,
            &stmt,
            nullptr
        );

    if (rc != SQLITE_OK || stmt == nullptr) {
        sqlite3_close(db);
        return false;
    }

    std::vector<TskMatchRecord> records;

    while (
        sqlite3_step(stmt) ==
        SQLITE_ROW
    ) {

        /*
         * OIViewerと同じく、
         * P2プロファイルとして検索する。
         */
        if (!EqualCp932Blob(
                stmt,
                4,
                profileCp932
            )) {
            continue;
        }

        records.push_back(
            ReadRecord(stmt)
        );
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (records.empty()) {
        stats.found = false;
        return true;
    }

    stats.found = true;

    std::sort(
        records.begin(),
        records.end(),
        [](const TskMatchRecord& a,
           const TskMatchRecord& b) {

            if (a.hasParsedTimestamp &&
                b.hasParsedTimestamp) {

                return
                    a.timestampKey <
                    b.timestampKey;
            }

            return
                a.timestampText <
                b.timestampText;
        }
    );

    stats.totalMatches =
        static_cast<int>(records.size());

    for (const auto& record : records) {

        if (record.p2Rounds >= 2)
            stats.totalWins++;

        if (record.p1Rounds >= 2)
            stats.totalLosses++;
    }

    /*
     * OIViewer:
     * 100戦未満 -> 全戦
     * 100戦以上 -> 直近100戦
     */
    std::vector<TskMatchRecord> recent100 =
        records;

    if (recent100.size() > 100) {

        recent100.erase(
            recent100.begin(),
            recent100.end() - 100
        );
    }

    for (const auto& record :
         recent100) {

        AddPeriodResult(
            record,
            stats.last100
        );
    }

    /*
     * 直近30戦
     */
    std::vector<TskMatchRecord> recent30 =
        records;

    if (recent30.size() > 30) {

        recent30.erase(
            recent30.begin(),
            recent30.end() - 30
        );
    }

    for (const auto& record :
         recent30) {

        AddPeriodResult(
            record,
            stats.last30
        );
    }

    /*
     * 直近1か月
     */
    for (const auto& record :
         records) {

        if (record.hasParsedTimestamp &&
            IsWithinLastMonth(
                record.epochSeconds
            )) {

            AddPeriodResult(
                record,
                stats.lastMonth
            );
        }
    }

    /*
     * 初回 / 最終 / 最終勝敗
     */
    stats.firstMatchDate =
        records.front().timestampText;

    for (auto it = records.rbegin();
         it != records.rend();
         ++it) {

        if (it->hasParsedTimestamp) {

            if (IsBeforeToday(
                    it->epochSeconds
                )) {

                stats.lastMatchDateBeforeToday =
                    it->timestampText;

                break;
            }

        }
        else {
            /*
             * timestampが解析できない場合は
             * 最後の記録を表示する。
             */
            stats.lastMatchDateBeforeToday =
                it->timestampText;

            break;
        }
    }

    for (auto it = records.rbegin();
         it != records.rend();
         ++it) {

        if (it->p2Rounds >= 2) {

            stats.lastWinDate =
                it->timestampText;

            break;
        }
    }

    for (auto it = records.rbegin();
         it != records.rend();
         ++it) {

        if (it->p1Rounds >= 2) {

            stats.lastLossDate =
                it->timestampText;

            break;
        }
    }

    /*
     * 「100戦以上なら直近100戦、
     * 100未満なら全戦」でメインキャラ集計。
     */
    std::map<int, int> characterCounts;

    for (const auto& record :
         recent100) {

        if (record.p2CharacterId < 0)
            continue;

        characterCounts[
            record.p2CharacterId
        ]++;
    }

    for (const auto& pair :
         characterCounts) {

        /*
         * 同数ならIDが小さい側。
         */
        if (
            pair.second >
            stats.mainCharacterMatches
        ) {

            stats.mainCharacterId =
                pair.first;

            stats.mainCharacterMatches =
                pair.second;
        }
    }

    /*
     * OIViewerの既存互換判定。
     */
    stats.hasUnrecordedWinningRound =
        !records.empty();

    for (const auto& record :
         records) {

        if (!(record.p1Rounds == 0 &&
              record.p2Rounds == 2)) {

            stats.hasUnrecordedWinningRound =
                false;

            break;
        }
    }

    return true;
}


const char* GetCharacterNameUtf8(int id)
{
    switch (id) {
    case 0:  return "Reimu";
    case 1:  return "Marisa";
    case 2:  return "Sakuya";
    case 3:  return "Alice";
    case 4:  return "Patchouli";
    case 5:  return "Youmu";
    case 6:  return "Remilia";
    case 7:  return "Yuyuko";
    case 8:  return "Yukari";
    case 9:  return "Suika";
    case 10: return "Reisen";
    case 11: return "Aya";
    case 12: return "Komachi";
    case 13: return "Iku";
    case 14: return "Tenshi";
    case 15: return "Sanae";
    case 16: return "Cirno";
    case 17: return "Meiling";
    case 18: return "Utsuho";
    case 19: return "Suwako";
    default: return "Unknown";
    }
}


const char* GetCharacterNameCp932(int id)
{
    return GetCharacterNameUtf8(id);
}
