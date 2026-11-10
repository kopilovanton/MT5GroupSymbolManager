#define NOMINMAX
#include "ExcelImport.h"

#include <windows.h>
#include "ZLib.h"
#include <shlwapi.h>
#include <xmllite.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t MaxWorkbookBytes = 256ull * 1024 * 1024;
constexpr std::uint32_t MaxArchiveEntries = 4096;
constexpr std::size_t MaxWorksheetRows = 1'048'576;
constexpr std::size_t MaxWorksheetColumns = 16'384;
constexpr std::size_t MaxCellCharacters = 32'767;
constexpr std::uint64_t MaxWorkbookPartBytes = 256ull * 1024 * 1024;

struct ZipEntry {
    std::string name;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t localHeaderOffset = 0;
};

template <typename T>
class ComOwner {
public:
    ComOwner() = default;
    explicit ComOwner(T* value) : value_(value) {}
    ~ComOwner() { if (value_) value_->Release(); }
    ComOwner(const ComOwner&) = delete;
    ComOwner& operator=(const ComOwner&) = delete;
    ComOwner(ComOwner&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ComOwner& operator=(ComOwner&& other) noexcept {
        if (this != &other) {
            if (value_) value_->Release();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    T* get() const { return value_; }
    T** put() { if (value_) { value_->Release(); value_ = nullptr; } return &value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
private:
    T* value_ = nullptr;
};

std::uint16_t read16(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0]) |
        static_cast<std::uint16_t>(data[1] << 8);
}

std::uint32_t read32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

bool safeArchiveName(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\' ||
        name.find(':') != std::string::npos || name.find('\\') != std::string::npos ||
        name.find('\0') != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char ch) { return ch >= 0x80; })) return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = name.find('/', start);
        const std::string_view component(name.data() + start,
            (end == std::string::npos ? name.size() : end) - start);
        if (component == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool worksheetPartName(const std::string& name) {
    constexpr std::string_view prefix = "xl/worksheets/";
    if (!name.starts_with(prefix) || !name.ends_with(".xml")) return false;
    const std::string_view fileName(name.data() + prefix.size(),
        name.size() - prefix.size() - std::string_view(".xml").size());
    return fileName.starts_with("sheet") &&
        fileName.find('/') == std::string_view::npos &&
        fileName.size() > 5 &&
        std::all_of(fileName.begin() + 5, fileName.end(),
            [](char ch) { return ch >= '0' && ch <= '9'; });
}

bool validateWorkbookArchive(const std::wstring& path,
                             std::vector<ZipEntry>& entries,
                             std::uint64_t& archiveSize,
                             std::wstring& error) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!input) {
        error = L"Cannot open the selected Excel workbook.";
        return false;
    }
    const std::streamoff end = input.tellg();
    if (end < 22 || static_cast<std::uint64_t>(end) > MaxWorkbookBytes) {
        error = L"The Excel workbook is empty or larger than 256 MiB.";
        return false;
    }
    archiveSize = static_cast<std::uint64_t>(end);
    const std::size_t size = static_cast<std::size_t>(archiveSize);
    const std::size_t tailSize = std::min<std::size_t>(size, 65557);
    std::vector<unsigned char> tail(tailSize);
    input.seekg(static_cast<std::streamoff>(size - tailSize));
    input.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (!input) {
        error = L"Cannot read the Excel workbook directory.";
        return false;
    }

    std::size_t eocd = std::string::npos;
    for (std::size_t pos = tail.size() - 22;; --pos) {
        if (read32(tail.data() + pos) == 0x06054B50 &&
            pos + 22 + read16(tail.data() + pos + 20) == tail.size()) {
            eocd = pos;
            break;
        }
        if (pos == 0) break;
    }
    if (eocd == std::string::npos) {
        error = L"The selected file is not a valid .xlsx workbook.";
        return false;
    }
    const unsigned char* endRecord = tail.data() + eocd;
    const std::uint16_t diskNumber = read16(endRecord + 4);
    const std::uint16_t directoryDisk = read16(endRecord + 6);
    const std::uint16_t entriesOnDisk = read16(endRecord + 8);
    const std::uint16_t entryCount = read16(endRecord + 10);
    const std::uint32_t directorySize = read32(endRecord + 12);
    const std::uint32_t directoryOffset = read32(endRecord + 16);
    if (diskNumber != 0 || directoryDisk != 0 || entriesOnDisk != entryCount ||
        entryCount == 0 || entryCount > MaxArchiveEntries ||
        static_cast<std::uint64_t>(directoryOffset) + directorySize > size ||
        static_cast<std::uint64_t>(directoryOffset) + directorySize > size - tailSize + eocd) {
        error = L"The Excel workbook has an invalid or unsupported ZIP directory.";
        return false;
    }

    input.clear();
    input.seekg(directoryOffset);
    entries.clear();
    entries.reserve(entryCount);
    for (std::uint16_t index = 0; index < entryCount; ++index) {
        unsigned char header[46]{};
        input.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!input || read32(header) != 0x02014B50) {
            error = L"The Excel workbook has a malformed ZIP directory.";
            return false;
        }
        const std::uint16_t nameLength = read16(header + 28);
        const std::uint16_t extraLength = read16(header + 30);
        const std::uint16_t commentLength = read16(header + 32);
        const std::uint64_t nextEntry = static_cast<std::uint64_t>(input.tellg()) +
            nameLength + extraLength + commentLength;
        if (nextEntry > static_cast<std::uint64_t>(directoryOffset) + directorySize) {
            error = L"The Excel workbook has a malformed ZIP directory.";
            return false;
        }

        ZipEntry entry;
        entry.flags = read16(header + 8);
        entry.method = read16(header + 10);
        entry.crc = read32(header + 16);
        entry.compressedSize = read32(header + 20);
        entry.uncompressedSize = read32(header + 24);
        entry.localHeaderOffset = read32(header + 42);
        if (entry.compressedSize == std::numeric_limits<std::uint32_t>::max() ||
            entry.uncompressedSize == std::numeric_limits<std::uint32_t>::max() ||
            entry.localHeaderOffset == std::numeric_limits<std::uint32_t>::max()) {
            error = L"This Excel workbook uses an unsupported ZIP64 entry.";
            return false;
        }
        entry.name.resize(nameLength);
        input.read(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        input.seekg(static_cast<std::streamoff>(extraLength) + commentLength, std::ios::cur);
        if (!input || !safeArchiveName(entry.name)) {
            error = L"The Excel workbook contains an unsafe ZIP path.";
            return false;
        }
        entries.push_back(std::move(entry));
    }
    if (std::none_of(entries.begin(), entries.end(), [](const ZipEntry& entry) {
            return worksheetPartName(entry.name);
        })) {
        error = L"The workbook's first sheet is missing. Import an Excel file exported by MT5 Group Symbol Manager.";
        return false;
    }
    return true;
}

bool readZipEntry(std::ifstream& archive, std::uint64_t archiveSize,
                  const ZipEntry& entry, std::string& contents,
                  std::wstring& error) {
    if ((entry.flags & 0x0001) != 0) {
        error = L"Encrypted Excel workbooks are not supported.";
        return false;
    }
    if (entry.method != 0 && entry.method != 8) {
        error = L"The workbook uses an unsupported ZIP compression method.";
        return false;
    }
    if (entry.compressedSize > MaxWorkbookPartBytes ||
        entry.uncompressedSize > MaxWorkbookPartBytes) {
        error = L"An Excel workbook part is larger than 256 MiB.";
        return false;
    }

    const std::uint64_t localOffset = entry.localHeaderOffset;
    if (localOffset + 30 > archiveSize) {
        error = L"The Excel workbook has a truncated ZIP entry.";
        return false;
    }
    unsigned char localHeader[30]{};
    archive.clear();
    archive.seekg(static_cast<std::streamoff>(localOffset));
    archive.read(reinterpret_cast<char*>(localHeader), sizeof(localHeader));
    if (!archive || read32(localHeader) != 0x04034B50 ||
        read16(localHeader + 8) != entry.method || (read16(localHeader + 6) & 0x0001) != 0) {
        error = L"The Excel workbook has a malformed ZIP entry header.";
        return false;
    }

    const std::uint16_t nameLength = read16(localHeader + 26);
    const std::uint16_t extraLength = read16(localHeader + 28);
    const std::uint64_t dataOffset = localOffset + sizeof(localHeader) + nameLength + extraLength;
    if (dataOffset + entry.compressedSize > archiveSize) {
        error = L"The Excel workbook has a truncated ZIP entry.";
        return false;
    }
    std::string localName(nameLength, '\0');
    archive.read(localName.data(), static_cast<std::streamsize>(localName.size()));
    if (!archive || localName != entry.name) {
        error = L"The Excel workbook's ZIP entry names do not match.";
        return false;
    }

    std::string compressed(entry.compressedSize, '\0');
    archive.seekg(static_cast<std::streamoff>(dataOffset));
    archive.read(compressed.data(), static_cast<std::streamsize>(compressed.size()));
    if (!archive) {
        error = L"Cannot read an Excel workbook part.";
        return false;
    }

    std::string output;
    if (entry.method == 0) {
        if (entry.compressedSize != entry.uncompressedSize) {
            error = L"The Excel workbook contains an invalid uncompressed ZIP entry.";
            return false;
        }
        output = std::move(compressed);
    } else {
        output.resize(entry.uncompressedSize);
        char emptyOutput = 0;
        SZlib::z_stream stream{};
        stream.next_in = compressed.data();
        stream.avail_in = entry.compressedSize;
        stream.next_out = output.empty() ? &emptyOutput : output.data();
        stream.avail_out = std::max<std::uint32_t>(entry.uncompressedSize, 1);
        if (SZlib::inflateInit2(&stream, -SZlib::MAX_WBITS) != SZlib::Z_OK) {
            error = L"Cannot initialize the Excel ZIP decompressor.";
            return false;
        }
        const int inflateResult = SZlib::inflate(&stream, SZlib::Z_FINISH);
        const std::uint64_t bytesWritten = stream.total_out;
        SZlib::inflateEnd(&stream);
        if (inflateResult != SZlib::Z_STREAM_END || bytesWritten != entry.uncompressedSize ||
            stream.total_in != entry.compressedSize) {
            error = L"Cannot decompress an Excel workbook part.";
            return false;
        }
    }

    const auto* bytes = reinterpret_cast<const BYTE*>(output.data());
    if (SZlib::crc32(0, bytes, static_cast<unsigned>(output.size())) != entry.crc) {
        error = L"An Excel workbook part failed its integrity check.";
        return false;
    }
    contents = std::move(output);
    return true;
}

std::wstring xmlAttribute(IXmlReader* reader, LPCWSTR name) {
    if (FAILED(reader->MoveToAttributeByName(name, nullptr))) return {};
    const wchar_t* value = nullptr;
    UINT length = 0;
    const HRESULT hr = reader->GetValue(&value, &length);
    std::wstring result;
    if (SUCCEEDED(hr) && value) result.assign(value, length);
    reader->MoveToElement();
    return result;
}

bool openXmlReader(std::string_view xml, ComOwner<IXmlReader>& reader,
                   ComOwner<IStream>& stream) {
    if (xml.size() > std::numeric_limits<UINT>::max()) return false;
    IStream* memoryStream = SHCreateMemStream(
        reinterpret_cast<const BYTE*>(xml.data()), static_cast<UINT>(xml.size()));
    if (!memoryStream) return false;
    stream = ComOwner<IStream>(memoryStream);
    HRESULT hr = S_OK;
    hr = CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.put()), nullptr);
    if (FAILED(hr)) return false;
    return SUCCEEDED(reader->SetInput(stream.get()));
}

bool readSharedStrings(std::string_view xml, std::vector<std::wstring>& strings,
                       std::wstring& error) {
    ComOwner<IXmlReader> reader;
    ComOwner<IStream> stream;
    if (!openXmlReader(xml, reader, stream)) {
        error = L"Cannot open the workbook's shared strings.";
        return false;
    }
    bool insideString = false;
    std::wstring current;
    XmlNodeType node{};
    HRESULT hr = S_OK;
    while ((hr = reader->Read(&node)) == S_OK) {
        const wchar_t* localName = nullptr;
        UINT nameLength = 0;
        reader->GetLocalName(&localName, &nameLength);
        const std::wstring_view name(localName ? localName : L"", nameLength);
        if (node == XmlNodeType_Element && name == L"si") {
            insideString = true;
            current.clear();
        } else if ((node == XmlNodeType_Text || node == XmlNodeType_CDATA ||
                    node == XmlNodeType_Whitespace) && insideString) {
            const wchar_t* value = nullptr;
            UINT length = 0;
            if (SUCCEEDED(reader->GetValue(&value, &length)) && value) current.append(value, length);
        } else if (node == XmlNodeType_EndElement && name == L"si" && insideString) {
            strings.push_back(std::move(current));
            insideString = false;
            if (strings.size() > MaxWorksheetRows * 10) {
                error = L"The workbook has too many shared strings.";
                return false;
            }
        }
    }
    if (FAILED(hr)) {
        error = L"The workbook's shared strings XML is malformed.";
        return false;
    }
    return true;
}

bool parseCellReference(const std::wstring& reference,
                        std::size_t& row, std::size_t& column) {
    column = 0;
    row = 0;
    std::size_t offset = 0;
    while (offset < reference.size() && reference[offset] >= L'A' && reference[offset] <= L'Z') {
        const std::size_t digit = static_cast<std::size_t>(reference[offset] - L'A' + 1);
        if (column > (MaxWorksheetColumns - digit) / 26) return false;
        column = column * 26 + digit;
        ++offset;
    }
    if (offset == 0 || offset == reference.size()) return false;
    column -= 1;
    for (; offset < reference.size(); ++offset) {
        if (reference[offset] < L'0' || reference[offset] > L'9') return false;
        const std::size_t digit = static_cast<std::size_t>(reference[offset] - L'0');
        if (row > (MaxWorksheetRows - digit) / 10) return false;
        row = row * 10 + digit;
    }
    if (row == 0 || row > MaxWorksheetRows || column >= MaxWorksheetColumns) return false;
    row -= 1;
    return true;
}

bool readWorksheet(std::string_view xml,
                   const std::vector<std::wstring>& sharedStrings,
                   SymbolSnapshot& snapshot,
                   std::wstring& error) {
    ComOwner<IXmlReader> reader;
    ComOwner<IStream> stream;
    if (!openXmlReader(xml, reader, stream)) {
        error = L"Cannot open the workbook's first worksheet.";
        return false;
    }

    std::size_t currentRow = 0;
    std::size_t currentColumn = 0;
    std::wstring cellType;
    std::wstring cellValue;
    bool inCell = false;
    bool inValue = false;
    snapshot = {};
    XmlNodeType node{};
    HRESULT hr = S_OK;
    while ((hr = reader->Read(&node)) == S_OK) {
        const wchar_t* localName = nullptr;
        UINT nameLength = 0;
        reader->GetLocalName(&localName, &nameLength);
        const std::wstring_view name(localName ? localName : L"", nameLength);

        if (node == XmlNodeType_Element && name == L"row") {
            const std::wstring rowAttribute = xmlAttribute(reader.get(), L"r");
            if (!rowAttribute.empty()) {
                wchar_t* end = nullptr;
                const unsigned long parsed = std::wcstoul(rowAttribute.c_str(), &end, 10);
                if (!end || *end != L'\0' || parsed == 0 || parsed > MaxWorksheetRows) {
                    error = L"The worksheet contains an invalid row number.";
                    return false;
                }
                currentRow = static_cast<std::size_t>(parsed - 1);
            } else if (currentRow != 0 || !snapshot.rows.empty()) {
                currentRow = std::max(currentRow, snapshot.rows.size());
            }
            if (snapshot.rows.size() <= currentRow) snapshot.rows.resize(currentRow + 1);
        } else if (node == XmlNodeType_Element && name == L"c") {
            const std::wstring reference = xmlAttribute(reader.get(), L"r");
            std::size_t rowIndex = 0;
            if (!parseCellReference(reference, rowIndex, currentColumn)) {
                error = L"The worksheet contains an invalid cell reference.";
                return false;
            }
            currentRow = rowIndex;
            if (snapshot.rows.size() <= currentRow) snapshot.rows.resize(currentRow + 1);
            cellType = xmlAttribute(reader.get(), L"t");
            cellValue.clear();
            inValue = false;
            inCell = true;
        } else if (node == XmlNodeType_Element && inCell &&
                   (name == L"v" || name == L"t")) {
            inValue = true;
        } else if ((node == XmlNodeType_Text || node == XmlNodeType_CDATA ||
                    node == XmlNodeType_Whitespace) &&
                   inCell && inValue) {
            const wchar_t* value = nullptr;
            UINT length = 0;
            if (SUCCEEDED(reader->GetValue(&value, &length)) && value) cellValue.append(value, length);
            if (cellValue.size() > MaxCellCharacters) {
                error = L"A worksheet cell exceeds Excel's 32,767-character limit.";
                return false;
            }
        } else if (node == XmlNodeType_EndElement && inCell &&
                   (name == L"v" || name == L"t")) {
            inValue = false;
        } else if (node == XmlNodeType_EndElement && inCell && name == L"c") {
            if (cellType == L"s") {
                wchar_t* end = nullptr;
                const unsigned long long index = std::wcstoull(cellValue.c_str(), &end, 10);
                if (!end || end == cellValue.c_str() || *end != L'\0' || index >= sharedStrings.size()) {
                    error = L"The worksheet refers to a missing shared string.";
                    return false;
                }
                cellValue = sharedStrings[static_cast<std::size_t>(index)];
            } else if (cellType == L"b") {
                cellValue = cellValue == L"1" ? L"TRUE" : L"FALSE";
            }
            auto& row = snapshot.rows[currentRow];
            if (row.size() <= currentColumn) row.resize(currentColumn + 1);
            row[currentColumn] = std::move(cellValue);
            inCell = false;
            inValue = false;
        }
    }
    if (FAILED(hr)) {
        error = L"The worksheet XML is malformed.";
        return false;
    }
    const auto rowIsEmpty = [](const std::vector<std::wstring>& row) {
        return std::all_of(row.begin(), row.end(),
            [](const std::wstring& value) { return value.empty(); });
    };
    while (!snapshot.rows.empty() && rowIsEmpty(snapshot.rows.back())) {
        snapshot.rows.pop_back();
    }
    const auto firstNonEmptyRow = std::find_if(snapshot.rows.begin(), snapshot.rows.end(),
        [&](const std::vector<std::wstring>& row) { return !rowIsEmpty(row); });
    snapshot.rows.erase(snapshot.rows.begin(), firstNonEmptyRow);
    if (snapshot.rows.empty()) {
        error = L"The worksheet contains no data.";
        return false;
    }
    snapshot.columns = std::move(snapshot.rows.front());
    snapshot.rows.erase(snapshot.rows.begin());
    if (snapshot.columns.empty() || snapshot.rows.empty()) {
        error = L"The worksheet must include a header row and at least one symbol row.";
        return false;
    }
    for (auto& row : snapshot.rows) {
        if (row.size() > snapshot.columns.size() &&
            std::any_of(row.begin() + static_cast<std::ptrdiff_t>(snapshot.columns.size()), row.end(),
                        [](const std::wstring& value) { return !value.empty(); })) {
            error = L"The worksheet contains data to the right of the header columns.";
            return false;
        }
        row.resize(snapshot.columns.size());
    }
    return true;
}

} // namespace

bool ImportSnapshotFromXlsx(const std::wstring& filePath,
                            SymbolSnapshot& snapshot,
                            std::wstring& error) {
    error.clear();
    snapshot = {};
    std::vector<ZipEntry> entries;
    std::uint64_t archiveSize = 0;
    if (!validateWorkbookArchive(filePath, entries, archiveSize, error)) return false;

    std::ifstream archive(std::filesystem::path(filePath), std::ios::binary);
    if (!archive) {
        error = L"Cannot reopen the selected Excel workbook.";
        return false;
    }

    std::vector<std::pair<std::string, std::string>> worksheetParts;
    std::string sharedStringsXml;
    bool hasSharedStrings = false;
    for (const ZipEntry& entry : entries) {
        const bool isWorksheet = worksheetPartName(entry.name);
        const bool isSharedStrings = entry.name == "xl/sharedStrings.xml";
        if (!isWorksheet && !isSharedStrings) continue;

        std::string contents;
        if (!readZipEntry(archive, archiveSize, entry, contents, error)) return false;
        if (isWorksheet) worksheetParts.emplace_back(entry.name, std::move(contents));
        else {
            sharedStringsXml = std::move(contents);
            hasSharedStrings = true;
        }
    }
    if (worksheetParts.empty()) {
        error = L"The workbook's first worksheet is empty or could not be read.";
        return false;
    }

    // Excel workbooks can contain an empty first tab before the tab with exported data.
    // Try worksheet files in natural sheet-number order and use the first tab with a table.
    const auto sheetNumber = [](const std::string& path) {
        const std::size_t nameStart = path.find_last_of('/') + 1;
        const std::size_t numberStart = path.find("sheet", nameStart);
        if (numberStart == std::string::npos) return std::numeric_limits<unsigned long long>::max();
        const std::size_t firstDigit = numberStart + 5;
        std::size_t end = firstDigit;
        while (end < path.size() && path[end] >= '0' && path[end] <= '9') ++end;
        if (end == firstDigit) return std::numeric_limits<unsigned long long>::max();
        try {
            return std::stoull(path.substr(firstDigit, end - firstDigit));
        } catch (...) {
            return std::numeric_limits<unsigned long long>::max();
        }
    };
    std::stable_sort(worksheetParts.begin(), worksheetParts.end(),
        [&](const auto& left, const auto& right) {
            return sheetNumber(left.first) < sheetNumber(right.first);
        });

    std::vector<std::wstring> sharedStrings;
    if (hasSharedStrings && !readSharedStrings(sharedStringsXml, sharedStrings, error)) {
        return false;
    }

    std::wstring lastEmptySheetError = L"The worksheet contains no data.";
    for (const auto& [path, worksheetXml] : worksheetParts) {
        (void)path;
        SymbolSnapshot candidate;
        std::wstring candidateError;
        if (readWorksheet(worksheetXml, sharedStrings, candidate, candidateError)) {
            snapshot = std::move(candidate);
            return true;
        }
        if (candidateError == L"The worksheet contains no data." ||
            candidateError == L"The worksheet must include a header row and at least one symbol row.") {
            lastEmptySheetError = std::move(candidateError);
            continue;
        }
        error = std::move(candidateError);
        return false;
    }
    error = std::move(lastEmptySheetError);
    return false;
}
