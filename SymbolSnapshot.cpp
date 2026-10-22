#include "SymbolSnapshot.h"
#include "SymbolValueMapDefaults.h"

#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4100)
#include <MT5APIManager.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

std::wstring apiError(const wchar_t* operation, MTAPIRES result) {
    return std::wstring(operation) + L" failed (MTAPIRES=" +
           std::to_wstring(static_cast<unsigned>(result)) + L").";
}

bool parseLogin(const std::wstring& text, std::uint64_t& login) {
    if (text.empty()) return false;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
    }

    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end != text.c_str() + text.size() || parsed == 0) return false;
    login = static_cast<std::uint64_t>(parsed);
    return true;
}

std::wstring cleanText(const wchar_t* value) {
    if (!value) return {};
    std::wstring result(value);
    for (wchar_t& ch : result) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    }
    return result;
}

template <typename T>
std::wstring numberText(T value) {
    if constexpr (std::is_floating_point_v<T>) {
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%.17g", static_cast<double>(value));
        return buffer;
    } else {
        return std::to_wstring(value);
    }
}

std::wstring precision8NumberText(double value) {
    if (!std::isfinite(value)) return numberText(value);

    wchar_t buffer[512]{};
    if (swprintf_s(buffer, L"%.8f", value) < 0) return numberText(value);
    std::wstring result(buffer);
    const std::size_t decimalPoint = result.find(L'.');
    if (decimalPoint != std::wstring::npos) {
        while (!result.empty() && result.back() == L'0') result.pop_back();
        if (!result.empty() && result.back() == L'.') result.pop_back();
    }
    if (result == L"-0") return L"0";
    return result;
}

std::wstring timestampText(std::int64_t timestamp) {
    if (timestamp == 0) return L"0";
    const __time64_t value = static_cast<__time64_t>(timestamp);
    std::tm utc{};
    if (_gmtime64_s(&utc, &value) != 0) return std::to_wstring(timestamp);

    wchar_t buffer[40]{};
    swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d:%02d UTC",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
        utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

std::wstring colorText(COLORREF color) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"#%02X%02X%02X (0x%08X)",
               GetRValue(color), GetGValue(color), GetBValue(color), color);
    return buffer;
}

std::wstring minuteTime(std::uint32_t minutes) {
    constexpr std::uint32_t minutesPerDay = 24 * 60;
    minutes %= minutesPerDay;
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%02u:%02u", minutes / 60, minutes % 60);
    return buffer;
}

struct Property {
    std::wstring name;
    std::function<std::wstring(const IMTConSymbol&, IMTConSymbolSession*)> read;
    std::function<bool(IMTConSymbol&, std::wstring_view, IMTConSymbolSession*,
                       bool&, std::wstring&)> write;
    bool editable = true;
    SymbolFieldValueKind valueKind = SymbolFieldValueKind::Text;
};

template <typename T>
constexpr SymbolFieldValueKind numericValueKind() {
    if constexpr (std::is_floating_point_v<T>) {
        return SymbolFieldValueKind::FloatingPoint;
    } else if constexpr (std::is_signed_v<T>) {
        return sizeof(T) <= sizeof(std::int32_t)
            ? SymbolFieldValueKind::SignedInteger32
            : SymbolFieldValueKind::SignedInteger64;
    } else {
        return sizeof(T) <= sizeof(std::uint32_t)
            ? SymbolFieldValueKind::UnsignedInteger32
            : SymbolFieldValueKind::UnsignedInteger64;
    }
}

std::wstring trim(std::wstring value);

template <typename T>
bool parseFieldNumber(std::wstring_view text, T& parsed) {
    const std::wstring value = trim(std::wstring(text));
    if (value.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    if constexpr (std::is_floating_point_v<T>) {
        const double result = std::wcstod(value.c_str(), &end);
        if (errno == ERANGE || end != value.c_str() + value.size() || !std::isfinite(result)) return false;
        parsed = static_cast<T>(result);
        return std::isfinite(parsed);
    } else if constexpr (std::is_unsigned_v<T>) {
        for (const wchar_t ch : value) if (ch < L'0' || ch > L'9') return false;
        const unsigned long long result = std::wcstoull(value.c_str(), &end, 10);
        if (errno == ERANGE || end != value.c_str() + value.size() ||
            result > static_cast<unsigned long long>((std::numeric_limits<T>::max)())) return false;
        parsed = static_cast<T>(result);
        return true;
    } else {
        std::size_t firstDigit = (value.front() == L'+' || value.front() == L'-') ? 1 : 0;
        if (firstDigit == value.size()) return false;
        for (std::size_t index = firstDigit; index < value.size(); ++index) {
            if (value[index] < L'0' || value[index] > L'9') return false;
        }
        const long long result = std::wcstoll(value.c_str(), &end, 10);
        if (errno == ERANGE || end != value.c_str() + value.size() ||
            result < static_cast<long long>(std::numeric_limits<T>::lowest()) ||
            result > static_cast<long long>((std::numeric_limits<T>::max)())) return false;
        parsed = static_cast<T>(result);
        return true;
    }
}

bool parseTimestamp(std::wstring_view text, std::int64_t& timestamp) {
    const std::wstring value = trim(std::wstring(text));
    if (parseFieldNumber<std::int64_t>(value, timestamp)) return true;

    if (value.size() != 23 || value[4] != L'-' || value[7] != L'-' ||
        value[10] != L' ' || value[13] != L':' || value[16] != L':' ||
        value.substr(19) != L" UTC") {
        return false;
    }

    const auto parseDigits = [&value](std::size_t start, std::size_t length, int& result) {
        result = 0;
        for (std::size_t index = start; index < start + length; ++index) {
            if (value[index] < L'0' || value[index] > L'9') return false;
            result = result * 10 + static_cast<int>(value[index] - L'0');
        }
        return true;
    };

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!parseDigits(0, 4, year) || !parseDigits(5, 2, month) ||
        !parseDigits(8, 2, day) || !parseDigits(11, 2, hour) ||
        !parseDigits(14, 2, minute) || !parseDigits(17, 2, second) ||
        year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    std::tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    const __time64_t converted = _mkgmtime64(&utc);
    if (converted == -1) return false;

    std::tm roundTrip{};
    if (_gmtime64_s(&roundTrip, &converted) != 0 ||
        roundTrip.tm_year != year - 1900 || roundTrip.tm_mon != month - 1 ||
        roundTrip.tm_mday != day || roundTrip.tm_hour != hour ||
        roundTrip.tm_min != minute || roundTrip.tm_sec != second) {
        return false;
    }
    timestamp = static_cast<std::int64_t>(converted);
    return true;
}

template <typename Getter, typename Setter>
bool writeTimestamp(std::wstring_view text, Getter getter, Setter setter,
                    bool& changed, std::wstring& error) {
    std::int64_t parsed = 0;
    if (!parseTimestamp(text, parsed)) {
        error = L"Invalid date/time value '" + std::wstring(text) +
            L"'. Use YYYY-MM-DD HH:mm:ss UTC or an integer Unix timestamp.";
        return false;
    }
    if (parsed == getter()) return true;
    const MTAPIRES result = setter(parsed);
    if (result != MT_RET_OK) {
        error = L"SDK rejected the date/time value (MTAPIRES=" +
            std::to_wstring(static_cast<unsigned>(result)) + L").";
        return false;
    }
    changed = true;
    return true;
}

template <typename Getter, typename Setter>
bool writeNumber(std::wstring_view text, Getter getter, Setter setter,
                 bool& changed, std::wstring& error) {
    using Value = std::remove_cv_t<std::invoke_result_t<Getter>>;
    Value parsed{};
    if (!parseFieldNumber(text, parsed)) {
        error = L"Invalid numeric value '" + std::wstring(text) + L"'.";
        return false;
    }
    if (parsed == getter()) return true;
    const MTAPIRES result = setter(parsed);
    if (result != MT_RET_OK) {
        error = L"SDK rejected the value (MTAPIRES=" +
            std::to_wstring(static_cast<unsigned>(result)) + L").";
        return false;
    }
    changed = true;
    return true;
}

template <typename Getter, typename Setter>
bool writePrecision8Number(std::wstring_view text, Getter getter, Setter setter,
                           bool& changed, std::wstring& error) {
    double parsed = 0;
    if (!parseFieldNumber(text, parsed)) {
        error = L"Invalid numeric value '" + std::wstring(text) + L"'.";
        return false;
    }
    if (precision8NumberText(parsed) ==
        precision8NumberText(static_cast<double>(getter()))) return true;

    const MTAPIRES result = setter(parsed);
    if (result != MT_RET_OK) {
        error = L"SDK rejected the swap value (MTAPIRES=" +
            std::to_wstring(static_cast<unsigned>(result)) + L").";
        return false;
    }
    changed = true;
    return true;
}

template <typename Getter>
bool validateReadOnlyNumber(std::wstring_view text, Getter getter,
                            std::wstring_view fieldName, std::wstring& error) {
    using Value = std::remove_cv_t<std::invoke_result_t<Getter>>;
    Value parsed{};
    if (!parseFieldNumber(text, parsed)) {
        error = L"Invalid numeric value '" + std::wstring(text) + L"'.";
        return false;
    }
    if (parsed != getter()) {
        error = L"Field '" + std::wstring(fieldName) + L"' is read-only.";
        return false;
    }
    return true;
}

template <typename Getter, typename Setter>
bool writeText(std::wstring_view text, Getter getter, Setter setter,
               bool& changed, std::wstring& error) {
    const std::wstring value(text);
    if (value == cleanText(getter())) return true;
    const MTAPIRES result = setter(value.c_str());
    if (result != MT_RET_OK) {
        error = L"SDK rejected the text value (MTAPIRES=" +
            std::to_wstring(static_cast<unsigned>(result)) + L").";
        return false;
    }
    changed = true;
    return true;
}

bool parseColor(std::wstring_view text, COLORREF& color) {
    const std::wstring value(text);
    const std::size_t marker = value.find(L"0x");
    if (marker != std::wstring::npos) {
        const wchar_t* start = value.c_str() + marker + 2;
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long parsed = std::wcstoul(start, &end, 16);
        if (errno == ERANGE || end == start || parsed > 0xFFFFFFFFul) return false;
        while (*end == L' ' || *end == L')') ++end;
        if (*end != L'\0') return false;
        color = static_cast<COLORREF>(parsed);
        return true;
    }
    if (value.size() == 7 && value.front() == L'#') {
        wchar_t* end = nullptr;
        const unsigned long rgb = std::wcstoul(value.c_str() + 1, &end, 16);
        if (end != value.c_str() + value.size()) return false;
        color = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        return true;
    }
    return false;
}

template <typename Getter, typename Setter>
bool writeColor(std::wstring_view text, Getter getter, Setter setter,
                bool& changed, std::wstring& error) {
    COLORREF parsed{};
    if (!parseColor(text, parsed)) {
        error = L"Invalid color value '" + std::wstring(text) + L"'.";
        return false;
    }
    if (parsed == getter()) return true;
    const MTAPIRES result = setter(parsed);
    if (result != MT_RET_OK) {
        error = L"SDK rejected the color value (MTAPIRES=" +
            std::to_wstring(static_cast<unsigned>(result)) + L").";
        return false;
    }
    changed = true;
    return true;
}

bool parseMinute(std::wstring_view text, std::uint32_t& minute) {
    if (text.size() != 5 || text[2] != L':') return false;
    if (text[0] < L'0' || text[0] > L'9' || text[1] < L'0' || text[1] > L'9' ||
        text[3] < L'0' || text[3] > L'9' || text[4] < L'0' || text[4] > L'9') return false;
    const std::uint32_t hour = static_cast<std::uint32_t>((text[0] - L'0') * 10 + text[1] - L'0');
    const std::uint32_t mins = static_cast<std::uint32_t>((text[3] - L'0') * 10 + text[4] - L'0');
    if (hour > 48 || mins > 59 || (hour == 48 && mins != 0)) return false;
    minute = hour * 60 + mins;
    return true;
}

using SessionRange = std::pair<std::uint32_t, std::uint32_t>;

bool parseSessions(std::wstring_view text, std::vector<SessionRange>& ranges) {
    ranges.clear();
    const std::wstring value = trim(std::wstring(text));
    if (value.empty()) return true;

    bool hasLegacyCount = false;
    std::uint32_t expectedCount = 0;
    std::size_t start = 0;
    const std::size_t legacyColon = value.find(L':');
    if (legacyColon != std::wstring::npos) {
        const std::wstring prefix = trim(value.substr(0, legacyColon));
        std::uint32_t parsedCount = 0;
        const std::size_t afterColon = legacyColon + 1;
        const bool legacySeparator = afterColon == value.size() ||
            value[afterColon] == L' ' || value[afterColon] == L'\t' || value[afterColon] == L';';
        if (legacySeparator && parseFieldNumber(prefix, parsedCount)) {
            hasLegacyCount = true;
            expectedCount = parsedCount;
            start = afterColon;
        }
    }

    constexpr std::uint32_t minutesPerDay = 24 * 60;
    while (start < value.size()) {
        while (start < value.size() && (value[start] == L' ' || value[start] == L';')) ++start;
        if (start == value.size()) break;
        const std::size_t separator = value.find(L'-', start);
        const std::size_t end = value.find(L';', start);
        if (separator == std::wstring::npos || (end != std::wstring::npos && separator > end)) return false;
        const std::size_t rangeEnd = end == std::wstring::npos ? value.size() : end;
        std::uint32_t open = 0, close = 0;
        if (!parseMinute(std::wstring_view(value).substr(start, separator - start), open) ||
            !parseMinute(std::wstring_view(value).substr(separator + 1, rangeEnd - separator - 1), close)) return false;

        open %= minutesPerDay;
        close %= minutesPerDay;
        if (close <= open) close += minutesPerDay;
        if (close - open > minutesPerDay) return false;
        ranges.emplace_back(open, close);
        start = rangeEnd == value.size() ? rangeEnd : rangeEnd + 1;
    }
    return !hasLegacyCount || ranges.size() == expectedCount;
}

bool normalizeSessionRange(SessionRange& range) {
    constexpr std::uint32_t minutesPerDay = 24 * 60;
    std::uint32_t open = range.first;
    std::uint32_t close = range.second;
    if (close <= open) close += minutesPerDay;
    const std::uint32_t duration = close - open;
    if (duration > minutesPerDay) return false;
    open %= minutesPerDay;
    range = {open, open + duration};
    return true;
}

bool normalizeSessionRanges(std::vector<SessionRange>& ranges) {
    for (SessionRange& range : ranges) {
        if (!normalizeSessionRange(range)) return false;
    }
    std::sort(ranges.begin(), ranges.end());
    return true;
}

template <typename Total, typename Next>
bool readSessions(std::uint32_t day, IMTConSymbolSession* session,
                  Total totalFn, Next nextFn, std::vector<SessionRange>& ranges) {
    ranges.clear();
    const std::uint32_t total = totalFn(day);
    ranges.reserve(total);
    for (std::uint32_t pos = 0; pos < total; ++pos) {
        if (nextFn(day, pos, session) != MT_RET_OK) return false;
        ranges.emplace_back(session->Open(), session->Close());
    }
    return true;
}

template <typename Total, typename Next, typename Clear, typename Add>
bool writeSessions(std::wstring_view text, std::uint32_t day, IMTConSymbolSession* session,
                   Total totalFn, Next nextFn, Clear clearFn, Add addFn,
                   bool& changed, std::wstring& error) {
    std::vector<SessionRange> requested;
    if (!parseSessions(text, requested)) {
        error = L"Invalid session list '" + std::wstring(text) + L"'.";
        return false;
    }
    std::vector<SessionRange> existing;
    if (!readSessions(day, session, totalFn, nextFn, existing)) {
        error = L"Cannot read the current symbol sessions before updating.";
        return false;
    }
    if (!normalizeSessionRanges(requested)) {
        error = L"The requested session list contains a session longer than 24 hours.";
        return false;
    }
    if (!normalizeSessionRanges(existing)) {
        error = L"The current symbol contains a session longer than 24 hours.";
        return false;
    }
    if (requested == existing) return true;

    MTAPIRES result = clearFn(day);
    if (result != MT_RET_OK) {
        error = L"SDK rejected clearing the symbol sessions (MTAPIRES=" +
            std::to_wstring(static_cast<unsigned>(result)) + L").";
        return false;
    }
    for (const auto& [open, close] : requested) {
        session->Clear();
        if (session->Open(open) != MT_RET_OK || session->Close(close) != MT_RET_OK) {
            error = L"SDK rejected a session time.";
            return false;
        }
        result = addFn(day, session);
        if (result != MT_RET_OK) {
            error = L"SDK rejected adding a symbol session (MTAPIRES=" +
                std::to_wstring(static_cast<unsigned>(result)) + L").";
            return false;
        }
    }
    changed = true;
    return true;
}

std::wstring trim(std::wstring value) {
    const std::size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const std::size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

constexpr std::uint64_t volumeExtScale = 100000000;

std::wstring volumeText(std::uint64_t volumeExt) {
    const std::uint64_t whole = volumeExt / volumeExtScale;
    const std::uint64_t fraction = volumeExt % volumeExtScale;
    if (fraction == 0) return std::to_wstring(whole);

    std::wstring fractionalText = std::to_wstring(fraction);
    fractionalText.insert(0, 8 - fractionalText.size(), L'0');
    while (!fractionalText.empty() && fractionalText.back() == L'0') fractionalText.pop_back();
    return std::to_wstring(whole) + L"." + fractionalText;
}

bool parseVolumeLots(std::wstring_view text, std::uint64_t& volumeExt,
                     std::wstring& error) {
    const std::wstring value = trim(std::wstring(text));
    constexpr std::uint64_t maxVolumeExt = MTAPI_VOLUME_EXT_MAX;
    constexpr std::int64_t maxExponent = 10000;
    const auto invalidValue = [&error]() {
        error = L"Enter a non-negative volume in lots with up to 8 decimal places.";
    };
    if (value.empty() || value.size() > 256) {
        invalidValue();
        return false;
    }

    const std::size_t exponentPosition = value.find_first_of(L"eE");
    if (exponentPosition != std::wstring::npos &&
        value.find_first_of(L"eE", exponentPosition + 1) != std::wstring::npos) {
        invalidValue();
        return false;
    }

    const std::size_t mantissaEnd = exponentPosition == std::wstring::npos
        ? value.size() : exponentPosition;
    std::wstring digits;
    digits.reserve(mantissaEnd);
    std::size_t digitsBeforeDecimal = 0;
    bool decimalSeen = false;
    bool digitSeen = false;
    for (std::size_t index = 0; index < mantissaEnd; ++index) {
        const wchar_t ch = value[index];
        if (ch == L'.') {
            if (decimalSeen) {
                invalidValue();
                return false;
            }
            decimalSeen = true;
            continue;
        }
        if (ch < L'0' || ch > L'9') {
            invalidValue();
            return false;
        }
        digitSeen = true;
        digits.push_back(ch);
        if (!decimalSeen) ++digitsBeforeDecimal;
    }
    if (!digitSeen) {
        invalidValue();
        return false;
    }

    std::int64_t exponent = 0;
    if (exponentPosition != std::wstring::npos) {
        std::size_t index = exponentPosition + 1;
        bool negativeExponent = false;
        if (index < value.size() && (value[index] == L'+' || value[index] == L'-')) {
            negativeExponent = value[index] == L'-';
            ++index;
        }
        if (index == value.size()) {
            invalidValue();
            return false;
        }
        std::int64_t magnitude = 0;
        for (; index < value.size(); ++index) {
            const wchar_t ch = value[index];
            if (ch < L'0' || ch > L'9') {
                invalidValue();
                return false;
            }
            const std::int64_t digit = ch - L'0';
            if (magnitude > (maxExponent - digit) / 10) {
                error = negativeExponent
                    ? L"Volume precision cannot exceed 8 decimal places."
                    : L"The volume is too large.";
                return false;
            }
            magnitude = magnitude * 10 + digit;
        }
        exponent = negativeExponent ? -magnitude : magnitude;
    }

    const std::size_t firstNonZero = digits.find_first_not_of(L'0');
    if (firstNonZero == std::wstring::npos) {
        volumeExt = 0;
        return true;
    }

    // Excel may serialize a valid step as the full decimal expansion of its binary double,
    // for example 9.9999999999999995E-7 instead of 0.000001.
    const auto acceptExcelFloatingPointValue = [&]() {
        wchar_t* end = nullptr;
        errno = 0;
        const double lots = std::wcstod(value.c_str(), &end);
        if (errno == ERANGE || end != value.c_str() + value.size() ||
            !std::isfinite(lots) || lots < 0) {
            return false;
        }

        const long double scaledLots = static_cast<long double>(lots) * volumeExtScale;
        if (!std::isfinite(scaledLots) || scaledLots < 0 ||
            scaledLots > static_cast<long double>(maxVolumeExt)) {
            return false;
        }

        const long double nearestVolumeExt = std::round(scaledLots);
        constexpr long double maxFloatingPointError = 0.0001L;
        const long double tolerance = std::numeric_limits<double>::epsilon() *
            (std::max)(std::abs(scaledLots), 1.0L) * 4.0L;
        if (tolerance > maxFloatingPointError ||
            std::abs(scaledLots - nearestVolumeExt) > tolerance) {
            return false;
        }

        volumeExt = static_cast<std::uint64_t>(nearestVolumeExt);
        return true;
    };

    const std::int64_t scaledDigits = static_cast<std::int64_t>(digitsBeforeDecimal) +
        exponent + 8 - static_cast<std::int64_t>(firstNonZero);
    if (scaledDigits <= 0) {
        if (acceptExcelFloatingPointValue()) return true;
        error = L"Volume precision cannot exceed 8 decimal places.";
        return false;
    }

    const std::size_t significantDigits = digits.size() - firstNonZero;
    const std::size_t integerDigits = static_cast<std::size_t>(scaledDigits);
    if (integerDigits < significantDigits) {
        const auto discardedStart = digits.begin() +
            static_cast<std::ptrdiff_t>(firstNonZero + integerDigits);
        if (std::any_of(discardedStart, digits.end(), [](wchar_t ch) { return ch != L'0'; })) {
            if (acceptExcelFloatingPointValue()) return true;
            error = L"Volume precision cannot exceed 8 decimal places.";
            return false;
        }
    }

    std::uint64_t scaledVolume = 0;
    const std::size_t mantissaDigits = integerDigits < significantDigits
        ? integerDigits : significantDigits;
    for (std::size_t index = 0; index < mantissaDigits; ++index) {
        const std::uint64_t digit = static_cast<std::uint64_t>(digits[firstNonZero + index] - L'0');
        if (scaledVolume > (maxVolumeExt - digit) / 10) {
            error = L"The volume is too large.";
            return false;
        }
        scaledVolume = scaledVolume * 10 + digit;
    }
    for (std::size_t index = mantissaDigits; index < integerDigits; ++index) {
        if (scaledVolume > maxVolumeExt / 10) {
            error = L"The volume is too large.";
            return false;
        }
        scaledVolume *= 10;
    }

    volumeExt = scaledVolume;
    return true;
}

template <typename VolumeGetter, typename VolumeSetter,
          typename VolumeExtGetter, typename VolumeExtSetter>
bool writeVolumeLots(std::wstring_view text,
                     VolumeGetter volumeGetter, VolumeSetter volumeSetter,
                     VolumeExtGetter volumeExtGetter, VolumeExtSetter volumeExtSetter,
                     bool& changed, std::wstring& error) {
    std::uint64_t volumeExt = 0;
    if (!parseVolumeLots(text, volumeExt, error)) return false;

    const std::uint64_t volume = SMTMath::VolumeFromVolumeExt(volumeExt);
    const std::uint64_t oldVolume = volumeGetter();
    const std::uint64_t oldVolumeExt = volumeExtGetter();
    if (volume == oldVolume && volumeExt == oldVolumeExt) return true;

    if (volume != oldVolume) {
        const MTAPIRES result = volumeSetter(volume);
        if (result != MT_RET_OK) {
            error = L"SDK rejected the volume value (MTAPIRES=" +
                std::to_wstring(static_cast<unsigned>(result)) + L").";
            return false;
        }
    }
    if (volumeExtGetter() != volumeExt) {
        const MTAPIRES result = volumeExtSetter(volumeExt);
        if (result != MT_RET_OK) {
            error = L"SDK rejected the extended volume value (MTAPIRES=" +
                std::to_wstring(static_cast<unsigned>(result)) + L").";
            return false;
        }
    }
    if (volumeGetter() != volume || volumeExtGetter() != volumeExt) {
        error = L"SDK did not retain the synchronized volume values.";
        return false;
    }
    changed = true;
    return true;
}

bool isHiddenVolumeExtField(std::wstring_view name) {
    return name == L"VolumeMinExt" || name == L"VolumeMaxExt" ||
           name == L"VolumeStepExt" || name == L"VolumeLimitExt";
}

std::wstring decodeUtf8(const std::string& bytes) {
    if (bytes.empty()) return {};
    const int offset = bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF ? 3 : 0;
    const int length = static_cast<int>(bytes.size()) - offset;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data() + offset, length, nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            bytes.data() + offset, length, result.data(), required) != required) {
        return {};
    }
    return result;
}

std::string encodeUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            text.data(), length, result.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

bool canonicalValueCode(std::wstring_view text, std::wstring& canonical) {
    if (text.empty()) return false;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
    }
    const std::wstring owned(text);
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::wcstoull(owned.c_str(), &end, 10);
    if (errno == ERANGE || end != owned.c_str() + owned.size()) return false;
    canonical = std::to_wstring(parsed);
    return true;
}

bool canonicalAlpha3Code(std::wstring_view text, std::wstring& canonical) {
    if (text.size() != 3) return false;
    canonical.clear();
    canonical.reserve(3);
    for (const wchar_t ch : text) {
        if (ch >= L'a' && ch <= L'z') canonical.push_back(ch - (L'a' - L'A'));
        else if (ch >= L'A' && ch <= L'Z') canonical.push_back(ch);
        else return false;
    }
    return true;
}

bool equalsIgnoreCase(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() &&
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                             right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool parseFlagCode(std::wstring_view text, std::uint64_t& value,
                   std::wstring* canonical = nullptr) {
    std::wstring normalized;
    if (!canonicalValueCode(text, normalized)) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::wcstoull(normalized.c_str(), &end, 10);
    if (errno == ERANGE || end != normalized.c_str() + normalized.size()) return false;
    value = static_cast<std::uint64_t>(parsed);
    if (canonical) *canonical = std::move(normalized);
    return true;
}

bool parseConstraintNumber(std::wstring_view text, double& value) {
    const std::wstring cleaned = trim(std::wstring(text));
    if (cleaned.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const double parsed = std::wcstod(cleaned.c_str(), &end);
    if (errno == ERANGE || end != cleaned.c_str() + cleaned.size() || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parseSymbolValueConstraint(std::wstring_view text,
                                SymbolValueConstraint& constraint) {
    std::array<std::wstring, 4> parts;
    std::size_t start = 0;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const std::size_t separator = text.find(L'|', start);
        if (index + 1 < parts.size() && separator == std::wstring_view::npos) return false;
        if (index + 1 == parts.size() && separator != std::wstring_view::npos) return false;
        parts[index] = trim(std::wstring(text.substr(start,
            separator == std::wstring_view::npos ? std::wstring_view::npos : separator - start)));
        if (separator == std::wstring_view::npos) break;
        start = separator + 1;
    }

    if (!parts[0].empty()) {
        if (!parseConstraintNumber(parts[0], constraint.minimum)) return false;
        constraint.hasMinimum = true;
    }
    if (!parts[1].empty()) {
        if (!parseConstraintNumber(parts[1], constraint.maximum)) return false;
        constraint.hasMaximum = true;
    }
    if (constraint.hasMinimum && constraint.hasMaximum &&
        constraint.minimum > constraint.maximum) return false;
    if (!parts[2].empty()) {
        for (const wchar_t ch : parts[2]) {
            if (ch < L'0' || ch > L'9') return false;
        }
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::wcstoull(parts[2].c_str(), &end, 10);
        if (errno == ERANGE || end != parts[2].c_str() + parts[2].size() ||
            parsed > (std::numeric_limits<std::size_t>::max)()) return false;
        constraint.maxChars = static_cast<std::size_t>(parsed);
        constraint.hasMaxChars = true;
    }
    if (!parts[3].empty()) {
        std::size_t rangeStart = 0;
        while (rangeStart <= parts[3].size()) {
            const std::size_t comma = parts[3].find(L',', rangeStart);
            const std::wstring token = trim(parts[3].substr(rangeStart,
                comma == std::wstring::npos ? std::wstring::npos : comma - rangeStart));
            if (token.empty()) return false;
            const std::size_t rangeSeparator = token.find(L"..");
            SymbolValueRange range;
            if (rangeSeparator == std::wstring::npos) {
                if (!parseConstraintNumber(token, range.minimum)) return false;
                range.maximum = range.minimum;
            } else {
                if (token.find(L"..", rangeSeparator + 2) != std::wstring::npos ||
                    !parseConstraintNumber(std::wstring_view(token).substr(0, rangeSeparator), range.minimum) ||
                    !parseConstraintNumber(std::wstring_view(token).substr(rangeSeparator + 2), range.maximum) ||
                    range.minimum > range.maximum) return false;
            }
            constraint.allowedValues.push_back(range);
            if (comma == std::wstring::npos) break;
            rangeStart = comma + 1;
        }
    }
    return true;
}

std::wstring sdkConstraintForField(std::wstring_view sourceName) {
    // Enum codes, bit masks and unsigned integer limits below are derived from
    // the field declarations in the installed MT5 SDK. Other fields get empty
    // template slots because the SDK does not define semantic limits.
    if (sourceName == L"Sector") return L"|||0..15";
    if (sourceName == L"Industry") return L"|||0,1..14,51..57,101..123,151..162,201..208,251..269,301..311,351..375,401..412,451..462,501..506,551..554";
    if (sourceName == L"CalcMode") return L"|||0..5,32..39,64";
    if (sourceName == L"TradeMode") return L"|||0..4";
    if (sourceName == L"ExecMode") return L"|||0..3";
    if (sourceName == L"GTCMode") return L"|||0..2";
    if (sourceName == L"FillFlags") return L"0|7||";
    if (sourceName == L"ExpirFlags") return L"0|15||";
    if (sourceName == L"OrderFlags") return L"0|127||";
    if (sourceName == L"TradeFlags") return L"0|3||";
    if (sourceName == L"MarginFlags") return L"0|31||";
    if (sourceName == L"SwapMode") return L"|||0..9";
    if (sourceName == L"Swap3Day") return L"|||0..7";
    if (sourceName == L"SwapFlags" || sourceName == L"REFlags" || sourceName == L"IEFlags") {
        return L"0|1||";
    }
    if (sourceName == L"IECheckMode" || sourceName == L"SpliceTimeType") return L"|||0";
    if (sourceName == L"OptionsMode") return L"|||0..3";
    if (sourceName == L"SpliceType") return L"|||0..2";
    if (sourceName == L"ChartMode") return L"|||0,1,255";
    if (sourceName == L"BookVolumeMode") return L"|||0..2";
    if (sourceName == L"TickFlags") return L"0|15||";
    if (sourceName == L"VolumeMin" || sourceName == L"VolumeMax" ||
        sourceName == L"VolumeStep" || sourceName == L"VolumeLimit" ||
        sourceName == L"IEVolumeMax" || sourceName == L"IEVolumeMaxExt") {
        return L"0|||";
    }
    constexpr std::wstring_view unsigned32Fields[] = {
        L"Digits", L"Spread", L"CurrencyBaseDigits", L"CurrencyProfitDigits",
        L"CurrencyMarginDigits", L"TickBookDepth", L"FilterSoft", L"FilterSoftTicks",
        L"FilterHard", L"FilterHardTicks", L"FilterDiscard", L"FilterGap",
        L"FilterGapTicks", L"FilterSpreadMin", L"FilterSpreadMax", L"QuotesTimeout",
        L"RETimeout", L"IETimeout", L"IESlipProfit", L"IESlipLosing",
        L"SpliceTimeDays", L"SubscriptionsDelay"
    };
    if (std::find(std::begin(unsigned32Fields), std::end(unsigned32Fields), sourceName) !=
        std::end(unsigned32Fields)) return L"0|4294967295||";
    return L"|||";
}

std::wstring serverValueForField(const SymbolFieldMapping& mapping,
                                 const std::wstring& value) {
    return SymbolValueForStorage(mapping, value);
}

bool writeDefaultSymbolMappings(const std::wstring& path,
                                const std::vector<Property>& properties,
                                std::wstring& error) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        error = L"Cannot create symbol field configuration: " + path;
        return false;
    }

    output << "# MT5 Symbol Manager field mappings.\n"
              "# Format: SDK field name=column heading. The line order sets the table and export order.\n"
              "# A blank line between groups draws a vertical divider before the next column.\n"
              "# Remove a line to omit that field. Keep each source field name unchanged.\n";
    for (const Property& property : properties) {
        if (isHiddenVolumeExtField(property.name)) continue;
        const std::string source = encodeUtf8(property.name);
        if (source.empty()) {
            error = L"Cannot encode a symbol field name in UTF-8.";
            return false;
        }
        output << source << '=' << source << '\n';
    }
    output << "\n# Value constraints. Format: SourceField=minimum|maximum|maxChars|allowedValues.\n"
              "# Empty slots disable that check. Numeric bounds use displayed units.\n"
              "# allowedValues is a comma-separated list of numbers and inclusive min..max ranges.\n"
              "[Constraints]\n";
    for (const Property& property : properties) {
        if (isHiddenVolumeExtField(property.name)) continue;
        const std::string source = encodeUtf8(property.name);
        if (source.empty()) {
            error = L"Cannot encode a symbol field name in UTF-8.";
            return false;
        }
        output << source << '=' << encodeUtf8(sdkConstraintForField(property.name)) << '\n';
    }
    output << '\n' << kDefaultSymbolValueMaps << '\n';
    output.close();
    if (!output) {
        error = L"Cannot write symbol field configuration: " + path;
        return false;
    }
    return true;
}

std::vector<Property> applyFieldMappings(
    const std::vector<SymbolFieldMapping>& mappings,
    const std::vector<Property>& availableProperties) {
    std::vector<Property> properties;
    properties.reserve(mappings.size());
    for (const SymbolFieldMapping& mapping : mappings) {
        const auto source = std::find_if(availableProperties.begin(), availableProperties.end(),
            [&mapping](const Property& property) { return property.name == mapping.sourceName; });
        if (source == availableProperties.end()) {
            throw std::logic_error("A symbol field mapping was not validated.");
        }
        Property property = *source;
        property.name = mapping.displayName;
        properties.push_back(std::move(property));
    }
    return properties;
}

std::vector<Property> makeProperties() {
    std::vector<Property> properties;
    properties.reserve(140);

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)
#define WIDEN_INNER(value) L##value
#define WIDEN(value) WIDEN_INNER(value)
#define ADD_FIELD(name) \
    properties.push_back({WIDEN(STRINGIFY(name)), \
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { \
            return numberText(symbol.name()); \
        }, \
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*, \
           bool& changed, std::wstring& error) { \
            return writeNumber(text, [&]() { return symbol.name(); }, \
                [&](auto value) { return symbol.name(value); }, changed, error); \
        }}); \
    properties.back().valueKind = numericValueKind<decltype(std::declval<const IMTConSymbol&>().name())>()
#define ADD_PRECISION8_FIELD(name) \
    properties.push_back({WIDEN(STRINGIFY(name)), \
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { \
            return precision8NumberText(symbol.name()); \
        }, \
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*, \
           bool& changed, std::wstring& error) { \
            return writePrecision8Number(text, [&]() { return symbol.name(); }, \
                [&](auto value) { return symbol.name(value); }, changed, error); \
        }}); \
    properties.back().valueKind = SymbolFieldValueKind::FloatingPoint
#define ADD_VOLUME_FIELD(name) \
    properties.push_back({WIDEN(STRINGIFY(name)), \
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { \
            const std::uint64_t volumeExt = symbol.name##Ext(); \
            const std::uint64_t volume = volumeExt != 0 ? volumeExt : \
                SMTMath::VolumeExtFromVolume(symbol.name()); \
            return volumeText(volume); \
        }, \
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*, \
           bool& changed, std::wstring& error) { \
            return writeVolumeLots(text, \
                [&]() { return symbol.name(); }, \
                [&](std::uint64_t value) { return symbol.name(value); }, \
                [&]() { return symbol.name##Ext(); }, \
                [&](std::uint64_t value) { return symbol.name##Ext(value); }, \
                changed, error); \
        }}); \
    properties.back().valueKind = SymbolFieldValueKind::VolumeLots
#define ADD_TEXT_FIELD(name) \
    properties.push_back({WIDEN(STRINGIFY(name)), \
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { \
            return cleanText(symbol.name()); \
        }, \
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*, \
           bool& changed, std::wstring& error) { \
            return writeText(text, [&]() { return symbol.name(); }, \
                [&](LPCWSTR value) { return symbol.name(value); }, changed, error); \
        }}); \
    properties.back().valueKind = SymbolFieldValueKind::Text
#define ADD_READONLY_FIELD(name) \
    properties.push_back({WIDEN(STRINGIFY(name)), \
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { \
            return numberText(symbol.name()); \
        }, \
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*, \
           bool&, std::wstring& error) { \
            return validateReadOnlyNumber(text, [&]() { return symbol.name(); }, \
                WIDEN(STRINGIFY(name)), error); \
        }, false}); \
    properties.back().valueKind = numericValueKind<decltype(std::declval<const IMTConSymbol&>().name())>()
#define ADD_COLOR_FIELD(name) \
    properties.push_back({WIDEN(STRINGIFY(name)), \
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { return colorText(symbol.name()); }, \
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*, \
           bool& changed, std::wstring& error) { \
            return writeColor(text, [&]() { return symbol.name(); }, \
                [&](COLORREF value) { return symbol.name(value); }, changed, error); \
        }}); \
    properties.back().valueKind = SymbolFieldValueKind::Color

    properties.push_back({L"Symbol",
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { return cleanText(symbol.Symbol()); },
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*,
           bool&, std::wstring& error) {
            if (text == cleanText(symbol.Symbol())) return true;
            error = L"The Symbol field cannot be changed during import.";
            return false;
        }, false});
    properties.push_back({L"Path",
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { return cleanText(symbol.Path()); },
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*,
           bool&, std::wstring& error) {
            if (text == cleanText(symbol.Path())) return true;
            error = L"The Path field cannot be changed during import.";
            return false;
        }, false});
    properties[properties.size() - 2].valueKind = SymbolFieldValueKind::Text;
    properties.back().valueKind = SymbolFieldValueKind::Text;
    ADD_TEXT_FIELD(ISIN);
    ADD_TEXT_FIELD(Description);
    ADD_TEXT_FIELD(International);
    ADD_TEXT_FIELD(Basis);
    ADD_TEXT_FIELD(Source);
    ADD_TEXT_FIELD(Page);
    ADD_TEXT_FIELD(CurrencyBase);
    properties.push_back({L"CurrencyBaseDigits",
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { return numberText(symbol.CurrencyBaseDigits()); },
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*,
           bool& changed, std::wstring& error) {
            return writeNumber(text, [&]() { return symbol.CurrencyBaseDigits(); },
                [&](auto value) { return symbol.CurrencyBaseDigitsSet(value); }, changed, error);
        }});
    properties.back().valueKind = numericValueKind<decltype(std::declval<const IMTConSymbol&>().CurrencyBaseDigits())>();
    ADD_TEXT_FIELD(CurrencyProfit);
    properties.push_back({L"CurrencyProfitDigits",
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { return numberText(symbol.CurrencyProfitDigits()); },
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*,
           bool& changed, std::wstring& error) {
            return writeNumber(text, [&]() { return symbol.CurrencyProfitDigits(); },
                [&](auto value) { return symbol.CurrencyProfitDigitsSet(value); }, changed, error);
        }});
    properties.back().valueKind = numericValueKind<decltype(std::declval<const IMTConSymbol&>().CurrencyProfitDigits())>();
    ADD_TEXT_FIELD(CurrencyMargin);
    properties.push_back({L"CurrencyMarginDigits",
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { return numberText(symbol.CurrencyMarginDigits()); },
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*,
           bool& changed, std::wstring& error) {
            return writeNumber(text, [&]() { return symbol.CurrencyMarginDigits(); },
                [&](auto value) { return symbol.CurrencyMarginDigitsSet(value); }, changed, error);
        }});
    properties.back().valueKind = numericValueKind<decltype(std::declval<const IMTConSymbol&>().CurrencyMarginDigits())>();
    ADD_COLOR_FIELD(Color);
    ADD_COLOR_FIELD(ColorBackground);
    ADD_FIELD(Digits);
    ADD_READONLY_FIELD(Point);
    ADD_READONLY_FIELD(Multiply);
    ADD_FIELD(TickFlags);
    ADD_FIELD(TickBookDepth);
    ADD_FIELD(FilterSoft);
    ADD_FIELD(FilterSoftTicks);
    ADD_FIELD(FilterHard);
    ADD_FIELD(FilterHardTicks);
    ADD_FIELD(FilterDiscard);
    ADD_FIELD(FilterSpreadMax);
    ADD_FIELD(FilterSpreadMin);
    ADD_FIELD(TradeMode);
    ADD_FIELD(CalcMode);
    ADD_FIELD(ExecMode);
    ADD_FIELD(GTCMode);
    ADD_FIELD(FillFlags);
    ADD_FIELD(ExpirFlags);
    ADD_FIELD(Spread);
    ADD_FIELD(SpreadBalance);
    ADD_FIELD(SpreadDiff);
    ADD_FIELD(SpreadDiffBalance);
    ADD_FIELD(TickValue);
    ADD_FIELD(TickSize);
    ADD_FIELD(ContractSize);
    ADD_FIELD(StopsLevel);
    ADD_FIELD(FreezeLevel);
    ADD_FIELD(QuotesTimeout);
    ADD_VOLUME_FIELD(VolumeMin);
    ADD_VOLUME_FIELD(VolumeMax);
    ADD_VOLUME_FIELD(VolumeStep);
    ADD_VOLUME_FIELD(VolumeLimit);
    ADD_FIELD(MarginFlags);
    ADD_FIELD(MarginInitial);
    ADD_FIELD(MarginMaintenance);
    ADD_FIELD(MarginLong);
    ADD_FIELD(MarginShort);
    ADD_FIELD(MarginLimit);
    ADD_FIELD(MarginStop);
    ADD_FIELD(MarginStopLimit);
    ADD_FIELD(SwapMode);
    ADD_PRECISION8_FIELD(SwapLong);
    ADD_PRECISION8_FIELD(SwapShort);
    ADD_FIELD(Swap3Day);
    properties.push_back({L"TimeStart",
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) {
            return timestampText(symbol.TimeStart());
        }, [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*,
             bool& changed, std::wstring& error) {
            return writeTimestamp(text, [&]() { return symbol.TimeStart(); },
                [&](std::int64_t value) { return symbol.TimeStart(value); }, changed, error);
        }});
    properties.back().valueKind = SymbolFieldValueKind::Timestamp;
    properties.push_back({L"TimeExpiration",
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) {
            return timestampText(symbol.TimeExpiration());
        }, [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*,
             bool& changed, std::wstring& error) {
            return writeTimestamp(text, [&]() { return symbol.TimeExpiration(); },
                [&](std::int64_t value) { return symbol.TimeExpiration(value); }, changed, error);
        }});
    properties.back().valueKind = SymbolFieldValueKind::Timestamp;
    ADD_FIELD(REFlags);
    ADD_FIELD(RETimeout);
    ADD_FIELD(IECheckMode);
    ADD_FIELD(IETimeout);
    ADD_FIELD(IESlipProfit);
    ADD_FIELD(IESlipLosing);
    ADD_FIELD(IEVolumeMax);
    ADD_FIELD(PriceSettle);
    ADD_FIELD(PriceLimitMax);
    ADD_FIELD(PriceLimitMin);
    ADD_FIELD(TradeFlags);
    ADD_FIELD(OrderFlags);

    static constexpr const wchar_t* marginRateNames[] = {
        L"Buy", L"Sell", L"BuyLimit", L"SellLimit",
        L"BuyStop", L"SellStop", L"BuyStopLimit", L"SellStopLimit"
    };
    for (std::uint32_t type = 0; type < std::size(marginRateNames); ++type) {
        const std::wstring initialName = std::wstring(L"MarginRateInitial.") + marginRateNames[type];
        properties.push_back({initialName, [type](const IMTConSymbol& s, IMTConSymbolSession*) {
            return precision8NumberText(s.MarginRateInitial(type));
        }, [type](IMTConSymbol& s, std::wstring_view text, IMTConSymbolSession*,
                  bool& changed, std::wstring& error) {
            return writePrecision8Number(text, [&]() { return s.MarginRateInitial(type); },
                [&](auto value) { return s.MarginRateInitial(type, value); }, changed, error);
        }});
        properties.back().valueKind = SymbolFieldValueKind::FloatingPoint;
        const std::wstring maintenanceName = std::wstring(L"MarginRateMaintenance.") + marginRateNames[type];
        properties.push_back({maintenanceName, [type](const IMTConSymbol& s, IMTConSymbolSession*) {
            return precision8NumberText(s.MarginRateMaintenance(type));
        }, [type](IMTConSymbol& s, std::wstring_view text, IMTConSymbolSession*,
                  bool& changed, std::wstring& error) {
            return writePrecision8Number(text, [&]() { return s.MarginRateMaintenance(type); },
                [&](auto value) { return s.MarginRateMaintenance(type, value); }, changed, error);
        }});
        properties.back().valueKind = SymbolFieldValueKind::FloatingPoint;
    }

    ADD_FIELD(OptionsMode);
    ADD_FIELD(PriceStrike);
    ADD_PRECISION8_FIELD(MarginRateLiquidity);
    ADD_FIELD(FaceValue);
    ADD_FIELD(AccruedInterest);
    ADD_FIELD(SpliceType);
    ADD_FIELD(SpliceTimeType);
    ADD_FIELD(SpliceTimeDays);
    ADD_FIELD(MarginHedged);
    ADD_PRECISION8_FIELD(MarginRateCurrency);
    ADD_FIELD(FilterGap);
    ADD_FIELD(FilterGapTicks);
    ADD_FIELD(ChartMode);
    ADD_FIELD(IEFlags);
    ADD_FIELD(VolumeMinExt);
    ADD_FIELD(VolumeMaxExt);
    ADD_FIELD(VolumeStepExt);
    ADD_FIELD(VolumeLimitExt);
    ADD_FIELD(IEVolumeMaxExt);
    ADD_TEXT_FIELD(Category);
    ADD_TEXT_FIELD(Exchange);
    ADD_TEXT_FIELD(CFI);
    ADD_FIELD(Sector);
    ADD_FIELD(Industry);
    ADD_TEXT_FIELD(Country);
    ADD_FIELD(SubscriptionsDelay);
    ADD_FIELD(SwapYearDays);
    ADD_FIELD(SwapFlags);
    ADD_FIELD(SwapRateSunday);
    ADD_FIELD(SwapRateMonday);
    ADD_FIELD(SwapRateTuesday);
    ADD_FIELD(SwapRateWednesday);
    ADD_FIELD(SwapRateThursday);
    ADD_FIELD(SwapRateFriday);
    ADD_FIELD(SwapRateSaturday);
    ADD_FIELD(BookVolumeMode);

    static constexpr const wchar_t* dayNames[] = {
        L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"
    };
    for (std::uint32_t day = 0; day < std::size(dayNames); ++day) {
        const std::wstring quoteName = std::wstring(L"QuoteSessions.") + dayNames[day];
        properties.push_back({quoteName, [day](const IMTConSymbol& s, IMTConSymbolSession* session) {
            const std::uint32_t total = s.SessionQuoteTotal(day);
            std::wstring value;
            for (std::uint32_t pos = 0; pos < total; ++pos) {
                const MTAPIRES result = s.SessionQuoteNext(day, pos, session);
                if (result != MT_RET_OK) {
                    if (!value.empty()) value += L"; ";
                    value += L"[MTAPIRES=" + std::to_wstring(static_cast<unsigned>(result)) + L"]";
                    continue;
                }
                if (!value.empty()) value += L"; ";
                value += minuteTime(session->Open()) + L"-" + minuteTime(session->Close());
            }
            return value;
        }, [day](IMTConSymbol& s, std::wstring_view text, IMTConSymbolSession* session,
                 bool& changed, std::wstring& error) {
            return writeSessions(text, day, session,
                [&](std::uint32_t weekday) { return s.SessionQuoteTotal(weekday); },
                [&](std::uint32_t weekday, std::uint32_t pos, IMTConSymbolSession* item) {
                    return s.SessionQuoteNext(weekday, pos, item);
                },
                [&](std::uint32_t weekday) { return s.SessionQuoteClear(weekday); },
                [&](std::uint32_t weekday, IMTConSymbolSession* item) {
                    return s.SessionQuoteAdd(weekday, item);
                }, changed, error);
        }});
        properties.back().valueKind = SymbolFieldValueKind::Session;

        const std::wstring tradeName = std::wstring(L"TradeSessions.") + dayNames[day];
        properties.push_back({tradeName, [day](const IMTConSymbol& s, IMTConSymbolSession* session) {
            const std::uint32_t total = s.SessionTradeTotal(day);
            std::wstring value;
            for (std::uint32_t pos = 0; pos < total; ++pos) {
                const MTAPIRES result = s.SessionTradeNext(day, pos, session);
                if (result != MT_RET_OK) {
                    if (!value.empty()) value += L"; ";
                    value += L"[MTAPIRES=" + std::to_wstring(static_cast<unsigned>(result)) + L"]";
                    continue;
                }
                if (!value.empty()) value += L"; ";
                value += minuteTime(session->Open()) + L"-" + minuteTime(session->Close());
            }
            return value;
        }, [day](IMTConSymbol& s, std::wstring_view text, IMTConSymbolSession* session,
                 bool& changed, std::wstring& error) {
            return writeSessions(text, day, session,
                [&](std::uint32_t weekday) { return s.SessionTradeTotal(weekday); },
                [&](std::uint32_t weekday, std::uint32_t pos, IMTConSymbolSession* item) {
                    return s.SessionTradeNext(weekday, pos, item);
                },
                [&](std::uint32_t weekday) { return s.SessionTradeClear(weekday); },
                [&](std::uint32_t weekday, IMTConSymbolSession* item) {
                    return s.SessionTradeAdd(weekday, item);
                }, changed, error);
        }});
        properties.back().valueKind = SymbolFieldValueKind::Session;
    }

#undef ADD_TEXT_FIELD
#undef ADD_VOLUME_FIELD
#undef ADD_COLOR_FIELD
#undef ADD_PRECISION8_FIELD
#undef ADD_READONLY_FIELD
#undef ADD_FIELD
#undef WIDEN
#undef WIDEN_INNER
#undef STRINGIFY
#undef STRINGIFY_INNER

    return properties;
}

class ManagerConnection {
public:
    ~ManagerConnection() {
        if (admin_) {
            admin_->Disconnect();
            admin_->Release();
        }
        if (initialized_) factory_.Shutdown();
    }

    bool connect(const std::wstring& server, std::uint64_t login,
                 const std::wstring& password, std::wstring& error) {
        MTAPIRES result = factory_.Initialize();
        if (result != MT_RET_OK) {
            error = apiError(L"MT5 API initialization", result);
            return false;
        }
        initialized_ = true;

        std::uint32_t version = 0;
        result = factory_.Version(version);
        if (result != MT_RET_OK) {
            error = apiError(L"Reading MT5 API version", result);
            return false;
        }

        result = factory_.CreateAdmin(version, &admin_);
        if (result != MT_RET_OK || !admin_) {
            error = result == MT_RET_OK ? L"MT5 API returned an empty administrator object."
                                        : apiError(L"Creating MT5 administrator object", result);
            return false;
        }

        result = admin_->Connect(server.c_str(), login, password.c_str(), nullptr, 0, 30000);
        if (result != MT_RET_OK) {
            error = apiError(L"Connecting to MT5 server", result);
            return false;
        }
        return true;
    }

    IMTAdminAPI* admin() const { return admin_; }

private:
    CMTManagerAPIFactory factory_;
    IMTAdminAPI* admin_ = nullptr;
    bool initialized_ = false;
};

std::string csvCell(std::wstring_view value) {
    std::string encoded = encodeUtf8(value);
    if (encoded.find_first_of(",\"\r\n") == std::string::npos) return encoded;
    std::string quoted;
    quoted.reserve(encoded.size() + 2);
    quoted.push_back('"');
    for (const char ch : encoded) {
        if (ch == '"') quoted.push_back('"');
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

bool saveSnapshotCsv(const SymbolSnapshot& snapshot, const std::wstring& path,
                     std::wstring& error) {
    std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!output) {
        error = L"Cannot create the symbol backup CSV: " + path;
        return false;
    }
    output.write("\xEF\xBB\xBF", 3);
    for (std::size_t column = 0; column < snapshot.columns.size(); ++column) {
        if (column) output.put(',');
        output << csvCell(snapshot.columns[column]);
    }
    output << "\r\n";
    for (const auto& row : snapshot.rows) {
        for (std::size_t column = 0; column < snapshot.columns.size(); ++column) {
            if (column) output.put(',');
            if (column < row.size()) output << csvCell(row[column]);
        }
        output << "\r\n";
    }
    output.close();
    if (!output) {
        error = L"Failed to finish writing the symbol backup CSV: " + path;
        return false;
    }
    return true;
}

std::wstring backupFilePath(const std::wstring& directory) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[128]{};
    swprintf_s(name, L"symbols_backup_%04u%02u%02u_%02u%02u%02u_%03u_%lu.csv",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, GetCurrentProcessId());
    return directory + L"\\" + name;
}

void releaseClones(std::unordered_map<std::wstring, IMTConSymbol*>& clones) {
    for (auto& [name, symbol] : clones) {
        (void)name;
        if (symbol) symbol->Release();
    }
    clones.clear();
}

} // namespace

std::wstring SymbolValueForDisplay(const SymbolFieldMapping& mapping,
                                   const std::wstring& value) {
    const std::wstring cleaned = trim(value);
    if (!mapping.textMappings.empty()) {
        std::wstring canonical;
        if (!canonicalAlpha3Code(cleaned, canonical)) return value;
        for (const auto& [code, label] : mapping.textMappings) {
            if (canonical == code) return label;
        }
        return value;
    }

    std::wstring canonical;
    if (!mapping.valueMappings.empty() && canonicalValueCode(cleaned, canonical)) {
        for (const auto& [code, label] : mapping.valueMappings) {
            if (canonical == code) return label;
        }
        return canonical;
    }

    if (!mapping.flagMappings.empty()) {
        std::uint64_t flags = 0;
        if (!parseFlagCode(cleaned, flags, &canonical)) return value;
        for (const auto& [code, label] : mapping.flagMappings) {
            if (code == canonical) return label;
        }
        if (flags == 0) {
            return canonical;
        }

        std::uint64_t remaining = flags;
        std::wstring labels;
        for (const auto& [code, label] : mapping.flagMappings) {
            std::uint64_t bit = 0;
            if (!parseFlagCode(code, bit) || bit == 0 || (bit & (bit - 1)) != 0 ||
                label.find(L'|') != std::wstring::npos || (remaining & bit) != bit) continue;
            if (!labels.empty()) labels += L" | ";
            labels += label;
            remaining &= ~bit;
        }
        return remaining == 0 && !labels.empty() ? labels : canonical;
    }
    return value;
}

std::wstring SymbolValueForStorage(const SymbolFieldMapping& mapping,
                                   const std::wstring& value) {
    const std::wstring cleaned = trim(value);
    if (!mapping.textMappings.empty()) {
        for (const auto& [code, label] : mapping.textMappings) {
            if (equalsIgnoreCase(cleaned, label)) return code;
        }
        std::wstring canonical;
        if (canonicalAlpha3Code(cleaned, canonical)) {
            for (const auto& [code, label] : mapping.textMappings) {
                (void)label;
                if (canonical == code) return canonical;
            }
        }
        return value;
    }

    if (!mapping.valueMappings.empty()) {
        for (const auto& [code, label] : mapping.valueMappings) {
            if (cleaned == label) return code;
        }
        std::wstring canonical;
        if (canonicalValueCode(cleaned, canonical)) return canonical;
        return value;
    }

    if (!mapping.flagMappings.empty()) {
        for (const auto& [code, label] : mapping.flagMappings) {
            if (equalsIgnoreCase(cleaned, label)) return code;
        }
        std::uint64_t numeric = 0;
        std::wstring canonical;
        if (parseFlagCode(cleaned, numeric, &canonical)) return canonical;

        std::uint64_t combined = 0;
        std::size_t start = 0;
        while (start <= cleaned.size()) {
            const std::size_t separator = cleaned.find(L'|', start);
            const std::wstring label = trim(cleaned.substr(start,
                separator == std::wstring::npos ? std::wstring::npos : separator - start));
            const auto entry = std::find_if(mapping.flagMappings.begin(), mapping.flagMappings.end(),
                [&label](const auto& candidate) {
                    std::uint64_t code = 0;
                    return candidate.second == label && parseFlagCode(candidate.first, code) &&
                        code != 0 && (code & (code - 1)) == 0 &&
                        candidate.second.find(L'|') == std::wstring::npos;
                });
            std::uint64_t bit = 0;
            if (entry == mapping.flagMappings.end() || !parseFlagCode(entry->first, bit)) return value;
            combined |= bit;
            if (separator == std::wstring::npos) break;
            start = separator + 1;
        }
        return std::to_wstring(combined);
    }
    return value;
}

bool ValidateSymbolFieldValue(const SymbolFieldMapping& mapping,
                              std::wstring_view value,
                              std::wstring& error) {
    error.clear();
    const SymbolValueConstraint& constraint = mapping.constraint;
    if (constraint.hasMaxChars && value.size() > constraint.maxChars) {
        error = L"Value exceeds the configured maxChars limit of " +
            std::to_wstring(constraint.maxChars) + L".";
        return false;
    }

    const bool needsNumber = constraint.hasMinimum || constraint.hasMaximum ||
        !constraint.allowedValues.empty();
    double number = 0;
    bool hasNumericValue = false;
    switch (mapping.valueKind) {
    case SymbolFieldValueKind::SignedInteger32: {
        std::int32_t parsed = 0;
        if (!parseFieldNumber(value, parsed)) {
            error = L"Value must be a valid signed 32-bit integer.";
            return false;
        }
        number = static_cast<double>(parsed);
        hasNumericValue = true;
        break;
    }
    case SymbolFieldValueKind::UnsignedInteger32: {
        std::uint32_t parsed = 0;
        if (!parseFieldNumber(value, parsed)) {
            error = L"Value must be a valid unsigned 32-bit integer.";
            return false;
        }
        number = static_cast<double>(parsed);
        hasNumericValue = true;
        break;
    }
    case SymbolFieldValueKind::SignedInteger64: {
        std::int64_t parsed = 0;
        if (!parseFieldNumber(value, parsed)) {
            error = L"Value must be a valid signed 64-bit integer.";
            return false;
        }
        number = static_cast<double>(parsed);
        hasNumericValue = true;
        break;
    }
    case SymbolFieldValueKind::UnsignedInteger64: {
        std::uint64_t parsed = 0;
        if (!parseFieldNumber(value, parsed)) {
            error = L"Value must be a valid unsigned 64-bit integer.";
            return false;
        }
        number = static_cast<double>(parsed);
        hasNumericValue = true;
        break;
    }
    case SymbolFieldValueKind::FloatingPoint:
        if (!parseConstraintNumber(value, number)) {
            error = L"Value must be a finite number.";
            return false;
        }
        hasNumericValue = true;
        break;
    case SymbolFieldValueKind::VolumeLots: {
        std::uint64_t volumeExt = 0;
        if (!parseVolumeLots(value, volumeExt, error)) return false;
        if (!parseConstraintNumber(value, number)) {
            error = L"Value must be a finite volume in lots.";
            return false;
        }
        hasNumericValue = true;
        break;
    }
    case SymbolFieldValueKind::Color: {
        COLORREF color = 0;
        if (!parseColor(value, color)) {
            error = L"Value must be a color in #RRGGBB or 0xAARRGGBB format.";
            return false;
        }
        number = static_cast<double>(color);
        hasNumericValue = true;
        break;
    }
    case SymbolFieldValueKind::Timestamp: {
        std::int64_t timestamp = 0;
        if (!parseTimestamp(value, timestamp)) {
            error = L"Value must be a UTC date/time or Unix timestamp.";
            return false;
        }
        number = static_cast<double>(timestamp);
        hasNumericValue = true;
        break;
    }
    case SymbolFieldValueKind::Session: {
        std::vector<SessionRange> ranges;
        if (!parseSessions(value, ranges)) {
            error = L"Value must contain valid session ranges in HH:mm-HH:mm format.";
            return false;
        }
        break;
    }
    case SymbolFieldValueKind::Text:
        break;
    }

    if (!needsNumber) return true;
    if (!hasNumericValue && !parseConstraintNumber(value, number)) {
        error = L"Value must be a finite number for the configured constraint.";
        return false;
    }
    if (constraint.hasMinimum && number < constraint.minimum) {
        error = L"Value is below the configured minimum " + numberText(constraint.minimum) + L".";
        return false;
    }
    if (constraint.hasMaximum && number > constraint.maximum) {
        error = L"Value exceeds the configured maximum " + numberText(constraint.maximum) + L".";
        return false;
    }
    if (!constraint.allowedValues.empty()) {
        const bool allowed = std::any_of(constraint.allowedValues.begin(), constraint.allowedValues.end(),
            [number](const SymbolValueRange& range) {
                return number >= range.minimum && number <= range.maximum;
            });
        if (!allowed) {
            error = L"Value is not listed in the configured allowedValues.";
            return false;
        }
    }
    return true;
}

bool IsSymbolFieldEditable(const std::wstring& sourceName) {
    static const std::unordered_set<std::wstring> readOnlyFields = [] {
        std::unordered_set<std::wstring> fields;
        for (const Property& property : makeProperties()) {
            if (!property.editable || !property.write) fields.insert(property.name);
        }
        return fields;
    }();
    return !readOnlyFields.contains(sourceName);
}

bool LoadSymbolFieldMappings(const std::wstring& path,
                             std::vector<SymbolFieldMapping>& mappings,
                             bool& created,
                             std::wstring& error) {
    mappings.clear();
    created = false;
    error.clear();

    const std::vector<Property> availableProperties = makeProperties();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        const DWORD attributesError = GetLastError();
        if (attributes != INVALID_FILE_ATTRIBUTES ||
            (attributesError != ERROR_FILE_NOT_FOUND && attributesError != ERROR_PATH_NOT_FOUND)) {
            error = L"Cannot read symbol field configuration: " + path;
            return false;
        }
        if (!writeDefaultSymbolMappings(path, availableProperties, error)) return false;
        created = true;
        input.clear();
        input.open(path, std::ios::binary);
        if (!input) {
            error = L"Cannot read symbol field configuration: " + path;
            return false;
        }
    }

    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() > 1024 * 1024) {
        error = L"Symbol field configuration is larger than 1 MiB: " + path;
        return false;
    }
    const std::wstring content = decodeUtf8(bytes);
    if (!bytes.empty() && content.empty()) {
        error = L"Symbol field configuration is not valid UTF-8: " + path;
        return false;
    }

    std::unordered_set<std::wstring> seenSourceNames;
    std::unordered_set<std::wstring> seenMapSections;
    std::unordered_map<std::wstring,
        std::vector<std::pair<std::wstring, std::wstring>>> parsedValueMappings;
    std::unordered_map<std::wstring,
        std::vector<std::pair<std::wstring, std::wstring>>> parsedFlagMappings;
    std::unordered_map<std::wstring,
        std::vector<std::pair<std::wstring, std::wstring>>> parsedTextMappings;
    std::unordered_map<std::wstring, SymbolValueConstraint> parsedConstraints;
    enum class MappingKind { None, Value, Flag, Text, Constraints };
    std::wstring activeMapSource;
    MappingKind activeMapKind = MappingKind::None;
    bool seenConstraintsSection = false;
    bool dividerPending = false;
    std::size_t lineNumber = 0;
    std::size_t start = 0;
    while (start <= content.size()) {
        ++lineNumber;
        const std::size_t end = content.find(L'\n', start);
        const std::wstring line = trim(content.substr(start,
            end == std::wstring::npos ? std::wstring::npos : end - start));
        if (line.empty()) {
            if (activeMapKind == MappingKind::None && !mappings.empty()) dividerPending = true;
        } else if (line.front() == L'#' || line.front() == L';') {
            // Comments are accepted in both the column mapping list and map sections.
        } else if (line.front() == L'[' && line.back() == L']') {
            const std::wstring sectionName = trim(line.substr(1, line.size() - 2));
            if (sectionName == L"Constraints") {
                if (seenConstraintsSection) {
                    error = L"Duplicate [Constraints] section in " + path;
                    return false;
                }
                seenConstraintsSection = true;
                activeMapSource.clear();
                activeMapKind = MappingKind::Constraints;
            } else {
            constexpr std::wstring_view valueMapPrefix = L"ValueMap.";
            constexpr std::wstring_view flagMapPrefix = L"FlagMap.";
            constexpr std::wstring_view textMapPrefix = L"TextMap.";
            MappingKind mapKind = MappingKind::None;
            std::wstring_view activePrefix;
            if (sectionName.rfind(valueMapPrefix, 0) == 0) {
                mapKind = MappingKind::Value;
                activePrefix = valueMapPrefix;
            } else if (sectionName.rfind(flagMapPrefix, 0) == 0) {
                mapKind = MappingKind::Flag;
                activePrefix = flagMapPrefix;
            } else if (sectionName.rfind(textMapPrefix, 0) == 0) {
                mapKind = MappingKind::Text;
                activePrefix = textMapPrefix;
            }
            if (mapKind == MappingKind::None ||
                sectionName.size() <= activePrefix.size() ||
                sectionName.rfind(activePrefix, 0) != 0) {
                error = L"Unknown configuration section on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }

            const std::wstring requestedField = trim(
                sectionName.substr(activePrefix.size()));
            std::wstring sourceName;
            const auto knownSource = std::find_if(availableProperties.begin(), availableProperties.end(),
                [&requestedField](const Property& property) {
                    return property.name == requestedField;
                });
            if (knownSource != availableProperties.end()) {
                sourceName = requestedField;
            } else {
                const auto heading = std::find_if(mappings.begin(), mappings.end(),
                    [&requestedField](const SymbolFieldMapping& mapping) {
                        return mapping.displayName == requestedField;
                    });
                if (heading != mappings.end()) sourceName = heading->sourceName;
            }
            if (sourceName.empty()) {
                error = L"Unknown map field '" + requestedField + L"' on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }
            if (!seenMapSections.insert(sourceName).second) {
                error = L"Duplicate map section for '" + sourceName + L"' in " + path;
                return false;
            }
            activeMapSource = std::move(sourceName);
            activeMapKind = mapKind;
            }
        } else if (activeMapKind == MappingKind::Constraints) {
            const std::size_t equals = line.find(L'=');
            if (equals == std::wstring::npos) {
                error = L"Expected SourceField=minimum|maximum|maxChars|allowedValues on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }
            const std::wstring sourceName = trim(line.substr(0, equals));
            const auto knownField = std::find_if(availableProperties.begin(), availableProperties.end(),
                [&sourceName](const Property& property) { return property.name == sourceName; });
            if (knownField == availableProperties.end()) {
                error = L"Unknown constrained field '" + sourceName + L"' on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }
            SymbolValueConstraint constraint;
            if (!parseSymbolValueConstraint(std::wstring_view(line).substr(equals + 1), constraint)) {
                error = L"Invalid value constraint for '" + sourceName + L"' on line " +
                    std::to_wstring(lineNumber) + L" in " + path +
                    L". Expected minimum|maximum|maxChars|allowedValues.";
                return false;
            }
            if (!parsedConstraints.emplace(sourceName, std::move(constraint)).second) {
                error = L"Duplicate constraint for '" + sourceName + L"' on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }
        } else if (activeMapKind != MappingKind::None) {
            const std::size_t equals = line.find(L'=');
            if (equals == std::wstring::npos) {
                error = L"Expected code=display label on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }
            std::wstring code;
            const std::wstring codeText = trim(line.substr(0, equals));
            const std::wstring label = trim(line.substr(equals + 1));
            const bool validCode = activeMapKind == MappingKind::Text
                ? canonicalAlpha3Code(codeText, code)
                : canonicalValueCode(codeText, code);
            if (!validCode || label.empty()) {
                error = activeMapKind == MappingKind::Text
                    ? L"Text-map entries require a three-letter ASCII code and non-empty label on line "
                    : L"Value and flag map entries require a non-negative integer code and non-empty label on line ";
                error += std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }
            auto& entries = activeMapKind == MappingKind::Flag
                ? parsedFlagMappings[activeMapSource]
                : activeMapKind == MappingKind::Text
                    ? parsedTextMappings[activeMapSource]
                    : parsedValueMappings[activeMapSource];
            const bool duplicateCode = std::any_of(entries.begin(), entries.end(),
                [&code](const auto& entry) { return entry.first == code; });
            const bool duplicateLabel = std::any_of(entries.begin(), entries.end(),
                [activeMapKind, &label](const auto& entry) {
                    return activeMapKind == MappingKind::Text
                        ? equalsIgnoreCase(entry.second, label)
                        : entry.second == label;
                });
            if (duplicateCode || duplicateLabel) {
                error = L"Duplicate code or label in map section for '" +
                    activeMapSource + L"' on line " + std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }
            entries.emplace_back(std::move(code), label);
        } else {
            const std::size_t equals = line.find(L'=');
            if (equals == std::wstring::npos) {
                error = L"Expected SourceField=ColumnHeading on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }

            SymbolFieldMapping mapping;
            mapping.sourceName = trim(line.substr(0, equals));
            mapping.displayName = trim(line.substr(equals + 1));
            mapping.dividerBefore = !mappings.empty() && dividerPending;
            if (mapping.sourceName.empty() || mapping.displayName.empty()) {
                error = L"Source field and column heading must not be empty on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }
            if (!seenSourceNames.insert(mapping.sourceName).second) {
                error = L"Duplicate source field '" + mapping.sourceName + L"' on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }
            const auto knownField = std::find_if(availableProperties.begin(), availableProperties.end(),
                [&mapping](const Property& property) { return property.name == mapping.sourceName; });
            if (knownField == availableProperties.end()) {
                error = L"Unknown symbol field '" + mapping.sourceName + L"' on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }
            mappings.push_back(std::move(mapping));
            dividerPending = false;
        }

        if (end == std::wstring::npos) break;
        start = end + 1;
    }

    if (mappings.empty()) {
        error = L"No symbol fields are configured in " + path;
        return false;
    }
    const auto ensureProtectedField = [&mappings](const std::wstring& sourceName,
                                                   const std::wstring& displayName) {
        const bool present = std::any_of(mappings.begin(), mappings.end(),
            [&sourceName](const SymbolFieldMapping& mapping) {
                return mapping.sourceName == sourceName;
            });
        if (!present) mappings.push_back({sourceName, displayName});
    };
    ensureProtectedField(L"Symbol", L"Common.Symbol");
    ensureProtectedField(L"Path", L"Common.Path");
    for (SymbolFieldMapping& mapping : mappings) {
        const auto property = std::find_if(availableProperties.begin(), availableProperties.end(),
            [&mapping](const Property& candidate) { return candidate.name == mapping.sourceName; });
        if (property != availableProperties.end()) mapping.valueKind = property->valueKind;
        const auto valueMap = parsedValueMappings.find(mapping.sourceName);
        if (valueMap != parsedValueMappings.end()) {
            mapping.valueMappings = valueMap->second;
        }
        const auto flagMap = parsedFlagMappings.find(mapping.sourceName);
        if (flagMap != parsedFlagMappings.end()) {
            mapping.flagMappings = flagMap->second;
        }
        const auto textMap = parsedTextMappings.find(mapping.sourceName);
        if (textMap != parsedTextMappings.end()) {
            mapping.textMappings = textMap->second;
        }
        const auto constraint = parsedConstraints.find(mapping.sourceName);
        if (constraint != parsedConstraints.end()) {
            mapping.constraint = constraint->second;
        }
    }
    return true;
}

bool LoadSymbolSnapshot(const std::wstring& server,
                        const std::wstring& loginText,
                        const std::wstring& password,
                        const std::vector<SymbolFieldMapping>& fieldMappings,
                        SymbolSnapshot& snapshot,
                        std::wstring& error) {
    error.clear();
    snapshot = {};

    std::uint64_t login = 0;
    if (!parseLogin(loginText, login)) {
        error = L"Login must contain only digits and be greater than zero.";
        return false;
    }
    if (server.empty() || password.empty()) {
        error = L"Server and password must not be empty.";
        return false;
    }

    ManagerConnection connection;
    if (!connection.connect(server, login, password, error)) return false;
    IMTAdminAPI* admin = connection.admin();

    IMTConSymbol* symbol = admin->SymbolCreate();
    IMTConSymbolSession* session = admin->SymbolSessionCreate();
    if (!symbol || !session) {
        if (symbol) symbol->Release();
        if (session) session->Release();
        error = L"MT5 API could not create symbol data objects.";
        return false;
    }

    const std::vector<Property> availableProperties = makeProperties();
    const std::vector<Property> properties = applyFieldMappings(fieldMappings, availableProperties);
    snapshot.columns.reserve(properties.size());
    snapshot.sourceNames.reserve(properties.size());
    for (std::size_t index = 0; index < properties.size(); ++index) {
        snapshot.columns.push_back(properties[index].name);
        snapshot.sourceNames.push_back(fieldMappings[index].sourceName);
    }

    const std::uint32_t total = admin->SymbolTotal();
    snapshot.rows.reserve(total);
    for (std::uint32_t pos = 0; pos < total; ++pos) {
        const MTAPIRES result = admin->SymbolNext(pos, symbol);
        if (result != MT_RET_OK) {
            ++snapshot.failedSymbols;
            continue;
        }

        std::vector<std::wstring> row;
        row.reserve(properties.size());
        for (std::size_t column = 0; column < properties.size(); ++column) {
            row.push_back(properties[column].read(*symbol, session));
        }
        snapshot.rows.push_back(std::move(row));
    }

    symbol->Release();
    session->Release();
    return true;
}

bool ApplyImportedSymbolSnapshot(const std::wstring& server,
                                 const std::wstring& loginText,
                                 const std::wstring& password,
                                 const std::vector<SymbolFieldMapping>& fieldMappings,
                                 const SymbolSnapshot& importedSnapshot,
                                 const std::wstring& backupDirectory,
                                 const ApplyCheckpoint& checkpoint,
                                 ApplyResult& result,
                                 std::wstring& error) {
    result = {};
    error.clear();
    if (server.empty() || password.empty() || fieldMappings.empty() ||
        importedSnapshot.rows.empty() || importedSnapshot.columns.size() != fieldMappings.size()) {
        error = L"The imported workbook or selected server profile is incomplete.";
        return false;
    }

    std::size_t symbolColumn = fieldMappings.size();
    for (std::size_t index = 0; index < fieldMappings.size(); ++index) {
        if (fieldMappings[index].sourceName == L"Symbol") symbolColumn = index;
    }
    if (symbolColumn == fieldMappings.size()) {
        error = L"symbols.ini must include the Symbol field before a workbook can be applied.";
        return false;
    }

    std::uint64_t login = 0;
    if (!parseLogin(loginText, login)) {
        error = L"Login must contain only digits and be greater than zero.";
        return false;
    }

    std::unordered_set<std::wstring> requestedNames;
    requestedNames.reserve(importedSnapshot.rows.size());
    for (const auto& row : importedSnapshot.rows) {
        if (row.size() != fieldMappings.size() || row[symbolColumn].empty()) {
            error = L"Every imported row must contain a Symbol and all configured columns.";
            return false;
        }
        if (!requestedNames.insert(row[symbolColumn]).second) {
            error = L"The workbook contains the duplicate symbol '" + row[symbolColumn] + L"'.";
            return false;
        }
        for (std::size_t column = 0; column < fieldMappings.size(); ++column) {
            std::wstring validationError;
            const std::wstring storageValue = SymbolValueForStorage(fieldMappings[column], row[column]);
            if (!ValidateSymbolFieldValue(fieldMappings[column], storageValue, validationError)) {
                error = L"Symbol '" + row[symbolColumn] + L"', field '" +
                    fieldMappings[column].sourceName + L"': " + validationError +
                    L" No values were applied.";
                return false;
            }
        }
    }

    ManagerConnection connection;
    if (!connection.connect(server, login, password, error)) return false;
    IMTAdminAPI* admin = connection.admin();
    IMTConSymbol* symbol = admin->SymbolCreate();
    IMTConSymbolSession* session = admin->SymbolSessionCreate();
    if (!symbol || !session) {
        if (symbol) symbol->Release();
        if (session) session->Release();
        error = L"MT5 API could not create symbol data objects for import.";
        return false;
    }

    const std::vector<Property> availableProperties = makeProperties();
    const std::vector<Property> properties = applyFieldMappings(fieldMappings, availableProperties);
    const std::vector<Property>& backupProperties = availableProperties;
    SymbolSnapshot backup;
    backup.columns.reserve(backupProperties.size());
    backup.sourceNames.reserve(backupProperties.size());
    for (const Property& property : backupProperties) {
        backup.columns.push_back(property.name);
        backup.sourceNames.push_back(property.name);
    }

    std::unordered_map<std::wstring, IMTConSymbol*> originalConfigs;
    originalConfigs.reserve(requestedNames.size());
    const std::uint32_t serverSymbolTotal = admin->SymbolTotal();
    backup.rows.reserve(serverSymbolTotal);
    bool cloneFailure = false;
    for (std::uint32_t position = 0; position < serverSymbolTotal; ++position) {
        const MTAPIRES nextResult = admin->SymbolNext(position, symbol);
        if (nextResult != MT_RET_OK) {
            ++backup.failedSymbols;
            continue;
        }
        const std::wstring name = cleanText(symbol->Symbol());
        std::vector<std::wstring> row;
        row.reserve(backupProperties.size());
        for (const Property& property : backupProperties) {
            std::wstring value = property.read(*symbol, session);
            if ((property.name.rfind(L"QuoteSessions.", 0) == 0 ||
                 property.name.rfind(L"TradeSessions.", 0) == 0) &&
                value.find(L"[MTAPIRES=") != std::wstring::npos) ++backup.failedSymbols;
            row.push_back(std::move(value));
        }
        backup.rows.push_back(std::move(row));

        if (requestedNames.contains(name)) {
            IMTConSymbol* clone = admin->SymbolCreate();
            if (!clone || clone->Assign(symbol) != MT_RET_OK) {
                if (clone) clone->Release();
                cloneFailure = true;
                break;
            }
            originalConfigs.emplace(name, clone);
        }
    }
    if (cloneFailure || backup.failedSymbols != 0) {
        releaseClones(originalConfigs);
        symbol->Release();
        session->Release();
        error = cloneFailure
            ? L"Cannot capture a complete in-memory rollback copy of the current symbols. No changes were applied."
            : L"Could not retrieve every current symbol field to create a complete backup. No changes were applied.";
        return false;
    }
    if (backup.rows.empty()) {
        releaseClones(originalConfigs);
        symbol->Release();
        session->Release();
        error = L"The connected server returned no symbol records. No changes were applied.";
        return false;
    }

    std::error_code fsError;
    std::filesystem::create_directories(std::filesystem::path(backupDirectory), fsError);
    if (fsError || !std::filesystem::is_directory(std::filesystem::path(backupDirectory), fsError)) {
        releaseClones(originalConfigs);
        symbol->Release();
        session->Release();
        error = L"Cannot create the backup directory: " + backupDirectory;
        return false;
    }
    result.backupPath = backupFilePath(backupDirectory);
    if (!saveSnapshotCsv(backup, result.backupPath, error)) {
        releaseClones(originalConfigs);
        symbol->Release();
        session->Release();
        return false;
    }

    std::vector<std::wstring> attempted;
    attempted.reserve(importedSnapshot.rows.size());
    for (std::size_t rowIndex = 0; rowIndex < importedSnapshot.rows.size(); ++rowIndex) {
        const std::wstring& name = importedSnapshot.rows[rowIndex][symbolColumn];
        if (checkpoint) {
            const ApplyChoice choice = checkpoint({rowIndex, importedSnapshot.rows.size(), false, name});
            if (choice != ApplyChoice::Continue) {
                result.aborted = true;
                result.restoreRequested = choice == ApplyChoice::StopAndRestore;
                result.notProcessed = importedSnapshot.rows.size() - rowIndex;
                break;
            }
        }

        const auto original = originalConfigs.find(name);
        if (original == originalConfigs.end()) {
            ++result.skipped;
            continue;
        }
        const MTAPIRES getResult = admin->SymbolGet(name.c_str(), symbol);
        if (getResult != MT_RET_OK) {
            ++result.failed;
            if (result.firstFailure.empty()) {
                result.firstFailure = L"Could not retrieve symbol '" + name + L"': " + apiError(L"SymbolGet", getResult);
            }
            continue;
        }

        bool changed = false;
        bool rowValid = true;
        std::wstring rowError;
        std::vector<std::size_t> changedColumns;
        changedColumns.reserve(fieldMappings.size());
        std::vector<std::wstring> originalValues(fieldMappings.size());
        for (std::size_t column = 0; column < fieldMappings.size(); ++column) {
            if (fieldMappings[column].sourceName == L"Symbol" ||
                fieldMappings[column].sourceName == L"Path") {
                continue;
            }
            originalValues[column] = properties[column].read(*symbol, session);
        }

        for (std::size_t column = 0; column < fieldMappings.size(); ++column) {
            if (fieldMappings[column].sourceName == L"Symbol") {
                if (importedSnapshot.rows[rowIndex][column] != cleanText(symbol->Symbol())) {
                    rowError = L"The Symbol value does not match the server symbol '" + name + L"'.";
                    rowValid = false;
                    break;
                }
                continue;
            }
            if (fieldMappings[column].sourceName == L"Path") continue;
            if (!properties[column].editable || !properties[column].write) continue;
            const std::wstring& importedValue = importedSnapshot.rows[rowIndex][column];
            if (importedValue == originalValues[column]) continue;

            changedColumns.push_back(column);

            bool fieldChanged = false;
            std::wstring fieldError;
            const std::wstring serverValue = serverValueForField(fieldMappings[column], importedValue);
            if (!properties[column].write(*symbol, serverValue, session, fieldChanged, fieldError)) {
                rowError = L"Symbol '" + name + L"', field '" + fieldMappings[column].sourceName +
                    L"': " + fieldError;
                rowValid = false;
                break;
            }
            if (fieldChanged) {
                changed = true;
            }
        }
        if (!rowValid) {
            ++result.failed;
            if (result.firstFailure.empty()) result.firstFailure = std::move(rowError);
            continue;
        }
        if (!changed) {
            ++result.unchanged;
            continue;
        }

        attempted.push_back(name);
        const MTAPIRES updateResult = admin->SymbolUpdate(symbol);
        if (updateResult != MT_RET_OK) {
            ++result.failed;
            if (result.firstFailure.empty()) {
                result.firstFailure = L"Could not update symbol '" + name + L"': " +
                    apiError(L"SymbolUpdate", updateResult);
            }
            continue;
        }

        const MTAPIRES verifyResult = admin->SymbolGet(name.c_str(), symbol);
        if (verifyResult != MT_RET_OK) {
            ++result.failed;
            if (result.firstFailure.empty()) {
                result.firstFailure = L"Symbol '" + name +
                    L"' was submitted, but the saved values could not be verified: " +
                    apiError(L"SymbolGet", verifyResult);
            }
            continue;
        }

        bool verified = true;
        for (const std::size_t column : changedColumns) {
            const std::wstring rawActualValue = properties[column].read(*symbol, session);
            const std::wstring actualValue = SymbolValueForDisplay(fieldMappings[column], rawActualValue);
            const std::wstring serverValue = serverValueForField(
                fieldMappings[column], importedSnapshot.rows[rowIndex][column]);
            bool wouldChange = false;
            std::wstring verifyError;
            if (!properties[column].write(*symbol, serverValue,
                                          session, wouldChange, verifyError) || wouldChange) {
                verified = false;
                if (result.firstFailure.empty()) {
                    result.firstFailure = L"Symbol '" + name + L"', field '" +
                        fieldMappings[column].sourceName + L"' was not saved. Expected '" +
                        SymbolValueForDisplay(fieldMappings[column], serverValue) +
                        L"', server returned '" +
                        actualValue + L"'.";
                }
                break;
            }
        }
        if (verified) ++result.applied;
        else ++result.failed;
    }

    if (!result.aborted && checkpoint) {
        const ApplyChoice choice = checkpoint({importedSnapshot.rows.size(),
            importedSnapshot.rows.size(), false, L""});
        if (choice != ApplyChoice::Continue) {
            result.aborted = true;
            result.restoreRequested = choice == ApplyChoice::StopAndRestore;
        }
    }

    if (result.restoreRequested && !attempted.empty()) {
        for (std::size_t offset = 0; offset < attempted.size(); ++offset) {
            const std::wstring& name = attempted[attempted.size() - 1 - offset];
            if (checkpoint) checkpoint({offset, attempted.size(), true, name});
            const auto original = originalConfigs.find(name);
            if (original == originalConfigs.end() ||
                admin->SymbolGet(name.c_str(), symbol) != MT_RET_OK ||
                symbol->Assign(original->second) != MT_RET_OK) {
                ++result.restoreFailed;
                if (result.firstFailure.empty()) {
                    result.firstFailure = L"Could not restore symbol '" + name + L"'.";
                }
                continue;
            }
            const MTAPIRES restoreResult = admin->SymbolUpdate(symbol);
            if (restoreResult != MT_RET_OK) {
                ++result.restoreFailed;
                if (result.firstFailure.empty()) {
                    result.firstFailure = L"Could not restore symbol '" + name + L"': " +
                        apiError(L"SymbolUpdate", restoreResult);
                }
                continue;
            }

            const MTAPIRES verifyRestoreResult = admin->SymbolGet(name.c_str(), symbol);
            if (verifyRestoreResult != MT_RET_OK) {
                ++result.restoreFailed;
                if (result.firstFailure.empty()) {
                    result.firstFailure = L"Symbol '" + name +
                        L"' was restored, but the restored values could not be verified: " +
                        apiError(L"SymbolGet", verifyRestoreResult);
                }
                continue;
            }

            bool restored = true;
            for (const Property& property : backupProperties) {
                if (!property.editable || !property.write) continue;

                const std::wstring expectedValue = property.read(*original->second, session);
                const std::wstring actualValue = property.read(*symbol, session);
                bool wouldChange = false;
                std::wstring verifyError;
                if (!property.write(*symbol, expectedValue, session, wouldChange, verifyError) ||
                    wouldChange) {
                    restored = false;
                    if (result.firstFailure.empty()) {
                        result.firstFailure = L"Symbol '" + name + L"', field '" +
                            property.name + L"' was not restored. Expected '" + expectedValue +
                            L"', server returned '" + actualValue + L"'.";
                        if (!verifyError.empty()) {
                            result.firstFailure += L" " + verifyError;
                        }
                    }
                    break;
                }
            }
            if (restored) ++result.restored;
            else ++result.restoreFailed;
        }
        if (checkpoint) checkpoint({attempted.size(), attempted.size(), true, L""});
    }
    releaseClones(originalConfigs);
    symbol->Release();
    session->Release();
    return true;
}
