#pragma once

#include <string>
#include <vector>

class CombinedPlayersDatabase
{
public:
    explicit CombinedPlayersDatabase(std::wstring path);

    // Finds the player containing profileCp932 and returns the player display name
    // plus all member profiles. Returns false when the DB is blank/unavailable,
    // invalid, or the profile is not registered. This class never writes to DB.
    bool ResolveProfile(
        const std::string& profileCp932,
        std::wstring& displayName,
        std::vector<std::string>& profilesCp932) const;

    // Compatibility helper for call sites that only need the display name.
    bool GetDisplayName(const std::string& profileCp932, std::wstring& displayName) const;

private:
    std::wstring path_;
};
