#include "Connections.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>

namespace {

std::wstring trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring decodeUtf8(const std::string& bytes) {
    if (bytes.empty()) return {};
    int offset = bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF ? 3 : 0;
    const int length = static_cast<int>(bytes.size()) - offset;
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              bytes.data() + offset, length, nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(required, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        bytes.data() + offset, length, result.data(), required);
    return result;
}

void applyKey(ConnectionProfile& profile, const std::wstring& key,
              const std::wstring& value) {
    if (_wcsicmp(key.c_str(), L"server") == 0) profile.server = value;
    else if (_wcsicmp(key.c_str(), L"login") == 0) profile.login = value;
    else if (_wcsicmp(key.c_str(), L"password") == 0) profile.password = value;
}

} // namespace

std::wstring GetExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash);
    return path;
}

bool LoadConnectionProfiles(const std::wstring& path,
                           std::vector<ConnectionProfile>& profiles,
                           bool& created,
                           std::wstring& error) {
    profiles.clear();
    created = false;
    error.clear();

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            error = L"Cannot create connection profile file: " + path;
            return false;
        }
        output << "# MT5 Symbol Manager connection profiles.\n"
                  "# Add one section per server. Passwords are stored as plain text.\n"
                  "[Example MT5 server]\n"
                  "server=server:443\n"
                  "login=1\n"
                  "password=\n";
        output.close();
        if (!output) {
            error = L"Cannot write connection profile file: " + path;
            return false;
        }
        created = true;
        input.clear();
        input.open(path, std::ios::binary);
    }

    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() > 1024 * 1024) {
        error = L"Connection profile file is larger than 1 MiB: " + path;
        return false;
    }
    const std::wstring content = decodeUtf8(bytes);
    if (!bytes.empty() && content.empty()) {
        error = L"Connection profile file is not valid UTF-8: " + path;
        return false;
    }

    ConnectionProfile* current = nullptr;
    std::size_t start = 0;
    while (start <= content.size()) {
        const std::size_t end = content.find(L'\n', start);
        std::wstring line = trim(content.substr(start, end == std::wstring::npos
            ? std::wstring::npos : end - start));
        if (!line.empty() && line[0] != L'#' && line[0] != L';') {
            if (line.front() == L'[' && line.back() == L']') {
                const std::wstring name = trim(line.substr(1, line.size() - 2));
                if (!name.empty()) {
                    profiles.push_back({});
                    profiles.back().name = name;
                    current = &profiles.back();
                } else {
                    current = nullptr;
                }
            } else if (current) {
                const std::size_t equals = line.find(L'=');
                if (equals != std::wstring::npos) {
                    applyKey(*current, trim(line.substr(0, equals)), trim(line.substr(equals + 1)));
                }
            }
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }

    profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [](const ConnectionProfile& profile) {
        return profile.name.empty();
    }), profiles.end());
    return true;
}
