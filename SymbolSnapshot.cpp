#include "SymbolSnapshot.h"

#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4100)
#include <MT5APIManager.h>
#pragma warning(pop)

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cwchar>
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

std::wstring colorText(COLORREF color) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"#%02X%02X%02X (0x%08X)",
               GetRValue(color), GetGValue(color), GetBValue(color), color);
    return buffer;
}

std::wstring minuteTime(std::uint32_t minutes) {
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
};

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
    if (hour > 24 || mins > 59 || (hour == 24 && mins != 0)) return false;
    minute = hour * 60 + mins;
    return true;
}

using SessionRange = std::pair<std::uint32_t, std::uint32_t>;

bool parseSessions(std::wstring_view text, std::vector<SessionRange>& ranges) {
    ranges.clear();
    const std::wstring value = trim(std::wstring(text));
    const std::size_t colon = value.find(L':');
    if (colon == std::wstring::npos) return false;
    std::uint32_t expectedCount = 0;
    if (!parseFieldNumber(std::wstring_view(value).substr(0, colon), expectedCount)) return false;
    std::size_t start = colon + 1;
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
        ranges.emplace_back(open, close);
        start = rangeEnd == value.size() ? rangeEnd : rangeEnd + 1;
    }
    return ranges.size() == expectedCount;
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
              "# Remove a line to omit that field. Keep each source field name unchanged.\n";
    for (const Property& property : properties) {
        const std::string source = encodeUtf8(property.name);
        if (source.empty()) {
            error = L"Cannot encode a symbol field name in UTF-8.";
            return false;
        }
        output << source << '=' << source << '\n';
    }
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
        }})
#define ADD_TEXT_FIELD(name) \
    properties.push_back({WIDEN(STRINGIFY(name)), \
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { \
            return cleanText(symbol.name()); \
        }, \
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*, \
           bool& changed, std::wstring& error) { \
            return writeText(text, [&]() { return symbol.name(); }, \
                [&](LPCWSTR value) { return symbol.name(value); }, changed, error); \
        }})
#define ADD_READONLY_FIELD(name) \
    properties.push_back({WIDEN(STRINGIFY(name)), \
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { \
            return numberText(symbol.name()); \
        }, \
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*, \
           bool&, std::wstring& error) { \
            return validateReadOnlyNumber(text, [&]() { return symbol.name(); }, \
                WIDEN(STRINGIFY(name)), error); \
        }, false})
#define ADD_COLOR_FIELD(name) \
    properties.push_back({WIDEN(STRINGIFY(name)), \
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { return colorText(symbol.name()); }, \
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*, \
           bool& changed, std::wstring& error) { \
            return writeColor(text, [&]() { return symbol.name(); }, \
                [&](COLORREF value) { return symbol.name(value); }, changed, error); \
        }})

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
    ADD_TEXT_FIELD(CurrencyProfit);
    properties.push_back({L"CurrencyProfitDigits",
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { return numberText(symbol.CurrencyProfitDigits()); },
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*,
           bool& changed, std::wstring& error) {
            return writeNumber(text, [&]() { return symbol.CurrencyProfitDigits(); },
                [&](auto value) { return symbol.CurrencyProfitDigitsSet(value); }, changed, error);
        }});
    ADD_TEXT_FIELD(CurrencyMargin);
    properties.push_back({L"CurrencyMarginDigits",
        [](const IMTConSymbol& symbol, IMTConSymbolSession*) { return numberText(symbol.CurrencyMarginDigits()); },
        [](IMTConSymbol& symbol, std::wstring_view text, IMTConSymbolSession*,
           bool& changed, std::wstring& error) {
            return writeNumber(text, [&]() { return symbol.CurrencyMarginDigits(); },
                [&](auto value) { return symbol.CurrencyMarginDigitsSet(value); }, changed, error);
        }});
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
    ADD_FIELD(VolumeMin);
    ADD_FIELD(VolumeMax);
    ADD_FIELD(VolumeStep);
    ADD_FIELD(VolumeLimit);
    ADD_FIELD(MarginFlags);
    ADD_FIELD(MarginInitial);
    ADD_FIELD(MarginMaintenance);
    ADD_FIELD(MarginLong);
    ADD_FIELD(MarginShort);
    ADD_FIELD(MarginLimit);
    ADD_FIELD(MarginStop);
    ADD_FIELD(MarginStopLimit);
    ADD_FIELD(SwapMode);
    ADD_FIELD(SwapLong);
    ADD_FIELD(SwapShort);
    ADD_FIELD(Swap3Day);
    ADD_FIELD(TimeStart);
    ADD_FIELD(TimeExpiration);
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
            return numberText(s.MarginRateInitial(type));
        }, [type](IMTConSymbol& s, std::wstring_view text, IMTConSymbolSession*,
                  bool& changed, std::wstring& error) {
            return writeNumber(text, [&]() { return s.MarginRateInitial(type); },
                [&](auto value) { return s.MarginRateInitial(type, value); }, changed, error);
        }});
        const std::wstring maintenanceName = std::wstring(L"MarginRateMaintenance.") + marginRateNames[type];
        properties.push_back({maintenanceName, [type](const IMTConSymbol& s, IMTConSymbolSession*) {
            return numberText(s.MarginRateMaintenance(type));
        }, [type](IMTConSymbol& s, std::wstring_view text, IMTConSymbolSession*,
                  bool& changed, std::wstring& error) {
            return writeNumber(text, [&]() { return s.MarginRateMaintenance(type); },
                [&](auto value) { return s.MarginRateMaintenance(type, value); }, changed, error);
        }});
    }

    ADD_FIELD(OptionsMode);
    ADD_FIELD(PriceStrike);
    ADD_FIELD(MarginRateLiquidity);
    ADD_FIELD(FaceValue);
    ADD_FIELD(AccruedInterest);
    ADD_FIELD(SpliceType);
    ADD_FIELD(SpliceTimeType);
    ADD_FIELD(SpliceTimeDays);
    ADD_FIELD(MarginHedged);
    ADD_FIELD(MarginRateCurrency);
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
            std::wstring value = std::to_wstring(total) + L": ";
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

        const std::wstring tradeName = std::wstring(L"TradeSessions.") + dayNames[day];
        properties.push_back({tradeName, [day](const IMTConSymbol& s, IMTConSymbolSession* session) {
            const std::uint32_t total = s.SessionTradeTotal(day);
            std::wstring value = std::to_wstring(total) + L": ";
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
    }

#undef ADD_TEXT_FIELD
#undef ADD_COLOR_FIELD
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
    std::size_t lineNumber = 0;
    std::size_t start = 0;
    while (start <= content.size()) {
        ++lineNumber;
        const std::size_t end = content.find(L'\n', start);
        const std::wstring line = trim(content.substr(start,
            end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!line.empty() && line.front() != L'#' && line.front() != L';') {
            const std::size_t equals = line.find(L'=');
            if (equals == std::wstring::npos) {
                error = L"Expected SourceField=ColumnHeading on line " +
                    std::to_wstring(lineNumber) + L" in " + path;
                return false;
            }

            SymbolFieldMapping mapping;
            mapping.sourceName = trim(line.substr(0, equals));
            mapping.displayName = trim(line.substr(equals + 1));
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
        for (const Property& property : properties) {
            row.push_back(property.read(*symbol, session));
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
            const std::wstring& importedValue = importedSnapshot.rows[rowIndex][column];
            if (importedValue == properties[column].read(*symbol, session)) continue;

            std::wstring fieldError;
            if (!properties[column].write(*symbol, importedValue, session, changed, fieldError)) {
                rowError = L"Symbol '" + name + L"', field '" + fieldMappings[column].sourceName +
                    L"': " + fieldError;
                rowValid = false;
                break;
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
        if (updateResult == MT_RET_OK) ++result.applied;
        else {
            ++result.failed;
            if (result.firstFailure.empty()) {
                result.firstFailure = L"Could not update symbol '" + name + L"': " +
                    apiError(L"SymbolUpdate", updateResult);
            }
        }
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
            if (restoreResult == MT_RET_OK) ++result.restored;
            else {
                ++result.restoreFailed;
                if (result.firstFailure.empty()) {
                    result.firstFailure = L"Could not restore symbol '" + name + L"': " +
                        apiError(L"SymbolUpdate", restoreResult);
                }
            }
        }
        if (checkpoint) checkpoint({attempted.size(), attempted.size(), true, L""});
    }
    releaseClones(originalConfigs);
    symbol->Release();
    session->Release();
    return true;
}
