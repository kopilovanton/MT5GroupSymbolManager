#pragma once

#include <string>

#include "SymbolSnapshot.h"

bool ExportSnapshotToXlsx(const SymbolSnapshot& snapshot,
                          const std::wstring& filePath,
                          std::wstring& error);
