#pragma once

#include <string>

#include "SymbolSnapshot.h"

bool ImportSnapshotFromXlsx(const std::wstring& filePath,
                            SymbolSnapshot& snapshot,
                            std::wstring& error);
