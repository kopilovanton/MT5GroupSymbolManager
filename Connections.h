#pragma once

#include <string>
#include <vector>

struct ConnectionProfile {
    std::wstring name;
    std::wstring server;
    std::wstring login;
    std::wstring password;
};

std::wstring GetExecutableDirectory();
bool LoadConnectionProfiles(const std::wstring& path,
                           std::vector<ConnectionProfile>& profiles,
                           bool& created,
                           std::wstring& error);
