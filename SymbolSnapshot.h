#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SymbolSnapshot {
    std::vector<std::wstring> columns;
    std::vector<std::wstring> sourceNames;
    std::vector<std::vector<std::wstring>> rows;
    std::uint32_t failedSymbols = 0;
};

struct SymbolFieldMapping {
    std::wstring sourceName;
    std::wstring displayName;
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

bool LoadSymbolSnapshot(const std::wstring& server,
                        const std::wstring& loginText,
                        const std::wstring& password,
                        const std::vector<SymbolFieldMapping>& fieldMappings,
                        SymbolSnapshot& snapshot,
                        std::wstring& error);

bool ApplyImportedSymbolSnapshot(const std::wstring& server,
                                 const std::wstring& loginText,
                                 const std::wstring& password,
                                 const std::vector<SymbolFieldMapping>& fieldMappings,
                                 const SymbolSnapshot& importedSnapshot,
                                 const std::wstring& backupDirectory,
                                 const ApplyCheckpoint& checkpoint,
                                 ApplyResult& result,
                                 std::wstring& error);
