#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <exception>
#include <limits>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Connections.h"
#include "ExcelImport.h"
#include "ExcelExport.h"
#include "SymbolSnapshot.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT WM_APP_FETCH_COMPLETE = WM_APP + 1;
constexpr UINT WM_APP_IMPORT_COMPLETE = WM_APP + 2;
constexpr UINT WM_APP_APPLY_PROGRESS = WM_APP + 3;
constexpr UINT WM_APP_APPLY_PAUSED = WM_APP + 4;
constexpr UINT WM_APP_APPLY_COMPLETE = WM_APP + 5;
constexpr UINT WM_APP_FILTER_COLUMN = WM_APP + 6;
constexpr int IDC_PROFILE = 1001;
constexpr int IDC_LOGIN = 1002;
constexpr int IDC_PASSWORD = 1003;
constexpr int IDC_CONNECT = 1004;
constexpr int IDC_EXPORT = 1005;
constexpr int IDC_IMPORT = 1006;
constexpr int IDC_EXCEL_PATH = 1007;
constexpr int IDC_BROWSE_EXCEL = 1008;
constexpr int IDC_PROFILE_PROMPT_CONNECT = 1101;
constexpr int IDC_PROFILE_PROMPT_CANCEL = 1102;
constexpr int IDC_PROGRESS_BAR = 1201;
constexpr int IDC_PROGRESS_TEXT = 1202;
constexpr int IDC_PROGRESS_ABORT = 1203;
constexpr int IDC_ABORT_CONFIRM = 1301;
constexpr int IDC_ABORT_RESTORE = 1302;
constexpr int IDC_FILTER_SEARCH = 1401;
constexpr int IDC_FILTER_VALUES = 1402;
constexpr int IDC_FILTER_SELECT_VISIBLE = 1403;
constexpr int IDC_FILTER_CLEAR = 1404;
constexpr int IDC_FILTER_APPLY = 1405;
constexpr int IDC_FILTER_CANCEL = 1406;
constexpr int IDC_FILTER_CLEAR_SELECTION = 1407;
constexpr wchar_t PROFILE_PROMPT_CLASS[] = L"MT5SymbolManagerProfilePrompt";
constexpr wchar_t APPLY_PROGRESS_CLASS[] = L"MT5SymbolManagerApplyProgress";
constexpr wchar_t ABORT_PROMPT_CLASS[] = L"MT5SymbolManagerAbortPrompt";
constexpr wchar_t COLUMN_FILTER_CLASS[] = L"MT5SymbolManagerColumnFilter";

struct FetchResult {
    bool success = false;
    std::wstring profile;
    std::wstring server;
    std::wstring error;
    SymbolSnapshot snapshot;
};

struct ImportLoadResult {
    bool success = false;
    std::wstring error;
    SymbolSnapshot snapshot;
};

struct ApplyWorkerResult {
    bool success = false;
    std::wstring error;
    ApplyResult result;
};

struct ApplyAsyncState {
    std::mutex mutex;
    bool windowAlive = true;
    std::unique_ptr<ApplyWorkerResult> result;
};

struct ApplyControlState {
    std::mutex mutex;
    std::condition_variable condition;
    bool pauseRequested = false;
    ApplyChoice choice = ApplyChoice::Continue;
};

struct AsyncState {
    std::mutex mutex;
    bool windowAlive = true;
    std::unique_ptr<FetchResult> result;
    std::unique_ptr<ImportLoadResult> importResult;
    std::wstring importPath;
};

HINSTANCE instanceHandle = nullptr;
HWND mainWindow = nullptr;
HWND profileCombo = nullptr;
HWND loginEdit = nullptr;
HWND passwordEdit = nullptr;
HWND connectButton = nullptr;
HWND exportButton = nullptr;
HWND importButton = nullptr;
HWND excelPathEdit = nullptr;
HWND browseExcelButton = nullptr;
HWND statusText = nullptr;
HWND symbolList = nullptr;
HWND applyProgressWindow = nullptr;
HWND applyProgressBar = nullptr;
HWND applyProgressText = nullptr;
HWND applyAbortButton = nullptr;
HFONT controlFont = nullptr;
HFONT statusFont = nullptr;
HFONT tableFont = nullptr;
std::vector<ConnectionProfile> profiles;
std::vector<SymbolFieldMapping> symbolFieldMappings;
bool symbolFieldConfigReady = false;
std::wstring symbolFieldConfigError;
SymbolSnapshot currentSnapshot;
using ChangedCellMap = std::unordered_map<std::wstring, std::unordered_set<std::size_t>>;
ChangedCellMap importedChangedCells;
std::size_t currentSymbolColumn = (std::numeric_limits<std::size_t>::max)();
std::shared_ptr<AsyncState> asyncState = std::make_shared<AsyncState>();
std::shared_ptr<ApplyAsyncState> applyAsyncState = std::make_shared<ApplyAsyncState>();
std::shared_ptr<ApplyControlState> applyControlState = std::make_shared<ApplyControlState>();
std::atomic_bool operationRunning = false;
std::thread fetchWorker;
std::thread importWorker;
std::thread applyWorker;
int activeSortColumn = -1;
bool activeSortAscending = true;
LRESULT activeProfileSelection = CB_ERR;
bool importedDataReady = false;
bool applyFinalizing = false;
std::wstring excelFilePath;
std::wstring backupDirectoryPath;
std::vector<std::wstring> generatedBackupPaths;

void setControlTreeFont(HWND parent, HFONT font);

struct ColumnFilter {
    bool active = false;
    std::unordered_set<std::wstring> selectedValues;
};

struct ColumnFilterPopupState {
    std::size_t column = 0;
    HWND searchEdit = nullptr;
    HWND valuesList = nullptr;
    std::vector<std::wstring> values;
    std::vector<std::size_t> visibleValueIndices;
    std::unordered_set<std::wstring> selectedValues;
    bool apply = false;
    bool clear = false;
};

std::vector<ColumnFilter> columnFilters;
std::vector<std::size_t> visibleRows;

bool parseSortableNumber(std::wstring_view value, double& number) {
    if (value.empty()) return false;
    const std::wstring text(value);
    wchar_t* end = nullptr;
    errno = 0;
    number = std::wcstod(text.c_str(), &end);
    return end != text.c_str() && end == text.c_str() + text.size() &&
        errno != ERANGE && std::isfinite(number);
}

int compareCellValues(std::wstring_view left, std::wstring_view right) {
    if (left.empty()) return right.empty() ? 0 : -1;
    if (right.empty()) return 1;
    double leftNumber = 0.0;
    double rightNumber = 0.0;
    const bool leftIsNumber = parseSortableNumber(left, leftNumber);
    const bool rightIsNumber = parseSortableNumber(right, rightNumber);
    if (leftIsNumber && rightIsNumber) {
        if (leftNumber < rightNumber) return -1;
        if (leftNumber > rightNumber) return 1;
        return 0;
    }
    if (leftIsNumber != rightIsNumber) return leftIsNumber ? -1 : 1;

    const int comparison = CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE);
    if (comparison == CSTR_LESS_THAN) return -1;
    if (comparison == CSTR_GREATER_THAN) return 1;
    return 0;
}

struct ProfilePromptState {
    bool connect = false;
};

LRESULT CALLBACK profilePromptProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ProfilePromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<ProfilePromptState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE:
        CreateWindowW(L"STATIC", L"Connect to the selected server profile?",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 18, 18, 355, 38,
            window, nullptr, instanceHandle, nullptr);
        CreateWindowW(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            190, 75, 88, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROFILE_PROMPT_CONNECT)),
            instanceHandle, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            286, 75, 88, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROFILE_PROMPT_CANCEL)),
            instanceHandle, nullptr);
        setControlTreeFont(window, controlFont);
        return 0;

    case WM_COMMAND:
        if (state && LOWORD(wParam) == IDC_PROFILE_PROMPT_CONNECT) {
            state->connect = true;
            DestroyWindow(window);
            return 0;
        }
        if (state && LOWORD(wParam) == IDC_PROFILE_PROMPT_CANCEL) {
            state->connect = false;
            DestroyWindow(window);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (state) state->connect = false;
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool confirmProfileConnect() {
    ProfilePromptState state;
    const HWND prompt = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        PROFILE_PROMPT_CLASS, L"Change server profile",
        WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 400, 150,
        mainWindow, nullptr, instanceHandle, &state);
    if (!prompt) return false;

    RECT ownerRect{}, promptRect{};
    GetWindowRect(mainWindow, &ownerRect);
    GetWindowRect(prompt, &promptRect);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - (promptRect.right - promptRect.left)) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - (promptRect.bottom - promptRect.top)) / 2;
    SetWindowPos(prompt, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    EnableWindow(mainWindow, FALSE);
    SetFocus(prompt);

    MSG message{};
    while (IsWindow(prompt)) {
        const BOOL gotMessage = GetMessageW(&message, nullptr, 0, 0);
        if (gotMessage <= 0) break;
        if (!IsDialogMessageW(prompt, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(mainWindow, TRUE);
    SetActiveWindow(mainWindow);
    return state.connect;
}

std::wstring getText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

void setStatus(const std::wstring& text) {
    if (statusText) SetWindowTextW(statusText, text.c_str());
}

void setControlTreeFont(HWND parent, HFONT font) {
    if (!parent || !font) return;
    SendMessageW(parent, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    for (HWND child = GetWindow(parent, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        setControlTreeFont(child, font);
    }
}

std::wstring_view rowCellValue(const std::vector<std::wstring>& row, std::size_t column) {
    return column < row.size() ? std::wstring_view(row[column]) : std::wstring_view{};
}

std::size_t findSymbolColumn(const SymbolSnapshot& snapshot) {
    for (std::size_t column = 0; column < snapshot.sourceNames.size(); ++column) {
        if (snapshot.sourceNames[column] == L"Symbol") return column;
    }
    constexpr std::wstring_view suffix = L".Symbol";
    for (std::size_t column = 0; column < snapshot.columns.size(); ++column) {
        const std::wstring& name = snapshot.columns[column];
        if (name == L"Symbol" || (name.size() >= suffix.size() &&
            std::wstring_view(name).substr(name.size() - suffix.size()) == suffix)) {
            return column;
        }
    }
    return (std::numeric_limits<std::size_t>::max)();
}

std::wstring_view snapshotColumnKey(const SymbolSnapshot& snapshot, std::size_t column) {
    if (column < snapshot.sourceNames.size() && !snapshot.sourceNames[column].empty()) {
        return snapshot.sourceNames[column];
    }
    if (column < snapshot.columns.size()) return snapshot.columns[column];
    return {};
}

bool importedValuesEqual(std::wstring_view left, std::wstring_view right) {
    double leftNumber = 0.0;
    double rightNumber = 0.0;
    if (parseSortableNumber(left, leftNumber) && parseSortableNumber(right, rightNumber)) {
        return leftNumber == rightNumber;
    }
    return left == right;
}

ChangedCellMap findImportedChanges(const SymbolSnapshot& before,
                                   const SymbolSnapshot& imported) {
    ChangedCellMap changedCells;
    if (before.rows.empty() || imported.rows.empty()) return changedCells;

    const std::size_t beforeSymbolColumn = findSymbolColumn(before);
    const std::size_t importedSymbolColumn = findSymbolColumn(imported);
    if (beforeSymbolColumn == (std::numeric_limits<std::size_t>::max)() ||
        importedSymbolColumn == (std::numeric_limits<std::size_t>::max)()) {
        return changedCells;
    }

    std::unordered_map<std::wstring, const std::vector<std::wstring>*> previousRows;
    previousRows.reserve(before.rows.size());
    for (const auto& row : before.rows) {
        const std::wstring_view symbol = rowCellValue(row, beforeSymbolColumn);
        if (!symbol.empty()) previousRows.emplace(std::wstring(symbol), &row);
    }

    std::unordered_map<std::wstring, std::size_t> previousColumns;
    previousColumns.reserve(before.columns.size());
    for (std::size_t column = 0; column < before.columns.size(); ++column) {
        const std::wstring_view key = snapshotColumnKey(before, column);
        if (!key.empty()) previousColumns.emplace(std::wstring(key), column);
    }

    for (const auto& importedRow : imported.rows) {
        const std::wstring_view symbol = rowCellValue(importedRow, importedSymbolColumn);
        const auto previousRow = previousRows.find(std::wstring(symbol));
        if (symbol.empty() || previousRow == previousRows.end()) continue;

        for (std::size_t importedColumn = 0; importedColumn < imported.columns.size(); ++importedColumn) {
            if (importedColumn == importedSymbolColumn) continue;
            const std::wstring_view key = snapshotColumnKey(imported, importedColumn);
            const auto previousColumn = previousColumns.find(std::wstring(key));
            if (key.empty() || previousColumn == previousColumns.end()) continue;

            const std::wstring_view oldValue = rowCellValue(*previousRow->second, previousColumn->second);
            const std::wstring_view newValue = rowCellValue(importedRow, importedColumn);
            if (!importedValuesEqual(oldValue, newValue)) {
                changedCells[std::wstring(symbol)].insert(importedColumn);
            }
        }
    }
    return changedCells;
}

bool containsOrdinalIgnoreCase(std::wstring_view value, std::wstring_view search) {
    if (search.empty()) return true;
    if (search.size() > value.size()) return false;
    for (std::size_t offset = 0; offset <= value.size() - search.size(); ++offset) {
        if (CompareStringOrdinal(value.data() + offset, static_cast<int>(search.size()),
                search.data(), static_cast<int>(search.size()), TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

void updateFilterHeaderLabel(std::size_t column) {
    if (column >= currentSnapshot.columns.size() || column >= columnFilters.size()) return;
    HWND header = ListView_GetHeader(symbolList);
    if (!header) return;
    std::wstring label = currentSnapshot.columns[column];
    if (columnFilters[column].active) label += L" *";
    HDITEMW item{};
    item.mask = HDI_TEXT;
    item.pszText = label.data();
    item.cchTextMax = static_cast<int>(label.size());
    Header_SetItem(header, static_cast<int>(column), &item);
}

void refreshVisibleRows() {
    visibleRows.clear();
    if (columnFilters.size() != currentSnapshot.columns.size()) {
        columnFilters.resize(currentSnapshot.columns.size());
    }
    for (std::size_t rowIndex = 0; rowIndex < currentSnapshot.rows.size(); ++rowIndex) {
        const auto& row = currentSnapshot.rows[rowIndex];
        bool matches = true;
        for (std::size_t column = 0; column < columnFilters.size(); ++column) {
            const ColumnFilter& filter = columnFilters[column];
            if (filter.active && !filter.selectedValues.contains(
                    std::wstring(rowCellValue(row, column)))) {
                matches = false;
                break;
            }
        }
        if (matches) visibleRows.push_back(rowIndex);
    }
    if (!symbolList) return;
    ListView_SetItemCountEx(symbolList, static_cast<int>(visibleRows.size()),
        LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (!visibleRows.empty()) {
        ListView_RedrawItems(symbolList, 0, static_cast<int>(visibleRows.size() - 1));
    }
}

void saveVisibleFilterChecks(ColumnFilterPopupState& state) {
    if (!state.valuesList) return;
    const int count = ListView_GetItemCount(state.valuesList);
    for (int itemIndex = 0; itemIndex < count; ++itemIndex) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = itemIndex;
        if (!ListView_GetItem(state.valuesList, &item)) continue;
        const std::size_t valueIndex = static_cast<std::size_t>(item.lParam);
        if (valueIndex >= state.values.size()) continue;
        if (ListView_GetCheckState(state.valuesList, itemIndex)) {
            state.selectedValues.insert(state.values[valueIndex]);
        } else {
            state.selectedValues.erase(state.values[valueIndex]);
        }
    }
}

void refreshFilterList(ColumnFilterPopupState& state, bool saveChecks) {
    if (!state.valuesList) return;
    if (saveChecks) saveVisibleFilterChecks(state);
    const std::wstring search = getText(state.searchEdit);
    state.visibleValueIndices.clear();
    ListView_DeleteAllItems(state.valuesList);
    for (std::size_t valueIndex = 0; valueIndex < state.values.size(); ++valueIndex) {
        const std::wstring& value = state.values[valueIndex];
        const std::wstring displayValue = value.empty() ? L"(Blanks)" : value;
        if (!containsOrdinalIgnoreCase(displayValue, search)) continue;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = ListView_GetItemCount(state.valuesList);
        item.pszText = const_cast<LPWSTR>(displayValue.c_str());
        item.lParam = static_cast<LPARAM>(valueIndex);
        const int inserted = ListView_InsertItem(state.valuesList, &item);
        if (inserted < 0) continue;
        ListView_SetCheckState(state.valuesList, inserted,
            state.selectedValues.contains(value) ? TRUE : FALSE);
        state.visibleValueIndices.push_back(valueIndex);
    }
}

LRESULT CALLBACK columnFilterProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ColumnFilterPopupState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<ColumnFilterPopupState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE: {
        if (!state) return -1;
        CreateWindowW(L"STATIC", L"Search:", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            12, 12, 52, 24, window, nullptr, instanceHandle, nullptr);
        state->searchEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            68, 12, 338, 24, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FILTER_SEARCH)), instanceHandle, nullptr);
        state->valuesList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
            12, 74, 394, 310, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FILTER_VALUES)), instanceHandle, nullptr);
        if (!state->searchEdit || !state->valuesList) return -1;
        ListView_SetExtendedListViewStyle(state->valuesList,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(L"Values");
        column.cx = 375;
        ListView_InsertColumn(state->valuesList, 0, &column);
        CreateWindowW(L"BUTTON", L"Select visible", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            12, 44, 115, 24, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FILTER_SELECT_VISIBLE)), instanceHandle, nullptr);
        CreateWindowW(L"BUTTON", L"Clear selection", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            130, 44, 140, 24, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FILTER_CLEAR_SELECTION)), instanceHandle, nullptr);
        CreateWindowW(L"BUTTON", L"Clear filter", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            130, 394, 94, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FILTER_CLEAR)), instanceHandle, nullptr);
        CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            285, 394, 58, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FILTER_APPLY)), instanceHandle, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            350, 394, 56, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FILTER_CANCEL)), instanceHandle, nullptr);
        setControlTreeFont(window, controlFont);
        refreshFilterList(*state, false);
        return 0;
    }

    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wParam) == IDC_FILTER_SEARCH && HIWORD(wParam) == EN_CHANGE) {
            refreshFilterList(*state, true);
            return 0;
        }
        if (LOWORD(wParam) == IDC_FILTER_SELECT_VISIBLE) {
            saveVisibleFilterChecks(*state);
            for (const std::size_t valueIndex : state->visibleValueIndices) {
                if (valueIndex < state->values.size()) state->selectedValues.insert(state->values[valueIndex]);
            }
            refreshFilterList(*state, false);
            return 0;
        }
        if (LOWORD(wParam) == IDC_FILTER_CLEAR_SELECTION) {
            saveVisibleFilterChecks(*state);
            state->selectedValues.clear();
            refreshFilterList(*state, false);
            return 0;
        }
        if (LOWORD(wParam) == IDC_FILTER_CLEAR) {
            state->clear = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDC_FILTER_APPLY) {
            saveVisibleFilterChecks(*state);
            state->apply = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDC_FILTER_CANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void showColumnFilter(int column) {
    if (operationRunning.load() || column < 0 ||
        static_cast<std::size_t>(column) >= currentSnapshot.columns.size()) return;

    const std::size_t columnIndex = static_cast<std::size_t>(column);
    ColumnFilterPopupState state;
    state.column = columnIndex;
    std::unordered_set<std::wstring> uniqueValues;
    for (const auto& row : currentSnapshot.rows) {
        const std::wstring value(rowCellValue(row, columnIndex));
        if (uniqueValues.insert(value).second) state.values.push_back(value);
    }
    std::sort(state.values.begin(), state.values.end(), [](const auto& left, const auto& right) {
        const int comparison = compareCellValues(left, right);
        if (comparison != 0) return comparison < 0;
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), FALSE) == CSTR_LESS_THAN;
    });
    if (columnIndex < columnFilters.size() && columnFilters[columnIndex].active) {
        state.selectedValues = columnFilters[columnIndex].selectedValues;
    } else {
        state.selectedValues = std::move(uniqueValues);
    }

    WNDCLASSEXW filterClass{};
    filterClass.cbSize = sizeof(filterClass);
    filterClass.hInstance = instanceHandle;
    filterClass.lpfnWndProc = columnFilterProc;
    filterClass.lpszClassName = COLUMN_FILTER_CLASS;
    filterClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    filterClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    if (!RegisterClassExW(&filterClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    const std::wstring title = L"Filter: " + currentSnapshot.columns[columnIndex];
    HWND popup = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        COLUMN_FILTER_CLASS, title.c_str(), WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 470, mainWindow, nullptr, instanceHandle, &state);
    if (!popup) return;

    int x = 0;
    int y = 0;
    HWND header = ListView_GetHeader(symbolList);
    RECT anchor{};
    if (header && Header_GetItemRect(header, column, &anchor)) {
        POINT point{anchor.left, anchor.bottom};
        MapWindowPoints(header, nullptr, &point, 1);
        x = point.x;
        y = point.y;
    } else {
        RECT owner{};
        GetWindowRect(mainWindow, &owner);
        x = owner.left + 40;
        y = owner.top + 80;
    }
    RECT popupRect{};
    GetWindowRect(popup, &popupRect);
    const int width = popupRect.right - popupRect.left;
    const int height = popupRect.bottom - popupRect.top;
    const HMONITOR monitor = MonitorFromPoint(POINT{x, y}, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    RECT workArea{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(monitor, &monitorInfo)) workArea = monitorInfo.rcWork;
    const int workLeft = static_cast<int>(workArea.left);
    const int workTop = static_cast<int>(workArea.top);
    const int workRight = static_cast<int>(workArea.right);
    const int workBottom = static_cast<int>(workArea.bottom);
    const int maxX = workRight - width > workLeft ? workRight - width : workLeft;
    const int maxY = workBottom - height > workTop ? workBottom - height : workTop;
    x = std::clamp(x, workLeft, maxX);
    if (y + height > workArea.bottom) y -= height;
    y = std::clamp(y, workTop, maxY);

    EnableWindow(mainWindow, FALSE);
    SetWindowPos(popup, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(popup);
    SetFocus(state.searchEdit);

    MSG message{};
    while (IsWindow(popup)) {
        const BOOL gotMessage = GetMessageW(&message, nullptr, 0, 0);
        if (gotMessage <= 0) {
            if (gotMessage == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (!IsDialogMessageW(popup, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(mainWindow, TRUE);
    SetActiveWindow(mainWindow);

    if (columnIndex >= columnFilters.size()) columnFilters.resize(currentSnapshot.columns.size());
    if (state.clear) {
        columnFilters[columnIndex] = {};
    } else if (state.apply) {
        ColumnFilter& filter = columnFilters[columnIndex];
        filter.active = state.selectedValues.size() < state.values.size();
        filter.selectedValues = std::move(state.selectedValues);
        if (!filter.active) filter.selectedValues.clear();
    } else {
        return;
    }
    updateFilterHeaderLabel(columnIndex);
    refreshVisibleRows();
}

LRESULT CALLBACK symbolListSubclassProc(HWND window, UINT message, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NOTIFY) {
        const auto* notification = reinterpret_cast<const NMHDR*>(lParam);
        if (notification && notification->hwndFrom == ListView_GetHeader(window) &&
            notification->code == HDN_DROPDOWN) {
            const auto* header = reinterpret_cast<const NMHEADERW*>(lParam);
            PostMessageW(mainWindow, WM_APP_FILTER_COLUMN, static_cast<WPARAM>(header->iItem), 0);
            return 0;
        }
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void updateExportButton() {
    if (exportButton) {
        EnableWindow(exportButton, !operationRunning.load() && !currentSnapshot.columns.empty());
    }
    if (importButton) {
        EnableWindow(importButton, !operationRunning.load() && importedDataReady);
    }
    if (browseExcelButton) {
        EnableWindow(browseExcelButton, !operationRunning.load());
    }
}

void setBusy(bool busy) {
    if (connectButton) EnableWindow(connectButton, !busy);
    if (profileCombo) EnableWindow(profileCombo, !busy);
    if (loginEdit) EnableWindow(loginEdit, !busy);
    if (passwordEdit) EnableWindow(passwordEdit, !busy);
    if (excelPathEdit) EnableWindow(excelPathEdit, !busy);
    updateExportButton();
    if (symbolList) EnableWindow(symbolList, !busy);
}

void clearTable() {
    currentSnapshot = {};
    importedChangedCells.clear();
    currentSymbolColumn = (std::numeric_limits<std::size_t>::max)();
    activeSortColumn = -1;
    activeSortAscending = true;
    columnFilters.clear();
    visibleRows.clear();
    importedDataReady = false;
    excelFilePath.clear();
    if (excelPathEdit) SetWindowTextW(excelPathEdit, L"");
    updateExportButton();
    if (!symbolList) return;
    ListView_SetItemCountEx(symbolList, 0, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    HWND header = ListView_GetHeader(symbolList);
    const int columns = header ? Header_GetItemCount(header) : 0;
    for (int index = columns - 1; index >= 0; --index) {
        ListView_DeleteColumn(symbolList, index);
    }
}

void updateCredentials() {
    const LRESULT selected = SendMessageW(profileCombo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) return;
    const LRESULT index = SendMessageW(profileCombo, CB_GETITEMDATA, selected, 0);
    if (index < 0 || static_cast<std::size_t>(index) >= profiles.size()) return;

    const ConnectionProfile& profile = profiles[static_cast<std::size_t>(index)];
    SetWindowTextW(loginEdit, profile.login.c_str());
    SetWindowTextW(passwordEdit, profile.password.c_str());
    clearTable();
    setStatus(L"Selected profile: " + profile.name + L". Press Connect to retrieve symbol parameters.");
}

void addTableColumns() {
    columnFilters.assign(currentSnapshot.columns.size(), ColumnFilter{});
    for (std::size_t index = 0; index < currentSnapshot.columns.size(); ++index) {
        const std::wstring& name = currentSnapshot.columns[index];
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<LPWSTR>(name.c_str());
        column.iSubItem = static_cast<int>(index);
        const int estimatedWidth = static_cast<int>(name.size() * 8 + 28);
        column.cx = index == 0 ? 150 : std::clamp(estimatedWidth, 120, 420);
        ListView_InsertColumn(symbolList, static_cast<int>(index), &column);
        HWND header = ListView_GetHeader(symbolList);
        HDITEMW headerItem{};
        headerItem.mask = HDI_FORMAT;
        if (header && Header_GetItem(header, static_cast<int>(index), &headerItem)) {
            headerItem.fmt |= HDF_SPLITBUTTON;
            Header_SetItem(header, static_cast<int>(index), &headerItem);
        }
    }
}

bool selectedProfile(ConnectionProfile& profile) {
    if (!profileCombo) return false;
    const LRESULT selected = SendMessageW(profileCombo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) return false;
    const LRESULT index = SendMessageW(profileCombo, CB_GETITEMDATA, selected, 0);
    if (index < 0 || static_cast<std::size_t>(index) >= profiles.size()) return false;
    profile = profiles[static_cast<std::size_t>(index)];
    profile.login = getText(loginEdit);
    profile.password = getText(passwordEdit);
    return true;
}

void startFetch() {
    bool expected = false;
    if (!operationRunning.compare_exchange_strong(expected, true)) return;

    if (!symbolFieldConfigReady) {
        operationRunning.store(false);
        setStatus(symbolFieldConfigError);
        return;
    }

    ConnectionProfile profile;
    if (!selectedProfile(profile)) {
        operationRunning.store(false);
        setStatus(L"No connection profile is selected. Edit connections.ini beside the application.");
        return;
    }
    if (profile.server.empty() || profile.login.empty() || profile.password.empty()) {
        operationRunning.store(false);
        setStatus(L"The selected profile must contain server, login, and password in connections.ini.");
        return;
    }

    clearTable();
    setBusy(true);
    setStatus(L"Connecting to " + profile.name + L" and retrieving symbol parameters...");

    const HWND window = mainWindow;
    const std::shared_ptr<AsyncState> state = asyncState;
    const std::vector<SymbolFieldMapping> fieldMappings = symbolFieldMappings;
    try {
        fetchWorker = std::thread([window, state, profile = std::move(profile),
                                   fieldMappings]() mutable {
        auto result = std::make_unique<FetchResult>();
        result->profile = profile.name;
        result->server = profile.server;
        try {
            result->success = LoadSymbolSnapshot(profile.server, profile.login, profile.password,
                                                  fieldMappings, result->snapshot, result->error);
        } catch (const std::exception& exception) {
            result->success = false;
            result->error = L"Unexpected error while retrieving symbols: ";
            const std::string message = exception.what();
            result->error.append(message.begin(), message.end());
        } catch (...) {
            result->success = false;
            result->error = L"Unexpected error while retrieving symbols.";
        }

        {
            std::lock_guard lock(state->mutex);
            if (!state->windowAlive) {
                operationRunning.store(false);
                return;
            }
            state->result = std::move(result);
        }
        operationRunning.store(false);
        PostMessageW(window, WM_APP_FETCH_COMPLETE, 0, 0);
        });
    } catch (const std::system_error&) {
        operationRunning.store(false);
        setBusy(false);
        setStatus(L"Cannot start the symbol retrieval worker thread.");
    }
}

void showFetchResult() {
    if (fetchWorker.joinable()) fetchWorker.join();
    std::unique_ptr<FetchResult> result;
    {
        std::lock_guard lock(asyncState->mutex);
        result = std::move(asyncState->result);
    }
    setBusy(false);
    if (!result) return;

    if (!result->success) {
        setStatus(result->error);
        return;
    }

    currentSnapshot = std::move(result->snapshot);
    importedChangedCells.clear();
    currentSymbolColumn = findSymbolColumn(currentSnapshot);
    addTableColumns();
    refreshVisibleRows();
    updateExportButton();

    std::wstring firstLine = L"Connected to " + result->profile + L" (" + result->server +
        L"). Retrieved parameters for " + std::to_wstring(currentSnapshot.rows.size()) + L" symbols.";
    std::wstring secondLine = L"Parameters: " + std::to_wstring(currentSnapshot.columns.size()) +
        L". Failed symbol records: " + std::to_wstring(currentSnapshot.failedSymbols) + L".";
    if (currentSnapshot.failedSymbols != 0) {
        secondLine += L" The failed records were skipped.";
    }
    setStatus(firstLine + L"\r\n" + secondLine);
}

void exportSnapshot() {
    if (currentSnapshot.columns.empty() || operationRunning.load()) return;

    wchar_t path[32768] = L"MT5Symbols.xlsx";
    static constexpr wchar_t filter[] =
        L"Excel Workbook (*.xlsx)\0*.xlsx\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = mainWindow;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrTitle = L"Export symbol parameters to Excel";
    dialog.lpstrDefExt = L"xlsx";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT |
        OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) {
        if (CommDlgExtendedError() != 0) {
            setStatus(L"Could not open the Excel export dialog.");
        }
        return;
    }

    setStatus(L"Exporting symbol parameters to Excel...");
    UpdateWindow(statusText);
    std::wstring error;
    if (ExportSnapshotToXlsx(currentSnapshot, path, error)) {
        setStatus(L"Exported " + std::to_wstring(currentSnapshot.rows.size()) +
            L" symbols and " + std::to_wstring(currentSnapshot.columns.size()) +
            L" parameters to Excel.");
    } else {
        setStatus(error);
        MessageBoxW(mainWindow, error.c_str(), L"Excel export failed", MB_OK | MB_ICONERROR);
    }
}

bool normalizeImportedSnapshot(SymbolSnapshot& snapshot,
                               const std::vector<SymbolFieldMapping>& mappings,
                               std::wstring& error) {
    error.clear();
    if (snapshot.columns.empty() || snapshot.rows.empty()) {
        error = L"The workbook must contain a header row and at least one symbol.";
        return false;
    }

    std::unordered_map<std::wstring, std::size_t> expectedColumns;
    expectedColumns.reserve(mappings.size());
    std::size_t symbolColumn = mappings.size();
    for (std::size_t index = 0; index < mappings.size(); ++index) {
        if (!expectedColumns.emplace(mappings[index].displayName, index).second) {
            error = L"symbols.ini contains duplicate column headings; make each heading unique before importing.";
            return false;
        }
        if (mappings[index].sourceName == L"Symbol") symbolColumn = index;
    }
    if (symbolColumn == mappings.size()) {
        error = L"symbols.ini must include the Symbol field before importing a workbook.";
        return false;
    }
    if (snapshot.columns.size() != mappings.size()) {
        error = L"The workbook columns do not match the current symbols.ini configuration.";
        return false;
    }

    std::vector<std::size_t> inputColumnForMapping(mappings.size(), mappings.size());
    std::unordered_set<std::wstring> seenHeadings;
    for (std::size_t inputColumn = 0; inputColumn < snapshot.columns.size(); ++inputColumn) {
        const std::wstring& heading = snapshot.columns[inputColumn];
        if (!seenHeadings.insert(heading).second) {
            error = L"The workbook contains duplicate column heading '" + heading + L"'.";
            return false;
        }
        const auto expected = expectedColumns.find(heading);
        if (expected == expectedColumns.end()) {
            error = L"The workbook contains unknown column '" + heading + L"'. Check that it was exported with the current symbols.ini.";
            return false;
        }
        inputColumnForMapping[expected->second] = inputColumn;
    }
    for (std::size_t index = 0; index < inputColumnForMapping.size(); ++index) {
        if (inputColumnForMapping[index] == mappings.size()) {
            error = L"The workbook is missing column '" + mappings[index].displayName + L"'.";
            return false;
        }
    }

    SymbolSnapshot normalized;
    normalized.columns.reserve(mappings.size());
    normalized.sourceNames.reserve(mappings.size());
    for (const SymbolFieldMapping& mapping : mappings) {
        normalized.columns.push_back(mapping.displayName);
        normalized.sourceNames.push_back(mapping.sourceName);
    }
    normalized.rows.reserve(snapshot.rows.size());
    std::unordered_set<std::wstring> symbols;
    symbols.reserve(snapshot.rows.size());
    for (const auto& sourceRow : snapshot.rows) {
        if (sourceRow.size() != snapshot.columns.size()) {
            error = L"The workbook contains a row with a different number of columns than its header.";
            return false;
        }
        std::vector<std::wstring> row(mappings.size());
        for (std::size_t column = 0; column < mappings.size(); ++column) {
            row[column] = sourceRow[inputColumnForMapping[column]];
        }
        if (row[symbolColumn].empty()) {
            error = L"The workbook contains a row with an empty Symbol value.";
            return false;
        }
        if (!symbols.insert(row[symbolColumn]).second) {
            error = L"The workbook contains duplicate symbol '" + row[symbolColumn] + L"'.";
            return false;
        }
        normalized.rows.push_back(std::move(row));
    }
    snapshot = std::move(normalized);
    return true;
}

void loadExcelWorkbook() {
    if (operationRunning.load()) return;
    wchar_t path[32768]{};
    static constexpr wchar_t filter[] =
        L"Excel Workbook (*.xlsx)\0*.xlsx\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = mainWindow;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrTitle = L"Load symbol parameters from Excel";
    dialog.lpstrDefExt = L"xlsx";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError() != 0) setStatus(L"Could not open the Excel import dialog.");
        return;
    }

    bool expected = false;
    if (!operationRunning.compare_exchange_strong(expected, true)) return;
    const std::wstring selectedPath(path);
    const auto mappings = symbolFieldMappings;
    const HWND window = mainWindow;
    const auto state = asyncState;
    setBusy(true);
    setStatus(L"Reading the Excel workbook and preparing the table...");
    try {
        importWorker = std::thread([window, state, selectedPath, mappings]() mutable {
            auto result = std::make_unique<ImportLoadResult>();
            try {
                result->success = ImportSnapshotFromXlsx(selectedPath, result->snapshot, result->error);
                if (result->success) {
                    result->success = normalizeImportedSnapshot(result->snapshot, mappings, result->error);
                }
            } catch (const std::exception& exception) {
                result->success = false;
                result->error = L"Unexpected error while reading the Excel workbook: ";
                const std::string message = exception.what();
                result->error.append(message.begin(), message.end());
            } catch (...) {
                result->success = false;
                result->error = L"Unexpected error while reading the Excel workbook.";
            }
            {
                std::lock_guard lock(state->mutex);
                if (!state->windowAlive) {
                    operationRunning.store(false);
                    return;
                }
                state->importResult = std::move(result);
                state->importPath = selectedPath;
            }
            operationRunning.store(false);
            PostMessageW(window, WM_APP_IMPORT_COMPLETE, 0, 0);
        });
    } catch (const std::system_error&) {
        operationRunning.store(false);
        setBusy(false);
        setStatus(L"Cannot start the Excel import worker thread.");
    }
}

void showImportLoadResult() {
    if (importWorker.joinable()) importWorker.join();
    std::unique_ptr<ImportLoadResult> result;
    std::wstring path;
    {
        std::lock_guard lock(asyncState->mutex);
        result = std::move(asyncState->importResult);
        path = std::move(asyncState->importPath);
    }
    setBusy(false);
    if (!result) return;
    if (!result->success) {
        setStatus(result->error);
        MessageBoxW(mainWindow, result->error.c_str(), L"Excel import failed", MB_OK | MB_ICONERROR);
        return;
    }

    ChangedCellMap changedCells = findImportedChanges(currentSnapshot, result->snapshot);
    clearTable();
    currentSnapshot = std::move(result->snapshot);
    currentSymbolColumn = findSymbolColumn(currentSnapshot);
    importedChangedCells = std::move(changedCells);
    std::size_t changedCellCount = 0;
    for (const auto& [symbol, columns] : importedChangedCells) {
        (void)symbol;
        changedCellCount += columns.size();
    }
    excelFilePath = std::move(path);
    SetWindowTextW(excelPathEdit, excelFilePath.c_str());
    importedDataReady = true;
    activeSortColumn = -1;
    activeSortAscending = true;
    addTableColumns();
    refreshVisibleRows();
    updateExportButton();
    setStatus(L"Loaded " + std::to_wstring(currentSnapshot.rows.size()) + L" symbols and " +
        std::to_wstring(currentSnapshot.columns.size()) + L" parameters from Excel.\r\n" +
        L"Changed cells: " + std::to_wstring(changedCellCount) +
        L" (light yellow). Import is ready to apply these values to the selected server.");
}

void showAbortPrompt();

void startApplyImport() {
    if (!importedDataReady || currentSnapshot.rows.empty() || operationRunning.load()) return;
    ConnectionProfile profile;
    if (!selectedProfile(profile)) {
        setStatus(L"Select a server profile before applying the imported symbol values.");
        return;
    }
    const std::wstring executableDirectory = GetExecutableDirectory();
    if (executableDirectory.empty()) {
        setStatus(L"Cannot locate the application directory for the symbol backup.");
        return;
    }
    bool expected = false;
    if (!operationRunning.compare_exchange_strong(expected, true)) return;

    {
        std::lock_guard lock(applyControlState->mutex);
        applyControlState->pauseRequested = false;
        applyControlState->choice = ApplyChoice::Continue;
    }
    applyProgressWindow = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        APPLY_PROGRESS_CLASS, L"Applying symbol parameters",
        WS_CAPTION | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 440, 190,
        mainWindow, nullptr, instanceHandle, nullptr);
    if (!applyProgressWindow) {
        operationRunning.store(false);
        setStatus(L"Cannot create the import progress window.");
        return;
    }
    RECT owner{}, progress{};
    GetWindowRect(mainWindow, &owner);
    GetWindowRect(applyProgressWindow, &progress);
    SetWindowPos(applyProgressWindow, HWND_TOPMOST,
        owner.left + ((owner.right - owner.left) - (progress.right - progress.left)) / 2,
        owner.top + ((owner.bottom - owner.top) - (progress.bottom - progress.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);

    setBusy(true);
    setStatus(L"Creating a full backup of current server symbol parameters...");
    const auto state = applyAsyncState;
    const auto control = applyControlState;
    const auto mappings = symbolFieldMappings;
    const auto imported = currentSnapshot;
    const std::wstring backupDirectory = executableDirectory + L"\\backup";
    try {
        applyWorker = std::thread([window = mainWindow, state, control, profile = std::move(profile),
                                   mappings, imported, backupDirectory]() mutable {
            auto result = std::make_unique<ApplyWorkerResult>();
            try {
                const ApplyCheckpoint checkpoint = [window, control](const ApplyProgress& progress) {
                    auto* update = new ApplyProgress(progress);
                    if (!PostMessageW(window, WM_APP_APPLY_PROGRESS,
                                      reinterpret_cast<WPARAM>(update), 0)) delete update;

                    std::unique_lock lock(control->mutex);
                    if (!control->pauseRequested) return ApplyChoice::Continue;
                    lock.unlock();
                    PostMessageW(window, WM_APP_APPLY_PAUSED, 0, 0);
                    lock.lock();
                    control->condition.wait(lock, [&] {
                        return control->choice != ApplyChoice::Continue || !control->pauseRequested;
                    });
                    const ApplyChoice choice = control->choice;
                    if (choice == ApplyChoice::Continue) {
                        control->pauseRequested = false;
                    }
                    return choice;
                };
                result->success = ApplyImportedSymbolSnapshot(profile.server, profile.login,
                    profile.password, mappings, imported, backupDirectory, checkpoint,
                    result->result, result->error);
            } catch (const std::exception& exception) {
                result->success = false;
                result->error = L"Unexpected error while applying imported symbols: ";
                const std::string message = exception.what();
                result->error.append(message.begin(), message.end());
            } catch (...) {
                result->success = false;
                result->error = L"Unexpected error while applying imported symbols.";
            }
            {
                std::lock_guard lock(state->mutex);
                if (!state->windowAlive) {
                    operationRunning.store(false);
                    return;
                }
                state->result = std::move(result);
            }
            operationRunning.store(false);
            PostMessageW(window, WM_APP_APPLY_COMPLETE, 0, 0);
        });
    } catch (const std::system_error&) {
        operationRunning.store(false);
        DestroyWindow(applyProgressWindow);
        applyProgressWindow = nullptr;
        setBusy(false);
        setStatus(L"Cannot start the symbol import worker thread.");
    }
}

void showApplyProgress(const ApplyProgress& progress) {
    if (!applyProgressWindow) return;
    const int total = static_cast<int>(std::min<std::size_t>(progress.total,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int complete = static_cast<int>(std::min<std::size_t>(progress.completed,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    SendMessageW(applyProgressBar, PBM_SETRANGE32, 0, (std::max)(total, 1));
    SendMessageW(applyProgressBar, PBM_SETPOS, complete, 0);
    const std::wstring label = progress.restoring
        ? L"Restoring existing symbols: " + std::to_wstring(progress.completed) + L" / " + std::to_wstring(progress.total)
        : L"Applying symbols: " + std::to_wstring(progress.completed) + L" / " + std::to_wstring(progress.total);
    SetWindowTextW(applyProgressText, label.c_str());
    if (progress.restoring || (progress.total != 0 && progress.completed >= progress.total)) {
        applyFinalizing = !progress.restoring;
        EnableWindow(applyAbortButton, FALSE);
    } else {
        applyFinalizing = false;
        EnableWindow(applyAbortButton, TRUE);
    }
}

void showApplyResult() {
    if (applyWorker.joinable()) applyWorker.join();
    std::unique_ptr<ApplyWorkerResult> result;
    {
        std::lock_guard lock(applyAsyncState->mutex);
        result = std::move(applyAsyncState->result);
    }
    if (applyProgressWindow) DestroyWindow(applyProgressWindow);
    applyProgressWindow = nullptr;
    applyProgressBar = nullptr;
    applyProgressText = nullptr;
    applyAbortButton = nullptr;
    applyFinalizing = false;
    setBusy(false);
    if (!result) return;

    if (!result->result.backupPath.empty()) generatedBackupPaths.push_back(result->result.backupPath);
    if (!result->success) {
        setStatus(result->error);
        MessageBoxW(mainWindow, result->error.c_str(), L"Excel import failed", MB_OK | MB_ICONERROR);
        return;
    }

    const ApplyResult& applied = result->result;
    std::wstring summary = L"Updated: " + std::to_wstring(applied.applied) +
        L". Unchanged: " + std::to_wstring(applied.unchanged) +
        L". Failed: " + std::to_wstring(applied.failed) +
        L". Symbols not found and skipped: " + std::to_wstring(applied.skipped) +
        L". Not processed: " + std::to_wstring(applied.notProcessed) + L".";
    if (applied.aborted) summary += L" Import was aborted.";
    if (applied.restoreRequested) {
        summary += L" Restored: " + std::to_wstring(applied.restored) +
            L". Restore failures: " + std::to_wstring(applied.restoreFailed) + L".";
    }
    if (!applied.firstFailure.empty()) summary += L"\r\nFirst failure: " + applied.firstFailure;
    if (!applied.backupPath.empty()) summary += L"\r\nBackup: " + applied.backupPath;
    setStatus(summary);
    MessageBoxW(mainWindow, summary.c_str(), L"Symbol import result", MB_OK |
        ((applied.aborted || applied.failed || applied.skipped || applied.restoreFailed)
            ? MB_ICONWARNING : MB_ICONINFORMATION));
}

struct AbortPromptState {
    ApplyChoice choice = ApplyChoice::Continue;
};

LRESULT CALLBACK applyProgressProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateWindowW(L"STATIC", L"Preparing the symbol update...", WS_CHILD | WS_VISIBLE | SS_LEFT,
            18, 18, 390, 26, window, nullptr, instanceHandle, nullptr);
        applyProgressBar = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
            18, 54, 390, 22, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS_BAR)),
            instanceHandle, nullptr);
        applyProgressText = CreateWindowW(L"STATIC", L"Applying symbols: 0 / 0", WS_CHILD | WS_VISIBLE | SS_LEFT,
            18, 84, 390, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS_TEXT)),
            instanceHandle, nullptr);
        applyAbortButton = CreateWindowW(L"BUTTON", L"Abort", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            310, 118, 98, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROGRESS_ABORT)),
            instanceHandle, nullptr);
        setControlTreeFont(window, controlFont);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_PROGRESS_ABORT && HIWORD(wParam) == BN_CLICKED) {
            if (!operationRunning.load() || !applyAbortButton || !IsWindowEnabled(applyAbortButton)) return 0;
            EnableWindow(applyAbortButton, FALSE);
            SetWindowTextW(applyProgressText, L"Pausing after the current symbol...");
            {
                std::lock_guard lock(applyControlState->mutex);
                applyControlState->pauseRequested = true;
                applyControlState->choice = ApplyChoice::Continue;
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK abortPromptProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AbortPromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<AbortPromptState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC",
            L"The import is paused. Confirm stops after the current symbol and keeps completed changes.\n"
            L"Confirm and Restore existing symbols reverts every change completed by this import.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 18, 16, 580, 52, window, nullptr, instanceHandle, nullptr);
        CreateWindowW(L"BUTTON", L"Confirm",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 260, 82, 100, 32,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ABORT_CONFIRM)), instanceHandle, nullptr);
        CreateWindowW(L"BUTTON", L"Confirm and Restore existing symbols",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 370, 82, 230, 32,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ABORT_RESTORE)), instanceHandle, nullptr);
        setControlTreeFont(window, controlFont);
        return 0;
    }
    case WM_COMMAND:
        if (state && LOWORD(wParam) == IDC_ABORT_CONFIRM) {
            state->choice = ApplyChoice::StopKeepChanges;
            DestroyWindow(window);
            return 0;
        }
        if (state && LOWORD(wParam) == IDC_ABORT_RESTORE) {
            state->choice = ApplyChoice::StopAndRestore;
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void showAbortPrompt() {
    if (!applyProgressWindow) return;
    AbortPromptState state;
    HWND prompt = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        ABORT_PROMPT_CLASS, L"Abort Import",
        WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 620, 165,
        applyProgressWindow, nullptr, instanceHandle, &state);
    if (!prompt) {
        std::lock_guard lock(applyControlState->mutex);
        applyControlState->choice = ApplyChoice::Continue;
        applyControlState->pauseRequested = false;
        applyControlState->condition.notify_all();
        if (!applyFinalizing) EnableWindow(applyAbortButton, TRUE);
        return;
    }
    RECT owner{}, dialog{};
    GetWindowRect(applyProgressWindow, &owner);
    GetWindowRect(prompt, &dialog);
    SetWindowPos(prompt, HWND_TOPMOST,
        owner.left + ((owner.right - owner.left) - (dialog.right - dialog.left)) / 2,
        owner.top + ((owner.bottom - owner.top) - (dialog.bottom - dialog.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    EnableWindow(applyProgressWindow, FALSE);
    SetFocus(prompt);
    MSG message{};
    while (IsWindow(prompt)) {
        const BOOL received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0) break;
        if (!IsDialogMessageW(prompt, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(applyProgressWindow, TRUE);
    SetActiveWindow(applyProgressWindow);
    {
        std::lock_guard lock(applyControlState->mutex);
        applyControlState->choice = state.choice;
        applyControlState->pauseRequested = false;
        applyControlState->condition.notify_all();
    }
    if (state.choice == ApplyChoice::Continue) {
        if (!applyFinalizing) EnableWindow(applyAbortButton, TRUE);
        SetWindowTextW(applyProgressText, L"Import resumed.");
    } else if (state.choice == ApplyChoice::StopAndRestore) {
        SetWindowTextW(applyProgressText, L"Stopping and restoring existing symbols...");
    } else {
        SetWindowTextW(applyProgressText, L"Stopping after the current symbol...");
    }
}

void addLabel(HWND parent, const wchar_t* text, int x, int y, int width) {
    HWND label = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                               x, y, width, 30, parent, nullptr, instanceHandle, nullptr);
    if (controlFont) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont), TRUE);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        if (control == statusText) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        // Read-only edit controls use WM_CTLCOLORSTATIC; repaint them on an opaque field background.
        if (control == loginEdit || control == passwordEdit || control == excelPathEdit) {
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_CONNECT && HIWORD(wParam) == BN_CLICKED) {
            startFetch();
            return 0;
        }
        if (LOWORD(wParam) == IDC_EXPORT && HIWORD(wParam) == BN_CLICKED) {
            exportSnapshot();
            return 0;
        }
        if (LOWORD(wParam) == IDC_BROWSE_EXCEL && HIWORD(wParam) == BN_CLICKED) {
            loadExcelWorkbook();
            return 0;
        }
        if (LOWORD(wParam) == IDC_IMPORT && HIWORD(wParam) == BN_CLICKED) {
            startApplyImport();
            return 0;
        }
        if (LOWORD(wParam) == IDC_PROFILE && HIWORD(wParam) == CBN_SELCHANGE) {
            const LRESULT selected = SendMessageW(profileCombo, CB_GETCURSEL, 0, 0);
            if (selected == CB_ERR || selected == activeProfileSelection) return 0;
            if (activeProfileSelection != CB_ERR && !confirmProfileConnect()) {
                SendMessageW(profileCombo, CB_SETCURSEL, activeProfileSelection, 0);
                return 0;
            }
            const bool reconnect = activeProfileSelection != CB_ERR;
            activeProfileSelection = selected;
            updateCredentials();
            if (reconnect) startFetch();
            return 0;
        }
        break;

    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (!header) break;
        const HWND listHeader = symbolList ? ListView_GetHeader(symbolList) : nullptr;
        if (header->hwndFrom == listHeader && header->code == HDN_DROPDOWN) {
            const auto* dropdown = reinterpret_cast<const NMHEADERW*>(lParam);
            PostMessageW(window, WM_APP_FILTER_COLUMN, static_cast<WPARAM>(dropdown->iItem), 0);
            return 0;
        }
        if (header->hwndFrom != symbolList) break;
        if (header->code == NM_CUSTOMDRAW) {
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
            if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                // ListView can reuse the custom-draw structure for consecutive subitems.
                // Reset colors before checking this cell so a changed cell's background
                // cannot bleed into later, unchanged cells in the same row.
                draw->clrText = CLR_DEFAULT;
                draw->clrTextBk = CLR_DEFAULT;
                if (currentSymbolColumn >= currentSnapshot.columns.size() ||
                    draw->nmcd.dwItemSpec >= visibleRows.size() || draw->iSubItem < 0) {
                    return CDRF_DODEFAULT;
                }
                const std::size_t visibleIndex = static_cast<std::size_t>(draw->nmcd.dwItemSpec);
                const std::size_t rowIndex = visibleRows[visibleIndex];
                if (rowIndex >= currentSnapshot.rows.size()) return CDRF_DODEFAULT;

                const std::wstring_view symbol = rowCellValue(currentSnapshot.rows[rowIndex], currentSymbolColumn);
                const auto changedSymbol = importedChangedCells.find(std::wstring(symbol));
                if (changedSymbol != importedChangedCells.end() &&
                    changedSymbol->second.contains(static_cast<std::size_t>(draw->iSubItem))) {
                    const COLORREF highlightColor = RGB(255, 255, 204);
                    draw->clrTextBk = highlightColor;
                    draw->clrText = GetSysColor(COLOR_WINDOWTEXT);
                    return CDRF_NEWFONT;
                }
            }
            return CDRF_DODEFAULT;
        }
        if (header->code == LVN_GETDISPINFOW) {
            auto* info = reinterpret_cast<NMLVDISPINFOW*>(lParam);
            if ((info->item.mask & LVIF_TEXT) != 0 && info->item.iItem >= 0 && info->item.iSubItem >= 0) {
                const std::size_t visibleIndex = static_cast<std::size_t>(info->item.iItem);
                const std::size_t column = static_cast<std::size_t>(info->item.iSubItem);
                if (visibleIndex < visibleRows.size()) {
                    const std::size_t row = visibleRows[visibleIndex];
                    if (row < currentSnapshot.rows.size() && column < currentSnapshot.rows[row].size()) {
                    info->item.pszText = const_cast<LPWSTR>(currentSnapshot.rows[row][column].c_str());
                    }
                }
            }
            return 0;
        }
        if (header->code == LVN_COLUMNCLICK) {
            const auto* click = reinterpret_cast<const NMLISTVIEW*>(lParam);
            const int column = click->iSubItem;
            if (column >= 0 && static_cast<std::size_t>(column) < currentSnapshot.columns.size() &&
                !currentSnapshot.rows.empty()) {
                if (activeSortColumn == column) {
                    activeSortAscending = !activeSortAscending;
                } else {
                    activeSortColumn = column;
                    activeSortAscending = true;
                }
                std::stable_sort(currentSnapshot.rows.begin(), currentSnapshot.rows.end(),
                    [ascending = activeSortAscending, column](const auto& left, const auto& right) {
                        const std::wstring_view leftValue = static_cast<std::size_t>(column) < left.size()
                            ? std::wstring_view(left[static_cast<std::size_t>(column)]) : std::wstring_view{};
                        const std::wstring_view rightValue = static_cast<std::size_t>(column) < right.size()
                            ? std::wstring_view(right[static_cast<std::size_t>(column)]) : std::wstring_view{};
                        if (leftValue.empty() != rightValue.empty()) return !leftValue.empty();
                        const int comparison = compareCellValues(leftValue, rightValue);
                        return ascending ? comparison < 0 : comparison > 0;
                    });
                refreshVisibleRows();

                const HWND currentHeader = ListView_GetHeader(symbolList);
                const int columnCount = Header_GetItemCount(currentHeader);
                for (int index = 0; index < columnCount; ++index) {
                    HDITEMW item{};
                    item.mask = HDI_FORMAT;
                    if (!Header_GetItem(currentHeader, index, &item)) continue;
                    item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
                    if (index == activeSortColumn) {
                        item.fmt |= activeSortAscending ? HDF_SORTUP : HDF_SORTDOWN;
                    }
                    Header_SetItem(currentHeader, index, &item);
                }
            }
            return 0;
        }
        break;
    }

    case WM_APP_FETCH_COMPLETE:
        showFetchResult();
        return 0;

    case WM_APP_FILTER_COLUMN:
        showColumnFilter(static_cast<int>(wParam));
        return 0;

    case WM_APP_IMPORT_COMPLETE:
        showImportLoadResult();
        return 0;

    case WM_APP_APPLY_PROGRESS: {
        std::unique_ptr<ApplyProgress> progress(reinterpret_cast<ApplyProgress*>(wParam));
        if (progress) showApplyProgress(*progress);
        return 0;
    }

    case WM_APP_APPLY_PAUSED:
        showAbortPrompt();
        return 0;

    case WM_APP_APPLY_COMPLETE:
        showApplyResult();
        return 0;

    case WM_CLOSE:
        if (operationRunning.load()) {
            MessageBoxW(window,
                L"A server or Excel operation is still running. Wait for it to finish before closing the application.",
                L"MT5 Symbol Manager", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        if (symbolList) RemoveWindowSubclass(symbolList, symbolListSubclassProc, 1);
        {
            std::lock_guard lock(asyncState->mutex);
            asyncState->windowAlive = false;
            asyncState->result.reset();
            asyncState->importResult.reset();
        }
        {
            std::lock_guard lock(applyAsyncState->mutex);
            applyAsyncState->windowAlive = false;
            if (applyAsyncState->result && !applyAsyncState->result->result.backupPath.empty()) {
                generatedBackupPaths.push_back(applyAsyncState->result->result.backupPath);
            }
            applyAsyncState->result.reset();
        }
        if (fetchWorker.joinable()) fetchWorker.join();
        if (importWorker.joinable()) importWorker.join();
        if (applyWorker.joinable()) applyWorker.join();
        for (const std::wstring& path : generatedBackupPaths) DeleteFileW(path.c_str());
        if (!backupDirectoryPath.empty()) RemoveDirectoryW(backupDirectoryPath.c_str());
        if (controlFont) DeleteObject(controlFont);
        if (statusFont) DeleteObject(statusFont);
        if (tableFont) DeleteObject(tableFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    instanceHandle = instance;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);

    const std::wstring executableDirectory = GetExecutableDirectory();
    if (executableDirectory.empty()) return 1;
    backupDirectoryPath = executableDirectory + L"\\backup";
    const std::wstring configPath = executableDirectory + L"\\connections.ini";
    const std::wstring symbolConfigPath = executableDirectory + L"\\symbols.ini";
    bool configCreated = false;
    std::wstring configError;
    if (!LoadConnectionProfiles(configPath, profiles, configCreated, configError)) {
        profiles.clear();
    }
    bool symbolConfigCreated = false;
    symbolFieldConfigReady = LoadSymbolFieldMappings(symbolConfigPath, symbolFieldMappings,
        symbolConfigCreated, symbolFieldConfigError);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProc;
    windowClass.lpszClassName = L"MT5SymbolManagerMainWindow";
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    if (!RegisterClassExW(&windowClass)) return 1;

    WNDCLASSEXW promptClass{};
    promptClass.cbSize = sizeof(promptClass);
    promptClass.hInstance = instance;
    promptClass.lpfnWndProc = profilePromptProc;
    promptClass.lpszClassName = PROFILE_PROMPT_CLASS;
    promptClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    promptClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    if (!RegisterClassExW(&promptClass)) return 1;

    WNDCLASSEXW progressClass{};
    progressClass.cbSize = sizeof(progressClass);
    progressClass.hInstance = instance;
    progressClass.lpfnWndProc = applyProgressProc;
    progressClass.lpszClassName = APPLY_PROGRESS_CLASS;
    progressClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    progressClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    if (!RegisterClassExW(&progressClass)) return 1;

    WNDCLASSEXW abortClass{};
    abortClass.cbSize = sizeof(abortClass);
    abortClass.hInstance = instance;
    abortClass.lpfnWndProc = abortPromptProc;
    abortClass.lpszClassName = ABORT_PROMPT_CLASS;
    abortClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    abortClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    if (!RegisterClassExW(&abortClass)) return 1;

    mainWindow = CreateWindowExW(0, windowClass.lpszClassName, L"MT5 Symbol Manager",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1360, 800, nullptr, nullptr, instance, nullptr);
    if (!mainWindow) return 1;

    HDC windowDc = GetDC(mainWindow);
    const int fontDpi = windowDc ? GetDeviceCaps(windowDc, LOGPIXELSY) : 96;
    if (windowDc) ReleaseDC(mainWindow, windowDc);
    controlFont = CreateFontW(-MulDiv(12, fontDpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    statusFont = CreateFontW(-MulDiv(13, fontDpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    tableFont = CreateFontW(-MulDiv(10, fontDpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    constexpr int top = 14;
    addLabel(mainWindow, L"Server:", 12, top, 58);
    profileCombo = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        70, top, 300, 220, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROFILE)), instance, nullptr);
    addLabel(mainWindow, L"Login:", 388, top, 50);
    loginEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL,
        438, top, 125, 30, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGIN)), instance, nullptr);
    addLabel(mainWindow, L"Password:", 580, top, 75);
    passwordEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY | ES_PASSWORD | ES_AUTOHSCROLL,
        655, top, 200, 30, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PASSWORD)), instance, nullptr);
    importButton = CreateWindowW(L"BUTTON", L"Import", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        875, top, 140, 30, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_IMPORT)), instance, nullptr);
    exportButton = CreateWindowW(L"BUTTON", L"Export Excel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        1035, top, 145, 30, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXPORT)), instance, nullptr);
    connectButton = CreateWindowW(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        1195, top, 145, 30, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CONNECT)), instance, nullptr);
    addLabel(mainWindow, L"Excel:", 12, 52, 58);
    excelPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
        70, 52, 1110, 30, mainWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXCEL_PATH)), instance, nullptr);
    browseExcelButton = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        1195, 52, 145, 30, mainWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE_EXCEL)), instance, nullptr);
    statusText = CreateWindowW(L"STATIC", L"Ready.", WS_CHILD | WS_VISIBLE | SS_LEFT,
        12, 90, 1328, 44, mainWindow, nullptr, instance, nullptr);
    symbolList = CreateWindowW(WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
        12, 140, 1328, 610, mainWindow, nullptr, instance, nullptr);
    ListView_SetExtendedListViewStyle(symbolList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    SetWindowSubclass(symbolList, symbolListSubclassProc, 1, 0);

    setControlTreeFont(mainWindow, controlFont);
    setControlTreeFont(symbolList, tableFont);
    const int scaledEditMargin = MulDiv(5, fontDpi, 96);
    const int editMargin = scaledEditMargin > 4 ? scaledEditMargin : 4;
    for (HWND edit : {loginEdit, passwordEdit, excelPathEdit}) {
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELONG(editMargin, editMargin));
        InvalidateRect(edit, nullptr, TRUE);
    }
    EnableWindow(exportButton, FALSE);
    EnableWindow(importButton, FALSE);
    if (statusFont) SendMessageW(statusText, WM_SETFONT, reinterpret_cast<WPARAM>(statusFont), TRUE);

    for (std::size_t index = 0; index < profiles.size(); ++index) {
        const LRESULT item = SendMessageW(profileCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(profiles[index].name.c_str()));
        if (item != CB_ERR && item != CB_ERRSPACE) {
            SendMessageW(profileCombo, CB_SETITEMDATA, item, static_cast<LPARAM>(index));
        }
    }

    if (SendMessageW(profileCombo, CB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(profileCombo, CB_SETCURSEL, 0, 0);
        activeProfileSelection = SendMessageW(profileCombo, CB_GETCURSEL, 0, 0);
        updateCredentials();
        if (configCreated) {
            setStatus(L"Created " + configPath + L". Edit the example connection profile, then restart the app.");
        } else if (!configError.empty()) {
            setStatus(configError);
        }
    } else {
        setStatus(configError.empty()
            ? L"No connection profiles are configured in " + configPath
            : configError);
    }
    if (!symbolFieldConfigReady) {
        setStatus(symbolFieldConfigError);
    } else if (symbolConfigCreated) {
        setStatus(L"Created " + symbolConfigPath + L" with all available symbol fields.");
    }

    ShowWindow(mainWindow, showCommand);
    UpdateWindow(mainWindow);
    if (configCreated || symbolConfigCreated) {
        std::wstring notice;
        if (configCreated) {
            notice += L"Created a sample connection profile file:\n" + configPath +
                L"\nEdit it with your MT5 server, login, and password, then restart the application.\n\n";
        }
        if (symbolConfigCreated) {
            notice += L"Created the symbol field configuration:\n" + symbolConfigPath +
                L"\nEdit the right side of each SourceField=ColumnHeading line to rename columns. "
                L"Line order controls the table and Excel export order.\n"
                L"Remove lines for fields you do not need.";
        }
        MessageBoxW(mainWindow, notice.c_str(), L"Configuration files created",
            MB_OK | MB_ICONINFORMATION);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
