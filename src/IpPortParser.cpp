#include "IpPortParser.hpp"

#include <cstddef>
#include <string>

namespace
{
bool IsDigit(wchar_t value)
{
    return value >= L'0' && value <= L'9';
}

bool ParseNumber(
    const std::wstring& source,
    size_t& position,
    size_t maxDigits,
    unsigned long maxValue,
    unsigned long& value)
{
    const size_t start = position;
    value = 0;
    while (position < source.size() && IsDigit(source[position]) && position - start < maxDigits) {
        value = value * 10UL + static_cast<unsigned long>(source[position] - L'0');
        ++position;
    }
    if (position == start || (position < source.size() && IsDigit(source[position])) || value > maxValue)
        return false;
    return true;
}

bool TryParseAt(const std::wstring& source, size_t start, size_t& end)
{
    if (start >= source.size() || !IsDigit(source[start]))
        return false;
    if (start > 0 && IsDigit(source[start - 1]))
        return false;

    size_t position = start;
    unsigned long value = 0;
    for (int octet = 0; octet < 4; ++octet) {
        if (!ParseNumber(source, position, 3, 255, value))
            return false;
        if (octet != 3) {
            if (position >= source.size() || source[position] != L'.')
                return false;
            ++position;
        }
    }
    if (position >= source.size() || source[position] != L':')
        return false;
    ++position;
    if (!ParseNumber(source, position, 5, 99999, value))
        return false;

    // Do not accept a partial match embedded in a longer digit sequence,
    // e.g. 999.1.1.1:80 or 1.2.3.4:123456. Dots/colons and other text around
    // a complete token are allowed because clipboard text is deliberately trimmed.
    if (position < source.size() && IsDigit(source[position]))
        return false;

    end = position;
    return true;
}
}

bool ExtractFirstValidIpPort(const std::wstring& source, std::string& result)
{
    result.clear();
    for (size_t start = 0; start < source.size(); ++start) {
        if (!IsDigit(source[start]))
            continue;
        size_t end = start;
        if (!TryParseAt(source, start, end))
            continue;
        result.reserve(end - start);
        for (size_t i = start; i < end; ++i)
            result.push_back(static_cast<char>(source[i]));
        return true;
    }
    return false;
}
