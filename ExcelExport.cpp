#include "ExcelExport.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t MaxExcelRows = 1'048'576;
constexpr std::size_t MaxExcelColumns = 16'384;
constexpr std::size_t MaxExcelCellCharacters = 32'767;

struct ZipEntry {
    std::string name;
    std::string content;
    std::uint32_t crc = 0;
    std::uint32_t localHeaderOffset = 0;
};

std::string toUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("Text is too long to encode as UTF-8.");
    }

    const int wideLength = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("Cannot encode text as UTF-8.");

    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), wideLength,
            result.data(), required, nullptr, nullptr) != required) {
        throw std::runtime_error("Cannot encode text as UTF-8.");
    }
    return result;
}

std::string xmlText(std::wstring_view value) {
    std::wstring valid;
    valid.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::uint32_t ch = static_cast<std::uint16_t>(value[index]);
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (index + 1 < value.size()) {
                const std::uint32_t low = static_cast<std::uint16_t>(value[index + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    valid.push_back(value[index]);
                    valid.push_back(value[++index]);
                }
            }
            continue;
        }
        if (ch >= 0xDC00 && ch <= 0xDFFF) continue;
        if (ch == 0x9 || ch == 0xA || ch == 0xD ||
            (ch >= 0x20 && ch <= 0xD7FF) || (ch >= 0xE000 && ch <= 0xFFFD)) {
            valid.push_back(value[index]);
        }
    }

    const std::string utf8 = toUtf8(valid);
    std::string escaped;
    escaped.reserve(utf8.size());
    for (const char byte : utf8) {
        switch (byte) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&apos;"; break;
        default: escaped.push_back(byte); break;
        }
    }
    return escaped;
}

bool numericForExcel(std::wstring_view value) {
    if (value.empty()) return false;
    const std::wstring copy(value);
    wchar_t* end = nullptr;
    errno = 0;
    const double parsed = std::wcstod(copy.c_str(), &end);
    if (end == copy.c_str() || end != copy.c_str() + copy.size() ||
        errno == ERANGE || !std::isfinite(parsed)) {
        return false;
    }

    std::size_t significantDigits = 0;
    bool firstNonZero = false;
    for (const wchar_t ch : value) {
        if (ch == L'e' || ch == L'E') break;
        if (ch < L'0' || ch > L'9') continue;
        if (ch != L'0') firstNonZero = true;
        if (firstNonZero) ++significantDigits;
    }
    return significantDigits <= 15;
}

std::string columnName(std::size_t column) {
    std::string result;
    for (++column; column != 0; column = (column - 1) / 26) {
        result.insert(result.begin(), static_cast<char>('A' + ((column - 1) % 26)));
    }
    return result;
}

std::string cellReference(std::size_t column, std::size_t row) {
    return columnName(column) + std::to_string(row);
}

void appendInlineCell(std::string& xml, std::string_view reference,
                      std::wstring_view value) {
    xml += "<c r=\"";
    xml.append(reference);
    xml += "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">";
    xml += xmlText(value);
    xml += "</t></is></c>";
}

void appendValueCell(std::string& xml, std::string_view reference,
                     std::wstring_view value) {
    if (numericForExcel(value)) {
        xml += "<c r=\"";
        xml.append(reference);
        xml += "\"><v>";
        xml += toUtf8(value);
        xml += "</v></c>";
    } else {
        appendInlineCell(xml, reference, value);
    }
}

bool stringField(std::wstring_view sourceName) {
    static constexpr std::wstring_view fields[] = {
        L"Symbol", L"Path", L"ISIN", L"Description", L"International", L"Basis",
        L"Source", L"Page", L"CurrencyBase", L"CurrencyProfit", L"CurrencyMargin",
        L"Category", L"Exchange", L"CFI", L"Country"
    };
    return std::find(std::begin(fields), std::end(fields), sourceName) != std::end(fields);
}

std::string worksheetXml(const SymbolSnapshot& snapshot) {
    const std::size_t lastRow = snapshot.rows.size() + 1;
    const std::string lastColumnName = columnName(snapshot.columns.size() - 1);
    const std::string lastCell = lastColumnName + std::to_string(lastRow);

    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
    xml += "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
    xml += "<dimension ref=\"A1:" + lastCell + "\"/>";
    xml += "<sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>";
    xml += "<sheetFormatPr defaultRowHeight=\"15\"/><cols>";
    for (std::size_t column = 0; column < snapshot.columns.size(); ++column) {
        const std::size_t width = std::clamp<std::size_t>(snapshot.columns[column].size() + 3, 14, 58);
        const std::size_t excelColumn = column + 1;
        xml += "<col min=\"" + std::to_string(excelColumn) + "\" max=\"" +
            std::to_string(excelColumn) + "\" width=\"" + std::to_string(width) +
            "\" customWidth=\"1\"/>";
    }
    xml += "</cols><sheetData><row r=\"1\">";
    for (std::size_t column = 0; column < snapshot.columns.size(); ++column) {
        appendInlineCell(xml, cellReference(column, 1), snapshot.columns[column]);
    }
    xml += "</row>";

    for (std::size_t row = 0; row < snapshot.rows.size(); ++row) {
        const std::size_t excelRow = row + 2;
        xml += "<row r=\"" + std::to_string(excelRow) + "\">";
        for (std::size_t column = 0; column < snapshot.columns.size(); ++column) {
            const std::string reference = cellReference(column, excelRow);
            if (column < snapshot.rows[row].size()) {
                if (column < snapshot.sourceNames.size() && stringField(snapshot.sourceNames[column])) {
                    appendInlineCell(xml, reference, snapshot.rows[row][column]);
                } else {
                    appendValueCell(xml, reference, snapshot.rows[row][column]);
                }
            } else {
                appendInlineCell(xml, reference, L"");
            }
        }
        xml += "</row>";
    }
    xml += "</sheetData><autoFilter ref=\"A1:" + lastCell + "\"/></worksheet>";
    return xml;
}

std::uint32_t crc32(std::string_view value) {
    std::uint32_t crc = 0xFFFFFFFF;
    for (const unsigned char byte : value) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool writeBytes(std::ofstream& file, const void* data, std::size_t size) {
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(file);
}

bool write16(std::ofstream& file, std::uint16_t value) {
    const unsigned char bytes[] = {
        static_cast<unsigned char>(value & 0xFF),
        static_cast<unsigned char>((value >> 8) & 0xFF)
    };
    return writeBytes(file, bytes, sizeof(bytes));
}

bool write32(std::ofstream& file, std::uint32_t value) {
    const unsigned char bytes[] = {
        static_cast<unsigned char>(value & 0xFF),
        static_cast<unsigned char>((value >> 8) & 0xFF),
        static_cast<unsigned char>((value >> 16) & 0xFF),
        static_cast<unsigned char>((value >> 24) & 0xFF)
    };
    return writeBytes(file, bytes, sizeof(bytes));
}

bool writeZip(std::ofstream& file, std::vector<ZipEntry>& entries) {
    for (ZipEntry& entry : entries) {
        if (entry.name.size() > std::numeric_limits<std::uint16_t>::max() ||
            entry.content.size() > std::numeric_limits<std::uint32_t>::max()) return false;
        const std::streamoff offset = file.tellp();
        if (offset < 0 || static_cast<std::uint64_t>(offset) > std::numeric_limits<std::uint32_t>::max()) return false;
        entry.localHeaderOffset = static_cast<std::uint32_t>(offset);
        entry.crc = crc32(entry.content);

        if (!write32(file, 0x04034B50) || !write16(file, 20) || !write16(file, 0) ||
            !write16(file, 0) || !write16(file, 0) || !write16(file, 0x0021) ||
            !write32(file, entry.crc) || !write32(file, static_cast<std::uint32_t>(entry.content.size())) ||
            !write32(file, static_cast<std::uint32_t>(entry.content.size())) ||
            !write16(file, static_cast<std::uint16_t>(entry.name.size())) || !write16(file, 0) ||
            !writeBytes(file, entry.name.data(), entry.name.size()) ||
            !writeBytes(file, entry.content.data(), entry.content.size())) return false;
    }

    const std::streamoff directoryOffset = file.tellp();
    if (directoryOffset < 0 || static_cast<std::uint64_t>(directoryOffset) > std::numeric_limits<std::uint32_t>::max() ||
        entries.size() > std::numeric_limits<std::uint16_t>::max()) return false;
    for (const ZipEntry& entry : entries) {
        if (!write32(file, 0x02014B50) || !write16(file, 20) || !write16(file, 20) ||
            !write16(file, 0) || !write16(file, 0) || !write16(file, 0) || !write16(file, 0x0021) ||
            !write32(file, entry.crc) || !write32(file, static_cast<std::uint32_t>(entry.content.size())) ||
            !write32(file, static_cast<std::uint32_t>(entry.content.size())) ||
            !write16(file, static_cast<std::uint16_t>(entry.name.size())) || !write16(file, 0) ||
            !write16(file, 0) || !write16(file, 0) || !write16(file, 0) || !write32(file, 0) ||
            !write32(file, entry.localHeaderOffset) ||
            !writeBytes(file, entry.name.data(), entry.name.size())) return false;
    }

    const std::streamoff endOffset = file.tellp();
    if (endOffset < directoryOffset || static_cast<std::uint64_t>(endOffset) > std::numeric_limits<std::uint32_t>::max()) return false;
    const std::uint32_t directorySize = static_cast<std::uint32_t>(endOffset - directoryOffset);
    return write32(file, 0x06054B50) && write16(file, 0) && write16(file, 0) &&
        write16(file, static_cast<std::uint16_t>(entries.size())) &&
        write16(file, static_cast<std::uint16_t>(entries.size())) &&
        write32(file, directorySize) && write32(file, static_cast<std::uint32_t>(directoryOffset)) &&
        write16(file, 0);
}

std::vector<ZipEntry> makeWorkbookParts(const SymbolSnapshot& snapshot) {
    static const std::string contentTypes =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "</Types>";
    static const std::string rootRelationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>";
    static const std::string workbook =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<bookViews><workbookView activeTab=\"0\"/></bookViews>"
        "<sheets><sheet name=\"Symbols\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
    static const std::string workbookRelationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "</Relationships>";
    static const std::string styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
        "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
        "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>";

    return {
        {"[Content_Types].xml", contentTypes},
        {"_rels/.rels", rootRelationships},
        {"xl/workbook.xml", workbook},
        {"xl/_rels/workbook.xml.rels", workbookRelationships},
        {"xl/styles.xml", styles},
        {"xl/worksheets/sheet1.xml", worksheetXml(snapshot)}
    };
}

} // namespace

bool ExportSnapshotToXlsx(const SymbolSnapshot& snapshot,
                          const std::wstring& filePath,
                          std::wstring& error) {
    error.clear();
    if (snapshot.columns.empty()) {
        error = L"There is no symbol data to export.";
        return false;
    }
    if (snapshot.columns.size() > MaxExcelColumns || snapshot.rows.size() + 1 > MaxExcelRows) {
        error = L"The symbol data exceeds Excel's row or column limit.";
        return false;
    }
    for (const std::wstring& column : snapshot.columns) {
        if (column.size() > MaxExcelCellCharacters) {
            error = L"A column heading exceeds Excel's 32,767-character cell limit.";
            return false;
        }
    }
    for (const auto& row : snapshot.rows) {
        for (const std::wstring& value : row) {
            if (value.size() > MaxExcelCellCharacters) {
                error = L"A cell value exceeds Excel's 32,767-character cell limit.";
                return false;
            }
        }
    }

    const std::wstring temporaryPath = filePath + L".tmp";
    try {
        std::vector<ZipEntry> entries = makeWorkbookParts(snapshot);
        std::ofstream file(std::filesystem::path(temporaryPath), std::ios::binary | std::ios::trunc);
        if (!file) {
            error = L"Cannot create the Excel file: " + temporaryPath;
            return false;
        }
        if (!writeZip(file, entries)) {
            file.close();
            DeleteFileW(temporaryPath.c_str());
            error = L"Failed while writing the Excel workbook.";
            return false;
        }
        file.close();
        if (!file) {
            DeleteFileW(temporaryPath.c_str());
            error = L"Failed to finish writing the Excel workbook.";
            return false;
        }
        if (!MoveFileExW(temporaryPath.c_str(), filePath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD code = GetLastError();
            DeleteFileW(temporaryPath.c_str());
            error = L"Cannot move the completed workbook to the selected path (Windows error " +
                std::to_wstring(code) + L").";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        DeleteFileW(temporaryPath.c_str());
        error = L"Excel export failed: ";
        const std::string message = exception.what();
        error.append(message.begin(), message.end());
        return false;
    } catch (...) {
        DeleteFileW(temporaryPath.c_str());
        error = L"Excel export failed because of an unexpected error.";
        return false;
    }
}
