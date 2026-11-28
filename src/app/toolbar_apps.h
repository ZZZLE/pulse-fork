// toolbar_apps.h — programs pinned to the toolbar (自定义工具栏 → 固定的软件).
//
// A pinned app is one more kind of toolbar pin: it sits after the pinned
// commands, opens the selection on click and takes dropped files the way a
// program row in the list does (OpsManager::OpenProgramIn with
// DropLaunchArguments). Which file types it opens comes from the system's
// 打开方式 data: SHAssocEnumHandlers plus the type's default program.
//
// Everything that touches the registry, COM or the program files runs on the
// caller's I/O worker (WorkerPool::EnqueueIo); the UI thread reads caches and
// gets an InvalidateRect when a result lands. The pure helpers at the top are self-tested.
#pragma once
#include "../common/menu_icon.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

constexpr size_t kMaxToolbarApps = 8;

// ---- Pure helpers -----------------------------------------------------------

inline std::wstring ToolbarLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

inline std::wstring ToolbarFileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// ".dwg" (lower case) for a file name or path; "" when it has none.
inline std::wstring ToolbarFileExt(const std::wstring& path) {
    const std::wstring name = ToolbarFileName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return ToolbarLower(name.substr(dot));
}

// Drops a leading \\?\ (and \\?\UNC\ -> \\) so paths compare as the shell
// reports them.
inline std::wstring ToolbarPlainPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + path.substr(8);
    if (path.rfind(L"\\\\?\\", 0) == 0) return path.substr(4);
    return path;
}

// True when `exe` is one of the type's programs: the same path, or the same
// file name (a handler registered from another install folder of the same
// program, or reported without its folder).
inline bool ProgramListHas(const std::vector<std::wstring>& programs, const std::wstring& exe) {
    if (exe.empty()) return false;
    const std::wstring path = ToolbarLower(ToolbarPlainPath(exe));
    const std::wstring name = ToolbarFileName(path);
    for (const auto& program : programs) {
        const std::wstring p = ToolbarLower(ToolbarPlainPath(program));
        if (p == path || (!name.empty() && ToolbarFileName(p) == name)) return true;
    }
    return false;
}

// A separator follows this pinned app (AppPrefs::toolbar_app_breaks).
inline bool ToolbarAppBreakAfter(const std::vector<std::wstring>& breaks, const std::wstring& app) {
    const std::wstring key = ToolbarLower(app);
    for (const auto& b : breaks)
        if (ToolbarLower(b) == key) return true;
    return false;
}

// Appends a program to the pinned list unless it is already there (same
// path, case-insensitive) or the list is full.
inline bool AddToolbarAppPath(std::vector<std::wstring>& apps, const std::wstring& path) {
    if (path.empty() || apps.size() >= kMaxToolbarApps) return false;
    const std::wstring key = ToolbarLower(ToolbarPlainPath(path));
    for (const auto& app : apps)
        if (ToolbarLower(ToolbarPlainPath(app)) == key) return false;
    apps.push_back(path);
    return true;
}

// ---- 选中时的「打开」按钮 (AppPrefs::toolbar_open_apps) ----------------------
//
// The toolbar's open button starts a program for the selected files. Which one
// is remembered per file type, Pulse-only (the Windows association is never
// touched): an entry is "ext=exe". No entry: the type's system default.

inline std::wstring ToolbarOpenAppFor(const std::vector<std::wstring>& entries, const std::wstring& ext) {
    const std::wstring key = ToolbarLower(ext);
    if (key.empty()) return {};
    for (const auto& entry : entries) {
        const size_t eq = entry.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        if (ToolbarLower(entry.substr(0, eq)) == key) return entry.substr(eq + 1);
    }
    return {};
}

// Sets (`exe` non-empty) or clears the program of `ext`. Returns whether the
// list changed.
inline bool SetToolbarOpenApp(std::vector<std::wstring>& entries, const std::wstring& ext,
                              const std::wstring& exe) {
    const std::wstring key = ToolbarLower(ext);
    if (key.empty()) return false;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const size_t eq = it->find(L'=');
        if (eq == std::wstring::npos || eq == 0 || ToolbarLower(it->substr(0, eq)) != key) continue;
        if (exe.empty()) { entries.erase(it); return true; }
        if (it->substr(eq + 1) == exe) return false;
        *it = key + L"=" + exe;
        return true;
    }
    if (exe.empty()) return false;
    entries.push_back(key + L"=" + exe);
    return true;
}

// How a pinned app relates to a set of items (the selection or a drag).
enum class AppFit : int { Neutral = 0, All = 1, Some = 2, None = 3 };

// From per-item answers: `opens` items it opens, `rejects` it does not, and
// `unknown` that cannot be told yet (folders, shortcuts, a type still being
// read). Unknown items never dim an app: with no rejects the fit is All (or
// Neutral when nothing is known), with some it is Some.
inline AppFit AppFitFromCounts(size_t opens, size_t rejects, size_t unknown) {
    if (rejects == 0) return opens > 0 ? AppFit::All : AppFit::Neutral;
    return opens == 0 && unknown == 0 ? AppFit::None : AppFit::Some;
}

// ---- Runtime (Windows) ------------------------------------------------------

struct ToolbarAppInfo {
    std::wstring path;   // as pinned: an .exe, or a shortcut to one
    std::wstring exe;    // the program it starts; "" when a shortcut does not resolve
    std::wstring name;   // display name
    common::MenuIconRef icon;
    bool ready = false;  // false: placeholder (file name, no icon) until the worker is done
};

// Reads the shortcut, name and icon now (registry + files). Workers, or a
// picker result the user is waiting for.
ToolbarAppInfo DescribeToolbarAppNow(const std::wstring& path);

// Queues a job on a worker thread (the job initializes COM itself).
using ToolbarAppWork = std::function<void(std::function<void()>)>;

// Cached description. The first call for a path queues the lookup on `work`
// and returns a placeholder; `notify` is repainted when the real one lands.
ToolbarAppInfo ToolbarApp(const std::wstring& path, HWND notify, const ToolbarAppWork& work);

// Does `exe` open files of type `ext` (".dwg")? 1 / 0, or -1 while the
// type's programs are still being read (`notify` repaints when they are).
int ToolbarAppOpens(const std::wstring& exe, const std::wstring& ext, HWND notify,
                    const ToolbarAppWork& work);
// Same, reading the type's programs now when they are not cached yet (a click
// or a drop: the user is waiting for the result anyway).
bool ToolbarAppOpensNow(const std::wstring& exe, const std::wstring& ext);

// The type's default program (打开方式's "always" program for ".txt"): what the
// toolbar's open button names and starts when the user has not picked one.
struct ToolbarDefaultApp {
    std::wstring exe;    // the program; "" when the type has none (see `name`)
    std::wstring name;   // display name (a Store handler may have no exe)
    common::MenuIconRef icon;
    bool ready = false;  // false: still being read
};
// Cached; the first call queues the lookup on `work` and returns an empty
// (not ready) answer, `notify` is repainted when the real one lands.
ToolbarDefaultApp ToolbarDefaultAppFor(const std::wstring& ext, HWND notify,
                                       const ToolbarAppWork& work);
// Same, read now (opening the button's dropdown or a click: the user waits).
ToolbarDefaultApp DescribeToolbarDefaultAppNow(const std::wstring& ext);

// Recommended programs (打开方式) for the given types, most common first,
// skipping `exclude` (already pinned). Synchronous: COM + registry, so call it
// where a short wait is fine (opening a menu or the customize sheet).
struct SuggestedToolbarApp {
    std::wstring exe;
    std::wstring name;
    common::MenuIconRef icon;
};
std::vector<SuggestedToolbarApp> SuggestToolbarApps(const std::vector<std::wstring>& exts,
                                                    const std::vector<std::wstring>& exclude,
                                                    size_t cap);

}  // namespace pulse::app
