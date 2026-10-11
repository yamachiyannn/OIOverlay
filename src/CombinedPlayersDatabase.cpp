#include "CombinedPlayersDatabase.hpp"

#include <Windows.h>
#include <sqlite3.h>
#include <algorithm>
#include <cwctype>
#include <utility>

namespace
{
std::wstring Cp932ToWide(const std::string& value)
{
    if (value.empty())
        return {};
    const int count = MultiByteToWideChar(932, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0)
        return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(932, 0, value.data(), static_cast<int>(value.size()), &result[0], count) <= 0)
        return {};
    return result;
}

std::string WideToCp932(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int count = WideCharToMultiByte(932, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
        return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(932, 0, value.data(), static_cast<int>(value.size()), &result[0], count, nullptr, nullptr) <= 0)
        return {};
    return result;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
        return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &result[0], count, nullptr, nullptr) <= 0)
        return {};
    return result;
}

std::wstring ColumnWideText(sqlite3_stmt* statement, int column)
{
    const auto* characters = static_cast<const wchar_t*>(sqlite3_column_text16(statement, column));
    const int bytes = sqlite3_column_bytes16(statement, column);
    if (!characters || bytes <= 0 || bytes % static_cast<int>(sizeof(wchar_t)) != 0)
        return {};
    return std::wstring(characters, static_cast<size_t>(bytes) / sizeof(wchar_t));
}

bool IsBlank(const std::wstring& value)
{
    return std::all_of(value.begin(), value.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; });
}

class DatabaseCloser
{
public:
    explicit DatabaseCloser(sqlite3* database) : database_(database) {}
    ~DatabaseCloser() { if (database_) sqlite3_close(database_); }
    DatabaseCloser(const DatabaseCloser&) = delete;
    DatabaseCloser& operator=(const DatabaseCloser&) = delete;
private:
    sqlite3* database_;
};

class StatementCloser
{
public:
    explicit StatementCloser(sqlite3_stmt* statement) : statement_(statement) {}
    ~StatementCloser() { if (statement_) sqlite3_finalize(statement_); }
    StatementCloser(const StatementCloser&) = delete;
    StatementCloser& operator=(const StatementCloser&) = delete;
private:
    sqlite3_stmt* statement_;
};
}

CombinedPlayersDatabase::CombinedPlayersDatabase(std::wstring path)
    : path_(std::move(path))
{
}

bool CombinedPlayersDatabase::ResolveProfile(
    const std::string& profileCp932,
    std::wstring& displayName,
    std::vector<std::string>& profilesCp932) const
{
    displayName.clear();
    profilesCp932.clear();
    if (path_.empty() || profileCp932.empty())
        return false;

    const std::wstring profile = Cp932ToWide(profileCp932);
    const std::string utf8Path = WideToUtf8(path_);
    if (profile.empty() || utf8Path.empty())
        return false;

    sqlite3* database = nullptr;
    const int openResult = sqlite3_open_v2(
        utf8Path.c_str(), &database,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (openResult != SQLITE_OK) {
        if (database)
            sqlite3_close(database);
        return false;
    }
    DatabaseCloser closeDatabase(database);
    sqlite3_busy_timeout(database, 200);

    int playerId = -1;
    std::wstring playerName;
    std::wstring representativeProfile;
    {
        static constexpr char query[] =
            "SELECT p.PlayerId, COALESCE(p.PlayerName, ''), COALESCE(p.RepresentativeProfile, '') "
            "FROM Players AS p "
            "JOIN Profiles AS pr ON pr.PlayerId = p.PlayerId "
            "WHERE pr.ProfileName = ?1 COLLATE BINARY LIMIT 1;";

        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, query, -1, &statement, nullptr) != SQLITE_OK)
            return false;
        StatementCloser closeStatement(statement);
        if (sqlite3_bind_text16(statement, 1, profile.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
            return false;
        if (sqlite3_step(statement) != SQLITE_ROW)
            return false;

        playerId = sqlite3_column_int(statement, 0);
        playerName = ColumnWideText(statement, 1);
        representativeProfile = ColumnWideText(statement, 2);
    }

    if (playerId < 0)
        return false;

    // OIV puts the representative profile first, then keeps the remaining
    // profiles in their stored display order.
    if (!representativeProfile.empty()) {
        const std::string representative = WideToCp932(representativeProfile);
        if (!representative.empty())
            profilesCp932.push_back(representative);
    }

    {
        static constexpr char query[] =
            "SELECT ProfileName FROM Profiles "
            "WHERE PlayerId = ?1 "
            "ORDER BY DisplayOrder, ProfileName COLLATE BINARY;";
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, query, -1, &statement, nullptr) != SQLITE_OK) {
            profilesCp932.clear();
            return false;
        }
        StatementCloser closeStatement(statement);
        if (sqlite3_bind_int(statement, 1, playerId) != SQLITE_OK) {
            profilesCp932.clear();
            return false;
        }
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const std::string profileName = WideToCp932(ColumnWideText(statement, 0));
            if (!profileName.empty() &&
                std::find(profilesCp932.begin(), profilesCp932.end(), profileName) == profilesCp932.end()) {
                profilesCp932.push_back(profileName);
            }
        }
    }

    if (profilesCp932.empty())
        return false;

    displayName = !IsBlank(playerName) ? playerName : representativeProfile;
    if (displayName.empty())
        displayName = profile;
    return true;
}

bool CombinedPlayersDatabase::GetDisplayName(const std::string& profileCp932, std::wstring& displayName) const
{
    std::vector<std::string> profiles;
    return ResolveProfile(profileCp932, displayName, profiles);
}
