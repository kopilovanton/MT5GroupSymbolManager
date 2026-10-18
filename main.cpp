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
constexpr UINT WM_APP_SYNC_TABLE_SCROLL = WM_APP + 7;
constexpr int PINNED_SYMBOL_COLUMN_WIDTH = 100;
constexpr int PINNED_PATH_COLUMN_WIDTH = 150;
constexpr int PINNED_SYMBOL_MIN_WIDTH = 20;
constexpr int PINNED_PATH_MIN_WIDTH = 20;
constexpr int TABLE_LEFT = 12;
constexpr int TABLE_TOP_WIDE = 90;
constexpr int TABLE_TOP_NARROW = 124;
constexpr int STATUS_HEIGHT = 44;
constexpr int STATUS_GAP = 6;
constexpr int TABLE_RIGHT_MARGIN = 4;
constexpr int TABLE_BOTTOM_MARGIN = 11;
constexpr int NARROW_LAYOUT_WIDTH = 1150;
constexpr wchar_t SINGLE_INSTANCE_MUTEX_NAME[] = L"Local\\MT5SymbolManager.SingleInstance";
constexpr int IDC_PROFILE = 1001;
constexpr int IDC_LOGIN = 1002;
constexpr int IDC_PASSWORD = 1003;
constexpr int IDC_CONNECT = 1004;
constexpr int IDC_EXPORT = 1005;
constexpr int IDC_APPLY = 1006;
constexpr int IDC_EXCEL_PATH = 1007;
constexpr int IDC_BROWSE_EXCEL = 1008;
constexpr int IDC_SHOW_CHANGED_SYMBOLS = 1009;
constexpr int IDC_CELL_EDITOR = 1501;
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
HWND serverLabel = nullptr;
HWND loginLabel = nullptr;
HWND passwordLabel = nullptr;
HWND excelLabel = nullptr;
HWND loginEdit = nullptr;
HWND passwordEdit = nullptr;
HWND connectButton = nullptr;
HWND exportButton = nullptr;
HWND applyButton = nullptr;
HWND excelPathEdit = nullptr;
HWND browseExcelButton = nullptr;
HWND showChangedSymbolsCheckbox = nullptr;
HWND cellEditor = nullptr;
HWND statusText = nullptr;
HWND symbolList = nullptr;
HWND pinnedSymbolList = nullptr;
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
SymbolSnapshot serverSnapshot;
SymbolSnapshot manualEditBaseline;
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
bool showChangedSymbolsOnly = false;
bool applyFinalizing = false;
std::size_t editingRowIndex = (std::numeric_limits<std::size_t>::max)();
std::size_t editingColumnIndex = (std::numeric_limits<std::size_t>::max)();
std::wstring cellEditorOriginalValue;
std::wstring excelFilePath;
std::wstring backupDirectoryPath;
std::vector<std::wstring> generatedBackupPaths;

void setControlTreeFont(HWND parent, HFONT font);
void finishCellEdit(bool commit);

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
std::vector<std::size_t> pinnedColumnIndices;
std::vector<std::size_t> scrollableColumnIndices;
bool syncingTableScroll = false;
bool syncingTableSelection = false;
int pinnedSymbolColumnWidth = PINNED_SYMBOL_COLUMN_WIDTH;
int pinnedPathColumnWidth = PINNED_PATH_COLUMN_WIDTH;
void layoutTableForClient(int clientWidth, int clientHeight);

const std::vector<std::size_t>& tableColumnsForView(HWND view) {
    return view == pinnedSymbolList ? pinnedColumnIndices : scrollableColumnIndices;
}

std::size_t snapshotColumnForView(HWND view, int localColumn) {
    const auto& columns = tableColumnsForView(view);
    if (localColumn < 0 || static_cast<std::size_t>(localColumn) >= columns.size()) {
        return (std::numeric_limits<std::size_t>::max)();
    }
    return columns[static_cast<std::size_t>(localColumn)];
}

int localColumnForSnapshot(HWND view, std::size_t snapshotColumn) {
    const auto& columns = tableColumnsForView(view);
    const auto found = std::find(columns.begin(), columns.end(), snapshotColumn);
    return found == columns.end() ? -1 : static_cast<int>(found - columns.begin());
}

int pinnedColumnsWidth() {
    int width = 0;
    for (const std::size_t column : pinnedColumnIndices) {
        if (column >= currentSnapshot.sourceNames.size()) continue;
        if (currentSnapshot.sourceNames[column] == L"Symbol") {
            width += pinnedSymbolColumnWidth;
        } else if (currentSnapshot.sourceNames[column] == L"Path") {
            width += pinnedPathColumnWidth;
        }
    }
    return width;
}

void syncPinnedColumnWidthsFromHeader() {
    if (!pinnedSymbolList) return;
    HWND header = ListView_GetHeader(pinnedSymbolList);
    if (!header) return;
    for (int localColumn = 0; localColumn < Header_GetItemCount(header); ++localColumn) {
        const std::size_t column = snapshotColumnForView(pinnedSymbolList, localColumn);
        if (column >= currentSnapshot.sourceNames.size()) continue;
        HDITEMW item{};
        item.mask = HDI_WIDTH;
        if (!Header_GetItem(header, localColumn, &item)) continue;
        if (currentSnapshot.sourceNames[column] == L"Symbol") {
            pinnedSymbolColumnWidth = (std::max)(PINNED_SYMBOL_MIN_WIDTH, item.cxy);
        } else if (currentSnapshot.sourceNames[column] == L"Path") {
            pinnedPathColumnWidth = (std::max)(PINNED_PATH_MIN_WIDTH, item.cxy);
        }
    }
}

void trackPinnedColumnWidth(NMHEADERW& notification) {
    if (!notification.pitem || (notification.pitem->mask & HDI_WIDTH) == 0) return;
    const std::size_t column = snapshotColumnForView(pinnedSymbolList, notification.iItem);
    if (column >= currentSnapshot.sourceNames.size()) return;
    int minimumWidth = 0;
    if (currentSnapshot.sourceNames[column] == L"Symbol") {
        minimumWidth = PINNED_SYMBOL_MIN_WIDTH;
    } else if (currentSnapshot.sourceNames[column] == L"Path") {
        minimumWidth = PINNED_PATH_MIN_WIDTH;
    } else {
        return;
    }
    notification.pitem->cxy = (std::max)(minimumWidth, notification.pitem->cxy);
    if (currentSnapshot.sourceNames[column] == L"Symbol") {
        pinnedSymbolColumnWidth = notification.pitem->cxy;
    } else {
        pinnedPathColumnWidth = notification.pitem->cxy;
    }
    RECT client{};
    if (mainWindow && GetClientRect(mainWindow, &client)) {
        layoutTableForClient(client.right, client.bottom);
    }
}

void clipPinnedScrollBar() {
    if (!pinnedSymbolList) return;
    RECT windowRect{};
    if (!GetWindowRect(pinnedSymbolList, &windowRect)) return;
    const int height = windowRect.bottom - windowRect.top;
    const int contentWidth = pinnedColumnsWidth();
    if (contentWidth <= 0) return;
    HRGN visibleRegion = CreateRectRgn(0, 0, contentWidth, height);
    if (!visibleRegion) return;
    if (!SetWindowRgn(pinnedSymbolList, visibleRegion, TRUE)) {
        DeleteObject(visibleRegion);
    }
}

void layoutTableForClient(int clientWidth, int clientHeight) {
    const int tableTop = clientWidth < NARROW_LAYOUT_WIDTH
        ? TABLE_TOP_NARROW : TABLE_TOP_WIDE;
    const int pinnedContentWidth = pinnedColumnsWidth();

    const int tableWidth = (std::max)(0,
        clientWidth - TABLE_LEFT - TABLE_RIGHT_MARGIN);
    const int statusY = clientHeight - STATUS_HEIGHT - TABLE_BOTTOM_MARGIN;
    const int tableHeight = (std::max)(0,
        statusY - STATUS_GAP - tableTop);
    const int pinnedWindowWidth = pinnedContentWidth > 0
        ? pinnedContentWidth + GetSystemMetrics(SM_CXVSCROLL) : 0;
    if (pinnedSymbolList) {
        ShowWindow(pinnedSymbolList, pinnedWindowWidth > 0 ? SW_SHOW : SW_HIDE);
        MoveWindow(pinnedSymbolList, TABLE_LEFT, tableTop,
            pinnedWindowWidth, tableHeight, TRUE);
        ShowScrollBar(pinnedSymbolList, SB_VERT, FALSE);
        clipPinnedScrollBar();
    }
    if (symbolList) {
        MoveWindow(symbolList, TABLE_LEFT + pinnedContentWidth, tableTop,
            (std::max)(0, tableWidth - pinnedContentWidth), tableHeight, TRUE);
    }
    if (statusText) {
        MoveWindow(statusText, 12, statusY,
            (std::max)(0, clientWidth - 16), STATUS_HEIGHT, TRUE);
    }
}

HWND tableViewForSnapshotColumn(std::size_t snapshotColumn) {
    return localColumnForSnapshot(pinnedSymbolList, snapshotColumn) >= 0
        ? pinnedSymbolList : symbolList;
}

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
            if (key == L"Path") continue;
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

void preserveImportedPathValues(SymbolSnapshot& imported, const SymbolSnapshot& server) {
    if (imported.rows.empty()) return;
    std::size_t importedSymbolColumn = (std::numeric_limits<std::size_t>::max)();
    std::size_t importedPathColumn = (std::numeric_limits<std::size_t>::max)();
    std::size_t serverSymbolColumn = (std::numeric_limits<std::size_t>::max)();
    std::size_t serverPathColumn = (std::numeric_limits<std::size_t>::max)();
    for (std::size_t column = 0; column < imported.sourceNames.size(); ++column) {
        if (imported.sourceNames[column] == L"Symbol") importedSymbolColumn = column;
        if (imported.sourceNames[column] == L"Path") importedPathColumn = column;
    }
    for (std::size_t column = 0; column < server.sourceNames.size(); ++column) {
        if (server.sourceNames[column] == L"Symbol") serverSymbolColumn = column;
        if (server.sourceNames[column] == L"Path") serverPathColumn = column;
    }
    if (importedPathColumn == (std::numeric_limits<std::size_t>::max)()) return;

    if (importedSymbolColumn == (std::numeric_limits<std::size_t>::max)() ||
        serverSymbolColumn == (std::numeric_limits<std::size_t>::max)() ||
        serverPathColumn == (std::numeric_limits<std::size_t>::max)() || server.rows.empty()) {
        for (auto& row : imported.rows) {
            if (importedPathColumn < row.size()) row[importedPathColumn].clear();
        }
        return;
    }

    std::unordered_map<std::wstring, std::wstring_view> serverPaths;
    serverPaths.reserve(server.rows.size());
    for (const auto& row : server.rows) {
        const std::wstring_view symbol = rowCellValue(row, serverSymbolColumn);
        if (!symbol.empty()) serverPaths.emplace(std::wstring(symbol), rowCellValue(row, serverPathColumn));
    }
    for (auto& row : imported.rows) {
        if (importedSymbolColumn >= row.size() || importedPathColumn >= row.size()) continue;
        const auto path = serverPaths.find(row[importedSymbolColumn]);
        if (path != serverPaths.end()) row[importedPathColumn] = path->second;
        else row[importedPathColumn].clear();
    }
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
    HWND view = tableViewForSnapshotColumn(column);
    const int localColumn = localColumnForSnapshot(view, column);
    HWND header = view ? ListView_GetHeader(view) : nullptr;
    if (!header) return;
    std::wstring label = currentSnapshot.columns[column];
    if (columnFilters[column].active) label += L" *";
    HDITEMW item{};
    item.mask = HDI_TEXT;
    item.pszText = label.data();
    item.cchTextMax = static_cast<int>(label.size());
    Header_SetItem(header, localColumn, &item);
}

void syncTableScroll(HWND source) {
    if (syncingTableScroll || !pinnedSymbolList || !symbolList ||
        (source != pinnedSymbolList && source != symbolList)) return;
    HWND target = source == symbolList ? pinnedSymbolList : symbolList;
    const int count = ListView_GetItemCount(source);
    if (count <= 0) return;
    int top = ListView_GetTopIndex(source);
    if (top < 0) return;
    top = (std::min)(top, count - 1);
    const int currentTargetTop = ListView_GetTopIndex(target);
    if (currentTargetTop == top) return;

    syncingTableScroll = true;
    ListView_EnsureVisible(target, top, FALSE);
    const int alignedTargetTop = ListView_GetTopIndex(target);
    if (alignedTargetTop != top) {
        RECT itemRect{};
        int rowHeight = 0;
        if (ListView_GetItemRect(target, top, &itemRect, LVIR_BOUNDS)) {
            rowHeight = itemRect.bottom - itemRect.top;
        }
        if (rowHeight > 0) {
            ListView_Scroll(target, 0, (top - alignedTargetTop) * rowHeight);
        }
    }
    if (target == pinnedSymbolList) {
        ShowScrollBar(target, SB_VERT, FALSE);
    }
    syncingTableScroll = false;
}

void refreshVisibleRows() {
    visibleRows.clear();
    if (columnFilters.size() != currentSnapshot.columns.size()) {
        columnFilters.resize(currentSnapshot.columns.size());
    }
    for (std::size_t rowIndex = 0; rowIndex < currentSnapshot.rows.size(); ++rowIndex) {
        const auto& row = currentSnapshot.rows[rowIndex];
        if (showChangedSymbolsOnly) {
            if (currentSymbolColumn >= row.size()) continue;
            const auto changedSymbol = importedChangedCells.find(row[currentSymbolColumn]);
            if (changedSymbol == importedChangedCells.end() || changedSymbol->second.empty()) continue;
        }
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
    const int itemCount = static_cast<int>(visibleRows.size());
    ListView_SetItemCountEx(symbolList, itemCount, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (pinnedSymbolList) {
        ListView_SetItemCountEx(pinnedSymbolList, itemCount,
            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        ShowScrollBar(pinnedSymbolList, SB_VERT, FALSE);
    }
    if (!visibleRows.empty()) {
        ListView_RedrawItems(symbolList, 0, itemCount - 1);
        if (pinnedSymbolList) ListView_RedrawItems(pinnedSymbolList, 0, itemCount - 1);
        syncTableScroll(symbolList);
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
    HWND view = tableViewForSnapshotColumn(columnIndex);
    const int localColumn = localColumnForSnapshot(view, columnIndex);
    HWND header = view ? ListView_GetHeader(view) : nullptr;
    RECT anchor{};
    if (header && localColumn >= 0 && Header_GetItemRect(header, localColumn, &anchor)) {
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
            const std::size_t column = snapshotColumnForView(window, header->iItem);
            if (column != (std::numeric_limits<std::size_t>::max)()) {
                PostMessageW(mainWindow, WM_APP_FILTER_COLUMN, static_cast<WPARAM>(column), 0);
            }
            return 0;
        }
    }
    if (window == pinnedSymbolList && message == WM_MOUSEWHEEL && symbolList) {
        SendMessageW(symbolList, message, wParam, lParam);
        PostMessageW(mainWindow, WM_APP_SYNC_TABLE_SCROLL,
            reinterpret_cast<WPARAM>(symbolList), 0);
        return 0;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (!syncingTableScroll && (window == symbolList || window == pinnedSymbolList) &&
        (message == WM_MOUSEWHEEL || message == WM_VSCROLL || message == WM_KEYDOWN)) {
        PostMessageW(mainWindow, WM_APP_SYNC_TABLE_SCROLL,
            reinterpret_cast<WPARAM>(window), 0);
    }
    return result;
}

bool hasUncommittedCellEdit() {
    return cellEditor && getText(cellEditor) != cellEditorOriginalValue;
}

bool hasPendingSymbolChanges() {
    return importedDataReady || !importedChangedCells.empty() || hasUncommittedCellEdit();
}

void updateExportButton() {
    if (exportButton) {
        EnableWindow(exportButton, !operationRunning.load() && !currentSnapshot.columns.empty());
    }
    if (applyButton) {
        EnableWindow(applyButton, !operationRunning.load() && hasPendingSymbolChanges());
    }
    if (browseExcelButton) {
        EnableWindow(browseExcelButton, !operationRunning.load());
    }
    if (showChangedSymbolsCheckbox) {
        const bool filterAvailable = importedDataReady || !importedChangedCells.empty();
        if (!filterAvailable && showChangedSymbolsOnly) {
            showChangedSymbolsOnly = false;
            SendMessageW(showChangedSymbolsCheckbox, BM_SETCHECK, BST_UNCHECKED, 0);
        }
        ShowWindow(showChangedSymbolsCheckbox, filterAvailable ? SW_SHOW : SW_HIDE);
        EnableWindow(showChangedSymbolsCheckbox,
            !operationRunning.load() && filterAvailable && !importedChangedCells.empty());
    }
}

LRESULT CALLBACK cellEditorSubclassProc(HWND window, UINT message, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
        const HWND editView = tableViewForSnapshotColumn(editingColumnIndex);
        finishCellEdit(wParam == VK_RETURN);
        if (editView) SetFocus(editView);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, cellEditorSubclassProc, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

void finishCellEdit(bool commit) {
    if (!cellEditor) return;

    HWND editor = cellEditor;
    const std::wstring value = getText(editor);
    const std::wstring oldValue = cellEditorOriginalValue;
    const std::size_t rowIndex = editingRowIndex;
    const std::size_t columnIndex = editingColumnIndex;
    cellEditor = nullptr;
    editingRowIndex = (std::numeric_limits<std::size_t>::max)();
    editingColumnIndex = (std::numeric_limits<std::size_t>::max)();
    cellEditorOriginalValue.clear();
    DestroyWindow(editor);

    if (!commit || value == oldValue || rowIndex >= currentSnapshot.rows.size() ||
        columnIndex >= currentSnapshot.rows[rowIndex].size()) {
        updateExportButton();
        return;
    }

    if (manualEditBaseline.rows.empty()) {
        manualEditBaseline = serverSnapshot.rows.empty() ? currentSnapshot : serverSnapshot;
    }
    currentSnapshot.rows[rowIndex][columnIndex] = value;
    importedChangedCells = findImportedChanges(manualEditBaseline, currentSnapshot);
    refreshVisibleRows();
    updateExportButton();

    std::size_t changedCells = 0;
    for (const auto& [symbol, columns] : importedChangedCells) {
        (void)symbol;
        changedCells += columns.size();
    }
    setStatus(L"Manual edit staged. Changed cells: " + std::to_wstring(changedCells) +
        L". Press Apply Changes to send the values to the selected server.");
}

void beginCellEdit(int visibleIndex, int columnIndex) {
    if (operationRunning.load() || visibleIndex < 0 || columnIndex < 0 || !symbolList) return;
    if (cellEditor) finishCellEdit(true);

    const std::size_t column = static_cast<std::size_t>(columnIndex);
    if (column >= currentSnapshot.columns.size() ||
        column >= currentSnapshot.sourceNames.size() ||
        !IsSymbolFieldEditable(currentSnapshot.sourceNames[column])) return;

    const std::size_t item = static_cast<std::size_t>(visibleIndex);
    if (item >= visibleRows.size()) return;
    const std::size_t row = visibleRows[item];
    if (row >= currentSnapshot.rows.size() || column >= currentSnapshot.rows[row].size()) return;

    if (manualEditBaseline.rows.empty()) {
        manualEditBaseline = serverSnapshot.rows.empty() ? currentSnapshot : serverSnapshot;
    }

    HWND view = tableViewForSnapshotColumn(column);
    const int localColumn = localColumnForSnapshot(view, column);
    if (!view || localColumn < 0) return;
    RECT cellRect{};
    if (!ListView_GetSubItemRect(view, visibleIndex, localColumn, LVIR_BOUNDS, &cellRect)) return;
    POINT corners[2]{{cellRect.left, cellRect.top}, {cellRect.right, cellRect.bottom}};
    MapWindowPoints(view, mainWindow, corners, 2);
    const int width = corners[1].x - corners[0].x;
    const int height = corners[1].y - corners[0].y;
    if (width <= 0 || height <= 0) return;

    editingRowIndex = row;
    editingColumnIndex = column;
    cellEditorOriginalValue = currentSnapshot.rows[row][column];
    cellEditor = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", cellEditorOriginalValue.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        corners[0].x, corners[0].y, width, height, mainWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CELL_EDITOR)), instanceHandle, nullptr);
    if (!cellEditor) {
        editingRowIndex = (std::numeric_limits<std::size_t>::max)();
        editingColumnIndex = (std::numeric_limits<std::size_t>::max)();
        cellEditorOriginalValue.clear();
        return;
    }

    SetWindowSubclass(cellEditor, cellEditorSubclassProc, 1, 0);
    if (tableFont) SendMessageW(cellEditor, WM_SETFONT, reinterpret_cast<WPARAM>(tableFont), TRUE);
    SetFocus(cellEditor);
    SendMessageW(cellEditor, EM_SETSEL, 0, -1);
}

void setBusy(bool busy) {
    if (connectButton) EnableWindow(connectButton, !busy);
    if (profileCombo) EnableWindow(profileCombo, !busy);
    if (loginEdit) EnableWindow(loginEdit, !busy);
    if (passwordEdit) EnableWindow(passwordEdit, !busy);
    if (excelPathEdit) EnableWindow(excelPathEdit, !busy);
    updateExportButton();
    if (symbolList) EnableWindow(symbolList, !busy);
    if (pinnedSymbolList) EnableWindow(pinnedSymbolList, !busy);
}

void clearTable(bool preserveBaseline = false) {
    if (cellEditor) finishCellEdit(false);
    currentSnapshot = {};
    if (!preserveBaseline) {
        serverSnapshot = {};
        manualEditBaseline = {};
    }
    importedChangedCells.clear();
    currentSymbolColumn = (std::numeric_limits<std::size_t>::max)();
    activeSortColumn = -1;
    activeSortAscending = true;
    columnFilters.clear();
    visibleRows.clear();
    importedDataReady = false;
    showChangedSymbolsOnly = false;
    if (showChangedSymbolsCheckbox) {
        SendMessageW(showChangedSymbolsCheckbox, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    excelFilePath.clear();
    if (excelPathEdit) SetWindowTextW(excelPathEdit, L"");
    updateExportButton();
    if (!symbolList) return;
    ListView_SetItemCountEx(symbolList, 0, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (pinnedSymbolList) {
        ListView_SetItemCountEx(pinnedSymbolList, 0, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    }
    for (HWND view : {pinnedSymbolList, symbolList}) {
        if (!view) continue;
        HWND header = ListView_GetHeader(view);
        const int columns = header ? Header_GetItemCount(header) : 0;
        for (int index = columns - 1; index >= 0; --index) {
            ListView_DeleteColumn(view, index);
        }
    }
    pinnedColumnIndices.clear();
    scrollableColumnIndices.clear();
    RECT client{};
    if (mainWindow && GetClientRect(mainWindow, &client)) {
        layoutTableForClient(client.right, client.bottom);
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
    pinnedColumnIndices.clear();
    scrollableColumnIndices.clear();
    for (const std::wstring_view pinnedName : {std::wstring_view(L"Symbol"), std::wstring_view(L"Path")}) {
        for (std::size_t index = 0; index < currentSnapshot.sourceNames.size(); ++index) {
            if (currentSnapshot.sourceNames[index] == pinnedName) {
                pinnedColumnIndices.push_back(index);
                break;
            }
        }
    }
    for (std::size_t index = 0; index < currentSnapshot.columns.size(); ++index) {
        if (std::find(pinnedColumnIndices.begin(), pinnedColumnIndices.end(), index) == pinnedColumnIndices.end()) {
            scrollableColumnIndices.push_back(index);
        }
    }

    auto insertColumn = [](HWND view, std::size_t localIndex, std::size_t snapshotIndex,
                           int width) {
        const std::wstring& name = currentSnapshot.columns[snapshotIndex];
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<LPWSTR>(name.c_str());
        column.iSubItem = static_cast<int>(localIndex);
        column.cx = width;
        ListView_InsertColumn(view, static_cast<int>(localIndex), &column);
        HWND header = ListView_GetHeader(view);
        HDITEMW headerItem{};
        headerItem.mask = HDI_FORMAT;
        if (header && Header_GetItem(header, static_cast<int>(localIndex), &headerItem)) {
            headerItem.fmt |= HDF_SPLITBUTTON;
            Header_SetItem(header, static_cast<int>(localIndex), &headerItem);
        }
    };

    for (std::size_t localIndex = 0; localIndex < pinnedColumnIndices.size(); ++localIndex) {
        const std::size_t snapshotIndex = pinnedColumnIndices[localIndex];
        const bool isPath = currentSnapshot.sourceNames[snapshotIndex] == L"Path";
        const int width = isPath ? pinnedPathColumnWidth : pinnedSymbolColumnWidth;
        insertColumn(pinnedSymbolList, localIndex, snapshotIndex, width);
    }
    for (std::size_t localIndex = 0; localIndex < scrollableColumnIndices.size(); ++localIndex) {
        const std::size_t snapshotIndex = scrollableColumnIndices[localIndex];
        insertColumn(symbolList, localIndex, snapshotIndex, 100);
    }
    RECT client{};
    if (mainWindow && GetClientRect(mainWindow, &client)) {
        layoutTableForClient(client.right, client.bottom);
    }
}

void updateTableSortIndicators() {
    for (HWND view : {pinnedSymbolList, symbolList}) {
        if (!view) continue;
        HWND header = ListView_GetHeader(view);
        if (!header) continue;
        const auto& columns = tableColumnsForView(view);
        for (std::size_t local = 0; local < columns.size(); ++local) {
            HDITEMW item{};
            item.mask = HDI_FORMAT;
            if (!Header_GetItem(header, static_cast<int>(local), &item)) continue;
            item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (static_cast<int>(columns[local]) == activeSortColumn) {
                item.fmt |= activeSortAscending ? HDF_SORTUP : HDF_SORTDOWN;
            }
            Header_SetItem(header, static_cast<int>(local), &item);
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
    serverSnapshot = currentSnapshot;
    manualEditBaseline = currentSnapshot;
    importedChangedCells.clear();
    currentSymbolColumn = findSymbolColumn(currentSnapshot);
    addTableColumns();
    refreshVisibleRows();
    updateExportButton();

    std::wstring firstLine = L"Connected to " + result->profile + L" (" + result->server +
        L"). Retrieved parameters for " + std::to_wstring(currentSnapshot.rows.size()) + L" symbols.";
    std::wstring secondLine = L"Parameters: " + std::to_wstring(currentSnapshot.columns.size()) +
        L". Double-click editable values; Failed symbol records: " +
        std::to_wstring(currentSnapshot.failedSymbols) + L".";
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
        if (inputColumnForMapping[index] == mappings.size() && mappings[index].sourceName != L"Path") {
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
            if (inputColumnForMapping[column] != mappings.size()) {
                row[column] = sourceRow[inputColumnForMapping[column]];
            }
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

    preserveImportedPathValues(result->snapshot, serverSnapshot);
    ChangedCellMap changedCells;
    if (!serverSnapshot.rows.empty()) {
        changedCells = findImportedChanges(serverSnapshot, result->snapshot);
    }
    clearTable(true);
    currentSnapshot = std::move(result->snapshot);
    manualEditBaseline = serverSnapshot.rows.empty() ? currentSnapshot : serverSnapshot;
    currentSymbolColumn = findSymbolColumn(currentSnapshot);
    importedChangedCells = std::move(changedCells);
    showChangedSymbolsOnly = false;
    if (showChangedSymbolsCheckbox) {
        SendMessageW(showChangedSymbolsCheckbox, BM_SETCHECK, BST_UNCHECKED, 0);
    }
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
        L" (light yellow). Press Apply Changes to send these values to the selected server.");
}

void showAbortPrompt();

void startApplyChanges() {
    if (cellEditor) finishCellEdit(true);
    if (!hasPendingSymbolChanges() || currentSnapshot.rows.empty() || operationRunning.load()) return;
    ConnectionProfile profile;
    if (!selectedProfile(profile)) {
        setStatus(L"Select a server profile before applying symbol changes.");
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
        setStatus(L"Cannot create the symbol update progress window.");
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
                result->error = L"Unexpected error while applying symbol changes: ";
                const std::string message = exception.what();
                result->error.append(message.begin(), message.end());
            } catch (...) {
                result->success = false;
                result->error = L"Unexpected error while applying symbol changes.";
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
        setStatus(L"Cannot start the symbol update worker thread.");
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
        MessageBoxW(mainWindow, result->error.c_str(), L"Symbol update failed", MB_OK | MB_ICONERROR);
        return;
    }

    const ApplyResult& applied = result->result;
    std::wstring summary = L"Updated: " + std::to_wstring(applied.applied) +
        L". Unchanged: " + std::to_wstring(applied.unchanged) +
        L". Failed: " + std::to_wstring(applied.failed) +
        L". Symbols not found and skipped: " + std::to_wstring(applied.skipped) +
        L". Not processed: " + std::to_wstring(applied.notProcessed) + L".";
    if (applied.aborted) summary += L" Update was aborted.";
    if (applied.restoreRequested) {
        summary += L" Restored: " + std::to_wstring(applied.restored) +
            L". Restore failures: " + std::to_wstring(applied.restoreFailed) + L".";
    }
    if (!applied.firstFailure.empty()) summary += L"\r\nFirst failure: " + applied.firstFailure;
    if (!applied.backupPath.empty()) summary += L"\r\nBackup: " + applied.backupPath;
    setStatus(summary);
    const bool completedWithoutErrors = !applied.aborted && !applied.restoreRequested &&
        applied.failed == 0 && applied.skipped == 0 && applied.notProcessed == 0 &&
        applied.restoreFailed == 0;
    if (completedWithoutErrors) {
        serverSnapshot = currentSnapshot;
        manualEditBaseline = currentSnapshot;
        importedChangedCells.clear();
        importedDataReady = false;
        refreshVisibleRows();
        updateExportButton();
    }
    MessageBoxW(mainWindow, summary.c_str(), L"Symbol update result", MB_OK |
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
            L"The update is paused. Confirm stops after the current symbol and keeps completed changes.\n"
            L"Confirm and Restore existing symbols reverts every change completed by this operation.",
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
        ABORT_PROMPT_CLASS, L"Abort Apply Changes",
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
        SetWindowTextW(applyProgressText, L"Applying changes resumed.");
    } else if (state.choice == ApplyChoice::StopAndRestore) {
        SetWindowTextW(applyProgressText, L"Stopping and restoring existing symbols...");
    } else {
        SetWindowTextW(applyProgressText, L"Stopping after the current symbol...");
    }
}

HWND addLabel(HWND parent, const wchar_t* text, int x, int y, int width) {
    HWND label = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                               x, y, width, 30, parent, nullptr, instanceHandle, nullptr);
    if (controlFont) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont), TRUE);
    return label;
}

void layoutResizableWindow(HWND window) {
    RECT client{};
    if (!GetClientRect(window, &client)) return;
    const int clientWidth = static_cast<int>(client.right);
    const int clientHeight = static_cast<int>(client.bottom);

    const bool narrowLayout = clientWidth < NARROW_LAYOUT_WIDTH;
    const int firstRowY = narrowLayout ? 10 : 14;
    const int secondRowY = narrowLayout ? 48 : 52;
    const int excelRowY = narrowLayout ? 86 : 52;

    if (narrowLayout) {
        const int fieldSpace = (std::max)(0, clientWidth - 231);
        const int serverWidth = fieldSpace * 40 / 100;
        const int loginWidth = fieldSpace * 25 / 100;
        const int passwordWidth = fieldSpace - serverWidth - loginWidth;
        const int serverX = 70;
        const int loginLabelX = serverX + serverWidth + 12;
        const int loginEditX = loginLabelX + 50;
        const int passwordLabelX = loginEditX + loginWidth + 12;
        const int passwordEditX = passwordLabelX + 75;

        if (serverLabel) MoveWindow(serverLabel, 12, firstRowY, 58, 30, TRUE);
        if (profileCombo) MoveWindow(profileCombo, serverX, firstRowY,
            serverWidth, 220, TRUE);
        if (loginLabel) MoveWindow(loginLabel, loginLabelX, firstRowY, 50, 30, TRUE);
        if (loginEdit) MoveWindow(loginEdit, loginEditX, firstRowY,
            loginWidth, 30, TRUE);
        if (passwordLabel) MoveWindow(passwordLabel, passwordLabelX, firstRowY, 75, 30, TRUE);
        if (passwordEdit) MoveWindow(passwordEdit, passwordEditX, firstRowY,
            passwordWidth, 30, TRUE);

        const int buttonGap = 8;
        const int buttonSpace = (std::max)(0, clientWidth - 24 - 2 * buttonGap);
        const int applyWidth = buttonSpace / 3;
        const int exportWidth = buttonSpace / 3;
        const int connectWidth = buttonSpace - applyWidth - exportWidth;
        int buttonX = 12;
        if (applyButton) MoveWindow(applyButton, buttonX, secondRowY,
            applyWidth, 30, TRUE);
        buttonX += applyWidth + buttonGap;
        if (exportButton) MoveWindow(exportButton, buttonX, secondRowY,
            exportWidth, 30, TRUE);
        buttonX += exportWidth + buttonGap;
        if (connectButton) MoveWindow(connectButton, buttonX, secondRowY,
            connectWidth, 30, TRUE);

        const int browseWidth = (std::clamp)(clientWidth * 14 / 100, 90, 170);
        const int checkboxWidth = (std::clamp)(clientWidth * 18 / 100, 160, 230);
        const int excelWidth = (std::max)(0,
            clientWidth - 70 - 10 - browseWidth - 10 - checkboxWidth - 12);
        const int browseX = 70 + excelWidth + 10;
        if (excelLabel) MoveWindow(excelLabel, 12, excelRowY, 58, 30, TRUE);
        if (excelPathEdit) MoveWindow(excelPathEdit, 70, excelRowY,
            excelWidth, 30, TRUE);
        if (browseExcelButton) MoveWindow(browseExcelButton, browseX, excelRowY,
            browseWidth, 30, TRUE);
        if (showChangedSymbolsCheckbox) {
            MoveWindow(showChangedSymbolsCheckbox, browseX + browseWidth + 10,
                excelRowY, checkboxWidth, 30, TRUE);
        }
    } else {
        const int flexSpace = clientWidth - 289;
        const int serverWidth = flexSpace * 300 / 1055;
        const int loginWidth = flexSpace * 125 / 1055;
        const int passwordWidth = flexSpace * 200 / 1055;
        const int applyWidth = flexSpace * 140 / 1055;
        const int exportWidth = flexSpace * 145 / 1055;
        const int connectWidth = flexSpace - serverWidth - loginWidth -
            passwordWidth - applyWidth - exportWidth;
        const int loginLabelX = 70 + serverWidth + 18;
        const int loginEditX = loginLabelX + 50;
        const int passwordLabelX = loginEditX + loginWidth + 17;
        const int passwordEditX = passwordLabelX + 75;
        const int applyX = passwordEditX + passwordWidth + 20;
        const int exportX = applyX + applyWidth + 20;
        const int connectX = exportX + exportWidth + 15;

        if (serverLabel) MoveWindow(serverLabel, 12, firstRowY, 58, 30, TRUE);
        if (profileCombo) MoveWindow(profileCombo, 70, firstRowY,
            serverWidth, 220, TRUE);
        if (loginLabel) MoveWindow(loginLabel, loginLabelX, firstRowY, 50, 30, TRUE);
        if (loginEdit) MoveWindow(loginEdit, loginEditX, firstRowY,
            loginWidth, 30, TRUE);
        if (passwordLabel) MoveWindow(passwordLabel, passwordLabelX, firstRowY, 75, 30, TRUE);
        if (passwordEdit) MoveWindow(passwordEdit, passwordEditX, firstRowY,
            passwordWidth, 30, TRUE);
        if (applyButton) MoveWindow(applyButton, applyX, firstRowY,
            applyWidth, 30, TRUE);
        if (exportButton) MoveWindow(exportButton, exportX, firstRowY,
            exportWidth, 30, TRUE);
        if (connectButton) MoveWindow(connectButton, connectX, firstRowY,
            connectWidth, 30, TRUE);

        const int widthDelta = clientWidth - 1344;
        const int excelWidth = 890 + widthDelta * 80 / 100;
        const int browseWidth = 145 + widthDelta * 8 / 100;
        const int checkboxWidth = 215 + widthDelta -
            (excelWidth - 890) - (browseWidth - 145);
        const int browseX = 80 + excelWidth;
        if (excelLabel) MoveWindow(excelLabel, 12, excelRowY, 58, 30, TRUE);
        if (excelPathEdit) MoveWindow(excelPathEdit, 70, excelRowY,
            excelWidth, 30, TRUE);
        if (browseExcelButton) MoveWindow(browseExcelButton, browseX, excelRowY,
            browseWidth, 30, TRUE);
        if (showChangedSymbolsCheckbox) {
            MoveWindow(showChangedSymbolsCheckbox, browseX + browseWidth + 10,
                excelRowY, checkboxWidth, 30, TRUE);
        }
    }

    layoutTableForClient(clientWidth, clientHeight);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) layoutResizableWindow(window);
        return 0;

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
        if (LOWORD(wParam) == IDC_CELL_EDITOR) {
            if (HIWORD(wParam) == EN_CHANGE) {
                updateExportButton();
            } else if (HIWORD(wParam) == EN_KILLFOCUS) {
                finishCellEdit(true);
            }
            return 0;
        }
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
        if (LOWORD(wParam) == IDC_SHOW_CHANGED_SYMBOLS && HIWORD(wParam) == BN_CLICKED) {
            showChangedSymbolsOnly = SendMessageW(showChangedSymbolsCheckbox,
                BM_GETCHECK, 0, 0) == BST_CHECKED;
            refreshVisibleRows();
            return 0;
        }
        if (LOWORD(wParam) == IDC_APPLY && HIWORD(wParam) == BN_CLICKED) {
            startApplyChanges();
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

    case WM_VSCROLL: {
        const HWND source = reinterpret_cast<HWND>(lParam);
        if (source == symbolList || source == pinnedSymbolList) {
            PostMessageW(window, WM_APP_SYNC_TABLE_SCROLL,
                reinterpret_cast<WPARAM>(source), 0);
        }
        break;
    }

    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (!header) break;
        HWND headerView = nullptr;
        if (pinnedSymbolList && header->hwndFrom == ListView_GetHeader(pinnedSymbolList)) {
            headerView = pinnedSymbolList;
        } else if (symbolList && header->hwndFrom == ListView_GetHeader(symbolList)) {
            headerView = symbolList;
        }
        if (headerView == pinnedSymbolList && header->code == HDN_TRACKW) {
            trackPinnedColumnWidth(*reinterpret_cast<NMHEADERW*>(lParam));
            return FALSE;
        }
        if (headerView == pinnedSymbolList && header->code == HDN_ENDTRACKW) {
            syncPinnedColumnWidthsFromHeader();
            RECT client{};
            if (GetClientRect(window, &client)) {
                layoutTableForClient(client.right, client.bottom);
            }
            return 0;
        }
        if (headerView && header->code == HDN_DROPDOWN) {
            const auto* dropdown = reinterpret_cast<const NMHEADERW*>(lParam);
            const std::size_t column = snapshotColumnForView(headerView, dropdown->iItem);
            if (column != (std::numeric_limits<std::size_t>::max)()) {
                PostMessageW(window, WM_APP_FILTER_COLUMN, static_cast<WPARAM>(column), 0);
            }
            return 0;
        }
        const HWND view = header->hwndFrom == pinnedSymbolList ? pinnedSymbolList :
            (header->hwndFrom == symbolList ? symbolList : nullptr);
        if (!view) break;
        if (header->code == LVN_ITEMCHANGED && !syncingTableSelection) {
            const auto* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
            if ((change->uChanged & LVIF_STATE) != 0 && change->iItem >= 0) {
                HWND other = view == symbolList ? pinnedSymbolList : symbolList;
                if (other) {
                    syncingTableSelection = true;
                    ListView_SetItemState(other, change->iItem, change->uNewState,
                        LVIS_SELECTED | LVIS_FOCUSED);
                    syncingTableSelection = false;
                }
            }
            return 0;
        }
        if (header->code == NM_DBLCLK) {
            const auto* click = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
            const std::size_t column = snapshotColumnForView(view, click->iSubItem);
            if (column != (std::numeric_limits<std::size_t>::max)()) {
                beginCellEdit(click->iItem, static_cast<int>(column));
            }
            return 0;
        }
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
                const std::size_t column = snapshotColumnForView(view, draw->iSubItem);
                if (column == (std::numeric_limits<std::size_t>::max)()) return CDRF_DODEFAULT;

                const std::wstring_view symbol = rowCellValue(currentSnapshot.rows[rowIndex], currentSymbolColumn);
                const auto changedSymbol = importedChangedCells.find(std::wstring(symbol));
                if (changedSymbol != importedChangedCells.end() &&
                    changedSymbol->second.contains(column)) {
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
                const std::size_t column = snapshotColumnForView(view, info->item.iSubItem);
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
            const std::size_t column = snapshotColumnForView(view, click->iSubItem);
            if (column != (std::numeric_limits<std::size_t>::max)() &&
                column < currentSnapshot.columns.size() &&
                !currentSnapshot.rows.empty()) {
                if (activeSortColumn == static_cast<int>(column)) {
                    activeSortAscending = !activeSortAscending;
                } else {
                    activeSortColumn = static_cast<int>(column);
                    activeSortAscending = true;
                }
                std::stable_sort(currentSnapshot.rows.begin(), currentSnapshot.rows.end(),
                    [ascending = activeSortAscending, column](const auto& left, const auto& right) {
                        const std::wstring_view leftValue = column < left.size()
                            ? std::wstring_view(left[column]) : std::wstring_view{};
                        const std::wstring_view rightValue = column < right.size()
                            ? std::wstring_view(right[column]) : std::wstring_view{};
                        if (leftValue.empty() != rightValue.empty()) return !leftValue.empty();
                        const int comparison = compareCellValues(leftValue, rightValue);
                        return ascending ? comparison < 0 : comparison > 0;
                    });
                refreshVisibleRows();
                updateTableSortIndicators();
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

    case WM_APP_SYNC_TABLE_SCROLL:
        syncTableScroll(reinterpret_cast<HWND>(wParam));
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
        if (pinnedSymbolList) RemoveWindowSubclass(pinnedSymbolList, symbolListSubclassProc, 1);
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
    HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, SINGLE_INSTANCE_MUTEX_NAME);
    if (!singleInstanceMutex) {
        MessageBoxW(nullptr, L"Unable to check whether MT5 Symbol Manager is already running.",
            L"MT5 Symbol Manager", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existingWindow = FindWindowW(L"MT5SymbolManagerMainWindow", nullptr);
        if (existingWindow) {
            if (IsIconic(existingWindow)) ShowWindow(existingWindow, SW_RESTORE);
            else ShowWindow(existingWindow, SW_SHOW);
            SetForegroundWindow(existingWindow);
        } else {
            MessageBoxW(nullptr, L"MT5 Symbol Manager is already running.",
                L"MT5 Symbol Manager", MB_OK | MB_ICONINFORMATION);
        }
        CloseHandle(singleInstanceMutex);
        return 0;
    }

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
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_THICKFRAME,
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
    tableFont = CreateFontW(-MulDiv(9, fontDpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    constexpr int top = 14;
    serverLabel = addLabel(mainWindow, L"Server:", 12, top, 58);
    profileCombo = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        70, top, 300, 220, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROFILE)), instance, nullptr);
    loginLabel = addLabel(mainWindow, L"Login:", 388, top, 50);
    loginEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL,
        438, top, 125, 30, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGIN)), instance, nullptr);
    passwordLabel = addLabel(mainWindow, L"Password:", 580, top, 75);
    passwordEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY | ES_PASSWORD | ES_AUTOHSCROLL,
        655, top, 200, 30, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PASSWORD)), instance, nullptr);
    applyButton = CreateWindowW(L"BUTTON", L"Apply Changes", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        875, top, 140, 30, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_APPLY)), instance, nullptr);
    exportButton = CreateWindowW(L"BUTTON", L"Export Excel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        1035, top, 145, 30, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXPORT)), instance, nullptr);
    connectButton = CreateWindowW(L"BUTTON", L"Connect", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        1195, top, 145, 30, mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CONNECT)), instance, nullptr);
    excelLabel = addLabel(mainWindow, L"Excel:", 12, 52, 58);
    excelPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
        70, 52, 890, 30, mainWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXCEL_PATH)), instance, nullptr);
    browseExcelButton = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        970, 52, 145, 30, mainWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE_EXCEL)), instance, nullptr);
    showChangedSymbolsCheckbox = CreateWindowW(L"BUTTON", L"Show changed symbols",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
        1125, 52, 215, 30, mainWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SHOW_CHANGED_SYMBOLS)), instance, nullptr);
    statusText = CreateWindowW(L"STATIC", L"Ready.", WS_CHILD | WS_VISIBLE | SS_LEFT,
        12, 90, 1328, 44, mainWindow, nullptr, instance, nullptr);
    pinnedSymbolList = CreateWindowW(WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
            LVS_SHOWSELALWAYS,
        12, 140, PINNED_SYMBOL_COLUMN_WIDTH + PINNED_PATH_COLUMN_WIDTH + GetSystemMetrics(SM_CXVSCROLL), 610,
        mainWindow, nullptr, instance, nullptr);
    symbolList = CreateWindowW(WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
        12 + PINNED_SYMBOL_COLUMN_WIDTH + PINNED_PATH_COLUMN_WIDTH, 140,
        1328 - PINNED_SYMBOL_COLUMN_WIDTH - PINNED_PATH_COLUMN_WIDTH, 610,
        mainWindow, nullptr, instance, nullptr);
    if (!pinnedSymbolList || !symbolList) return 1;
    ShowScrollBar(pinnedSymbolList, SB_VERT, FALSE);
    clipPinnedScrollBar();
    ListView_SetExtendedListViewStyle(pinnedSymbolList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    ListView_SetExtendedListViewStyle(symbolList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    SetWindowSubclass(pinnedSymbolList, symbolListSubclassProc, 1, 0);
    SetWindowSubclass(symbolList, symbolListSubclassProc, 1, 0);

    setControlTreeFont(mainWindow, controlFont);
    setControlTreeFont(pinnedSymbolList, tableFont);
    setControlTreeFont(symbolList, tableFont);
    const int scaledEditMargin = MulDiv(5, fontDpi, 96);
    const int editMargin = scaledEditMargin > 4 ? scaledEditMargin : 4;
    for (HWND edit : {loginEdit, passwordEdit, excelPathEdit}) {
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELONG(editMargin, editMargin));
        InvalidateRect(edit, nullptr, TRUE);
    }
    EnableWindow(exportButton, FALSE);
    EnableWindow(applyButton, FALSE);
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
    ReleaseMutex(singleInstanceMutex);
    CloseHandle(singleInstanceMutex);
    return static_cast<int>(message.wParam);
}
