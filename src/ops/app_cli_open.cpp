// app_cli_open.cpp — See app_cli_open.h for the contract.
#include "app_cli_open.h"

#include "../common/path_utils.h"

#include <cwctype>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pulse::ops {
namespace {

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

std::wstring Stem(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// app exe (lower) -> its CLI ("" = none). File-system probes, so remember them.
std::mutex g_mutex;
std::unordered_map<std::wstring, std::wstring> g_cli_for;

// <root>\resources\app.asar.unpacked\build\cli\win32-x64\<name>.exe, where
// <name> is a prefix of the app's own file name (Photon Studio -> photon).
std::wstring FindCli(const std::wstring& app_exe) {
    if (app_exe.empty()) return {};
    const std::wstring key = Lower(app_exe);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (const auto found = g_cli_for.find(key); found != g_cli_for.end()) return found->second;
    }
    std::wstring cli;
    const std::wstring root = ParentOf(app_exe);
    if (!root.empty()) {
        // An Electron app: the asar is beside it, the CLI inside the unpacked part.
        const std::wstring asar = root + L"\\resources\\app.asar";
        const std::wstring dir = root + L"\\resources\\app.asar.unpacked\\build\\cli\\win32-x64";
        if (GetFileAttributesW(asar.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const std::wstring app_stem = Lower(Stem(app_exe));
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((dir + L"\\*.exe").c_str(), &data);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    const std::wstring cli_stem = Lower(Stem(data.cFileName));
                    // "photon" prefixes "photon studio"; a random helper does not.
                    if (cli_stem.empty() || app_stem.rfind(cli_stem, 0) != 0) continue;
                    cli = dir + L"\\" + data.cFileName;
                    break;
                } while (FindNextFileW(find, &data));
                FindClose(find);
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cli_for[key] = cli;
    return cli;
}

// {"path":"..."} as UTF-8 (the CLI reads the file as JSON).
bool WriteRequestFile(const std::wstring& path, const std::wstring& file) {
    std::wstring json = L"{\"path\":\"";
    for (const wchar_t c : file) {
        if (c == L'\\' || c == L'"') json.push_back(L'\\');
        json.push_back(c);
    }
    json += L"\"}";
    // UTF-16 -> UTF-8 (WideCharToMultiByte), no BOM.
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), static_cast<int>(json.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    std::vector<char> utf8(static_cast<size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, json.c_str(), static_cast<int>(json.size()), utf8.data(), bytes,
                        nullptr, nullptr);
    HANDLE out = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) &&
                    written == utf8.size();
    CloseHandle(out);
    return ok;
}

}  // namespace

bool AppHasBundledCli(const std::wstring& app_exe) { return !FindCli(app_exe).empty(); }

bool TryOpenWithBundledCli(const std::wstring& app_exe, const std::wstring& file) {
    const std::wstring cli = FindCli(app_exe);
    if (cli.empty() || file.empty()) return false;
    wchar_t temp_dir[MAX_PATH]{};
    if (!GetTempPathW(ARRAYSIZE(temp_dir), temp_dir)) return false;
    wchar_t request[MAX_PATH]{};
    if (!GetTempFileNameW(temp_dir, L"pcl", 0, request)) return false;
    bool accepted = false;
    if (WriteRequestFile(request, pulse::path::StripExtendedPathPrefix(file))) {
        std::wstring command = L"\"" + cli + L"\" execute session.open --input \"" +
                               std::wstring(request) + L"\" --show";
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(0);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &startup, &process)) {
            // The CLI hands the file to the running app and exits; a cold start
            // (Photon not running yet) takes the longest. Never wait forever.
            const DWORD waited = WaitForSingleObject(process.hProcess, 20000);
            if (waited == WAIT_OBJECT_0) {
                DWORD code = 1;
                accepted = GetExitCodeProcess(process.hProcess, &code) && code == 0;
            }
            if (process.hThread) CloseHandle(process.hThread);
            if (process.hProcess) CloseHandle(process.hProcess);
        }
    }
    DeleteFileW(request);
    return accepted;
}

}  // namespace pulse::ops
