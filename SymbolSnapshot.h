#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct SymbolSnapshot {
    std::vector<std::wstring> columns;
    std::vector<std::wstring> sourceNames;
    std::vector<std::vector<std::wstring>> rows;
    std::uint32_t failedSymbols = 0;
};

enum class SymbolFieldValueKind {
    Text,
    SignedInteger32,
    UnsignedInteger32,
    SignedInteger64,
    UnsignedInteger64,
    FloatingPoint,
    VolumeLots,
    Color,
    Timestamp,
    Session
};

struct SymbolValueRange {
    double minimum = 0;
    double maximum = 0;
};

struct SymbolValueConstraint {
    bool hasMinimum = false;
    double minimum = 0;
    bool hasMaximum = false;
    double maximum = 0;
    bool hasMaxChars = false;
    std::size_t maxChars = 0;
    std::vector<SymbolValueRange> allowedValues;
};

struct SymbolFieldMapping {
    std::wstring sourceName;
    std::wstring displayName;
    bool dividerBefore = false;
    // Optional SDK value-code to user-facing label mappings for enum-like fields.
    std::vector<std::pair<std::wstring, std::wstring>> valueMappings;
    // Optional bit value to label mappings for SDK flag fields.
    std::vector<std::pair<std::wstring, std::wstring>> flagMappings;
    // Optional text-code to label mappings, such as ISO 3166-1 alpha-3 country codes.
    std::vector<std::pair<std::wstring, std::wstring>> textMappings;
    SymbolFieldValueKind valueKind = SymbolFieldValueKind::Text;
    SymbolValueConstraint constraint;
};

enum class ApplyChoice {
    Continue,
    StopKeepChanges,
    StopAndRestore
};

struct ApplyProgress {
    std::size_t completed = 0;
    std::size_t total = 0;
    bool restoring = false;
    std::wstring symbol;
};

struct ApplyResult {
    std::size_t applied = 0;
    std::size_t unchanged = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    std::size_t notProcessed = 0;
    std::size_t restored = 0;
    std::size_t restoreFailed = 0;
    bool aborted = false;
    bool restoreRequested = false;
    std::wstring backupPath;
    std::wstring firstFailure;
};

using ApplyCheckpoint = std::function<ApplyChoice(const ApplyProgress&)>;

bool LoadSymbolFieldMappings(const std::wstring& path,
                             std::vector<SymbolFieldMapping>& mappings,
                             bool& created,
                             std::wstring& error);

std::wstring SymbolValueForDisplay(const SymbolFieldMapping& mapping,
                                   const std::wstring& value);
std::wstring SymbolValueForStorage(const SymbolFieldMapping& mapping,
                                   const std::wstring& value);
bool ValidateSymbolFieldValue(const SymbolFieldMapping& mapping,
                              std::wstring_view value,
                              std::wstring& error);

bool LoadSymbolSnapshot(const std::wstring& server,
                        const std::wstring& loginText,
                        const std::wstring& password,
                        const std::vector<SymbolFieldMapping>& fieldMappings,
                        SymbolSnapshot& snapshot,
                        std::wstring& error);

bool IsSymbolFieldEditable(const std::wstring& sourceName);

bool ApplyImportedSymbolSnapshot(const std::wstring& server,
                                 const std::wstring& loginText,
                                 const std::wstring& password,
                                 const std::vector<SymbolFieldMapping>& fieldMappings,
                                 const SymbolSnapshot& importedSnapshot,
                                 const std::wstring& backupDirectory,
                                 const ApplyCheckpoint& checkpoint,
                                 ApplyResult& result,
                                 std::wstring& error);
