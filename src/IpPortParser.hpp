#pragma once

#include <string>

// Finds the first complete, valid IPv4:port token in arbitrary clipboard text.
// IPv4 octets must be 0..255 and the decimal port must be 0..99999.
bool ExtractFirstValidIpPort(const std::wstring& source, std::string& result);
