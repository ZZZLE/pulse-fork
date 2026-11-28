// app_toolbar_apps.cpp — pinned apps on the toolbar:
// view model, click, drop mode, the chip ⋯ rows and the 自定义工具栏 section.
// Program data (names, icons, which types an app opens) comes from
// toolbar_apps.h; opening goes through OpsManager::OpenProgramIn with the
// same arguments a program row in the list gets for dropped files.
#include "app_internal.h"
#include "link_resolve.h"
#include "toolbar_apps.h"

#include "../common/localization.h"
#include "../ui/toolbar_pin_drag.h"
#include "../ui/drag_drop.h"
#include "../ui/fluent_menu.h"
#include "../ui/toolbar_picker.h"

#include <shlobj.h>

#include <algorithm>
#include <map>

namespace pulse {
namespace {

constexpr size_t kFitScanCap = 256;        // items looked at for the hints
constexpr size_t kSelectionScanCap = 2000; // larger selections: no hints

app::ToolbarAppWork Work(AppState& s) {
    return [&s](std::function<void()> job) { s.worker.EnqueueIo(std::move(job)); };
}

std::wstring Fill(std::wstring text, const wchar_t* token, const std::wstring& value) {
    const size_t at = text.find(token);
    if (at != std::wstring::npos) text.replace(at, wcslen(token), value);
    return text;
}

std::wstring Text(l10n::StringId id) { return l10n::Get(id); }

// The dragged or selected items, as file types with counts. Folders and items
// without a type are `unknown` (they never dim an app).
struct ItemTypes {
    std::map<std::wstring, size_t> exts;
    size_t unknown = 0;
    size_t total = 0;
};

void AddItem(ItemTypes& t, const std::wstring& name, bool is_dir) {
    ++t.total;
    const std::wstring ext = is_dir ? std::wstring() : app::ToolbarFileExt(name);
    if (ext.empty() || ext == L".lnk") ++t.unknown;
    else ++t.exts[ext];
}

ItemTypes TypesOfPaths(const std::vector<std::wstring>& paths) {
    ItemTypes t;
    for (size_t i = 0; i < paths.size() && i < kFitScanCap; ++i) AddItem(t, paths[i], false);
    return t;
}

ItemTypes TypesOfSelection(AppState& s) {
    ItemTypes t;
    const app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || IsRecycleTab(tab)) return t;
    const size_t count = tab->SelectedCount();
    if (count == 0 || count > kSelectionScanCap) return t;
    const auto indices = tab->SelectedIndices();
    for (size_t i = 0; i < indices.size() && i < kFitScanCap; ++i) {
        const int index = indices[i];
        if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
        const auto& entry = tab->EntryAt(index);
        AddItem(t, entry.name, entry.is_dir);
    }
    return t;
}

struct FitCount {
    size_t opens = 0, rejects = 0, unknown = 0;
    app::AppFit Fit() const { return app::AppFitFromCounts(opens, rejects, unknown); }
};

FitCount CountFit(AppState& s, const std::wstring& exe, const ItemTypes& types) {
    FitCount c;
    c.unknown = types.unknown;
    for (const auto& [ext, n] : types.exts) {
        const int v = exe.empty() ? -1 : app::ToolbarAppOpens(exe, ext, s.hwnd, Work(s));
        (v > 0 ? c.opens : v == 0 ? c.rejects : c.unknown) += n;
    }
    return c;
}

// Two pins can share a name (a renamed copy, two versions side by side):
// the file name tells them apart in the tooltip, the menus and the dialog.
void Disambiguate(AppState& s, size_t index, app::ToolbarAppInfo& info) {
    for (size_t j = 0; j < s.appPrefs.toolbar_apps.size(); ++j) {
        if (j == index) continue;
        const app::ToolbarAppInfo other = app::ToolbarApp(s.appPrefs.toolbar_apps[j], s.hwnd, Work(s));
        if (!other.name.empty() && _wcsicmp(other.name.c_str(), info.name.c_str()) == 0) {
            info.name += L" (" + fs::StripLnkSuffix(app::ToolbarFileName(info.path)) + L")";
            return;
        }
    }
}

app::ToolbarAppInfo AppAt(AppState& s, size_t index) {
    if (index >= s.appPrefs.toolbar_apps.size()) return {};
    app::ToolbarAppInfo info = app::ToolbarApp(s.appPrefs.toolbar_apps[index], s.hwnd, Work(s));
    Disambiguate(s, index, info);
    return info;
}

// Ready description: the cached one, or read now (a click, a drop, a menu).
app::ToolbarAppInfo AppNow(AppState& s, size_t index) {
    app::ToolbarAppInfo info = AppAt(s, index);
    if (!info.ready && index < s.appPrefs.toolbar_apps.size()) {
        info = app::DescribeToolbarAppNow(s.appPrefs.toolbar_apps[index]);
        Disambiguate(s, index, info);
    }
    return info;
}

// What a click or a drop starts: the program, or the pinned shortcut when it
// does not resolve to one (a Store app's shortcut still starts).
std::wstring LaunchTarget(const app::ToolbarAppInfo& info) {
    return info.exe.empty() ? app::ToolbarPlainPath(info.path) : info.exe;
}

// The items an app takes: every item when `all`; otherwise folders, items the
// app lists in 打开方式 and shortcuts whose target it opens (passed as that
// target, which is what the program can read).
std::vector<std::wstring> ItemsFor(const std::wstring& exe, const std::vector<std::wstring>& items, bool all) {
    if (all || exe.empty()) return items;
    std::vector<std::wstring> out;
    for (const auto& item : items) {
        const std::wstring ext = app::ToolbarFileExt(item);
        const DWORD attrs = GetFileAttributesW(item.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            out.push_back(item);
        } else if (ext == L".lnk") {
            fs::DirEntry entry;
            if (app::ResolveLink(item, entry) && !entry.link_target.empty()) {
                if (entry.link_target_is_dir ||
                    app::ToolbarAppOpensNow(exe, app::ToolbarFileExt(entry.link_target)))
                    out.push_back(entry.link_target);
            }
        } else if (app::ToolbarAppOpensNow(exe, ext)) {
            out.push_back(item);
        }
    }
    return out;
}

void Start(AppState& s, const std::wstring& program, const std::vector<std::wstring>& items) {
    const std::wstring folder = fs::ParentPath(fs::NormalizePath(program));
    s.ops.OpenProgramIn(program, folder, items.empty() ? std::wstring() : ui::DropLaunchArguments(items));
}

// Pin suggestions offered by the chip ⋯ flyout, kept for the command ids.
std::vector<app::SuggestedToolbarApp>& MenuSuggestions() {
    static std::vector<app::SuggestedToolbarApp> list;
    return list;
}

// The file types in the current folder, most common first.
std::vector<std::wstring> FolderTypes(AppState& s, size_t cap) {
    std::vector<std::wstring> out;
    const app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || IsRecycleTab(tab)) return out;
    std::map<std::wstring, size_t> counts;
    const size_t n = std::min<size_t>(tab->EntryCount(), 5000);
    for (size_t i = 0; i < n; ++i) {
        const auto& entry = tab->EntryAt(static_cast<int>(i));
        if (entry.is_dir) continue;
        const std::wstring ext = app::ToolbarFileExt(entry.name);
        if (!ext.empty() && ext != L".lnk" && ext != L".exe") ++counts[ext];
    }
    std::vector<std::pair<std::wstring, size_t>> sorted(counts.begin(), counts.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sorted.size() && out.size() < cap; ++i) out.push_back(sorted[i].first);
    return out;
}

// ".dwg .dxf" — the folder's types an app opens, for the sheet's caption.
std::wstring TypesCaption(const std::wstring& exe, const std::vector<std::wstring>& folder_types) {
    std::wstring out;
    size_t shown = 0;
    for (const auto& ext : folder_types) {
        if (!app::ToolbarAppOpensNow(exe, ext)) continue;
        if (shown == 4) { out += L" …"; break; }
        if (!out.empty()) out += L" ";
        out += ext;
        ++shown;
    }
    return out;
}

}  // namespace

std::vector<ui::WindowViewModel::ToolbarAppPin> ToolbarAppPins(AppState& s) {
    std::vector<ui::WindowViewModel::ToolbarAppPin> out;
    const auto& apps = s.appPrefs.toolbar_apps;
    if (apps.empty()) return out;
    const ItemTypes types = s.toolbarDropMode ? TypesOfPaths(s.toolbarDropSources) : TypesOfSelection(s);
    for (size_t i = 0; i < apps.size() && i < app::kMaxToolbarApps; ++i) {
        const app::ToolbarAppInfo info = AppAt(s, i);
        ui::WindowViewModel::ToolbarAppPin pin;
        pin.name = info.name;
        pin.icon = info.icon;
        if (types.total > 0) pin.fit = static_cast<int>(CountFit(s, info.exe, types).Fit());
        out.push_back(std::move(pin));
    }
    return out;
}

void OpenWithToolbarApp(AppState& s, size_t app, const std::vector<std::wstring>& items, bool all) {
    if (app >= s.appPrefs.toolbar_apps.size()) return;
    const app::ToolbarAppInfo info = AppNow(s, app);
    Start(s, LaunchTarget(info), ItemsFor(info.exe, items, all));
}

void DropOnToolbarApp(AppState& s, size_t app, const std::vector<std::wstring>& items, bool all) {
    if (app >= s.appPrefs.toolbar_apps.size()) return;
    const app::ToolbarAppInfo info = AppNow(s, app);
    std::vector<std::wstring> handed = ItemsFor(info.exe, items, all);
    const size_t count = handed.size();
    Start(s, LaunchTarget(info), handed);
    // As in the prototype: the icon gulps the drop, then a note says what
    // went where.
    ui::GulpToolbarApp(&s.renderer, static_cast<int>(app));
    if (count > 0 && s.hwnd)
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Window,
            Fill(Fill(Text(l10n::StringId::ToolbarAppOpened), L"{app}", info.name), L"{n}", std::to_wstring(count)),
            std::wstring{}, {}, std::wstring{}, 2600);
}

void RunToolbarApp(AppState& s, size_t app) {
    if (app >= s.appPrefs.toolbar_apps.size()) return;
    const app::Tab* tab = ActiveTab(s);
    std::vector<std::wstring> selected;
    if (tab && !IsRecycleTab(tab) && tab->SelectedCount() > 0) selected = SelectedFullPaths(*tab);
    // Opens the selected items it takes; with none of them (or nothing
    // selected) the click just starts the program.
    OpenWithToolbarApp(s, app, selected, false);
}

std::wstring ToolbarAppName(AppState& s, size_t app) {
    return app < s.appPrefs.toolbar_apps.size() ? AppNow(s, app).name : std::wstring();
}

bool PinToolbarApp(AppState& s, const std::wstring& path) {
    if (!app::AddToolbarAppPath(s.appPrefs.toolbar_apps, path)) return false;
    if (!s.isolatedTest) s.appPrefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

std::wstring ToolbarAppTooltip(AppState& s, size_t app) {
    if (app >= s.appPrefs.toolbar_apps.size()) return {};
    const app::ToolbarAppInfo info = AppAt(s, app);
    const ItemTypes types = TypesOfSelection(s);
    const FitCount c = types.total > 0 ? CountFit(s, info.exe, types) : FitCount{};
    switch (c.Fit()) {
    case app::AppFit::All: return Fill(Text(l10n::StringId::ShellOpenApp), L"{app}", info.name);
    case app::AppFit::Some:
        return Fill(Fill(Text(l10n::StringId::ToolbarAppOpenSome), L"{app}", info.name), L"{n}",
                    std::to_wstring(c.opens + c.unknown));
    case app::AppFit::None: return Fill(Text(l10n::StringId::ToolbarAppCannotOpen), L"{app}", info.name);
    default: return Fill(Text(l10n::StringId::ToolbarAppTip), L"{app}", info.name);
    }
}

void UpdateToolbarDropMode(AppState& s, const std::vector<std::wstring>& sources) {
    // A single program among the dragged items can be pinned (no COM here:
    // a shortcut is checked when it is dropped).
    std::wstring program;
    if (sources.size() == 1) {
        const std::wstring& source = sources.front();
        const std::wstring ext = app::ToolbarFileExt(source);
        if ((ext == L".lnk" || ui::IsDropLaunchProgram(source)) &&
            !app::ProgramListHas(s.appPrefs.toolbar_apps, source) &&
            s.appPrefs.toolbar_apps.size() < app::kMaxToolbarApps)
            program = source;
    }
    const bool mode = !s.appPrefs.toolbar_apps.empty() || !program.empty();
    if (mode != s.toolbarDropMode || program != s.toolbarDropProgram || sources != s.toolbarDropSources) {
        s.toolbarDropMode = mode;
        s.toolbarDropProgram = std::move(program);
        s.toolbarDropSources = mode ? sources : std::vector<std::wstring>{};
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

bool ResolveToolbarAppDrop(AppState& s, const ui::WindowViewModel& vm, const ui::HitTestResult& hit,
                           const std::vector<std::wstring>& sources, POINT pt, DWORD key_state,
                           DWORD allowed, DWORD& effect) {
    s.dropToolbarApp = -1;
    s.dropToolbarForce = false;
    s.dropToolbarPin = false;
    if (!vm.toolbar_drop_mode) return false;
    const DWORD open_effect = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY
        : (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : DROPEFFECT_NONE;
    const auto badge = [&](std::wstring text) {
        s.dropBadge = std::move(text);
        s.dropBadgeX = static_cast<float>(pt.x);
        s.dropBadgeY = static_cast<float>(pt.y);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    };
    if (hit.region == ui::HitTestResult::ToolbarPinLabel) {
        effect = DROPEFFECT_NONE;
        if (!s.toolbarDropProgram.empty() && open_effect != DROPEFFECT_NONE) {
            s.dropToolbarPin = true;
            badge(Fill(Text(l10n::StringId::ToolbarPinApp), L"{app}",
                       fs::StripLnkSuffix(app::ToolbarFileName(s.toolbarDropProgram))));
            effect = (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : open_effect;
        } else {
            InvalidateRect(s.hwnd, nullptr, FALSE);
        }
        return true;
    }
    const int from = static_cast<int>(vm.toolbar_pins.size());
    if (hit.region != ui::HitTestResult::ToolbarPin || hit.index < from) return false;
    const size_t app = static_cast<size_t>(hit.index - from);
    if (app >= vm.toolbar_apps.size() || app >= s.appPrefs.toolbar_apps.size()) return false;
    s.springRow = -1;
    effect = DROPEFFECT_NONE;
    const app::ToolbarAppInfo info = AppAt(s, app);
    // A program is not handed itself.
    if (open_effect == DROPEFFECT_NONE || (!info.exe.empty() && ui::DropIntoOwnFolder(sources, info.exe))) {
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    const bool shift = (key_state & MK_SHIFT) != 0;
    const FitCount c = CountFit(s, info.exe, TypesOfPaths(sources));
    const app::AppFit fit = c.Fit();
    s.dropToolbarApp = static_cast<int>(app);
    if (fit == app::AppFit::None && !shift) {
        // Outline stays off: nothing would open. Shift forces it.
        s.dropToolbarApp = -1;
        badge(Fill(Text(l10n::StringId::ToolbarAppCannotOpen), L"{app}", info.name) + L" · " +
              Text(l10n::StringId::ToolbarAppForceHint));
        return true;
    }
    s.dropToolbarForce = shift && fit != app::AppFit::All && fit != app::AppFit::Neutral;
    if (s.dropToolbarForce)
        badge(Fill(Text(l10n::StringId::ToolbarAppForceOpen), L"{app}", info.name));
    else if (fit == app::AppFit::Some)
        badge(Fill(Fill(Text(l10n::StringId::ToolbarAppOpenSome), L"{app}", info.name), L"{n}",
                   std::to_wstring(c.opens + c.unknown + (sources.size() > kFitScanCap ? sources.size() - kFitScanCap : 0))));
    else
        badge(Fill(Text(l10n::StringId::ShellOpenApp), L"{app}", info.name));
    effect = open_effect;
    return true;
}

std::vector<ui::FluentMenuItem> ToolbarAppMenuItems(AppState& s) {
    std::vector<ui::FluentMenuItem> items;
    MenuSuggestions().clear();
    const app::Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->SelectedCount() == 0) return items;
    const ItemTypes types = TypesOfSelection(s);
    // 用 <pinned app> 打开, for the apps that open some of the selection.
    for (size_t i = 0; i < s.appPrefs.toolbar_apps.size() && i < app::kMaxToolbarApps; ++i) {
        const app::ToolbarAppInfo info = AppNow(s, i);
        const app::AppFit fit = types.total > 0 ? CountFit(s, info.exe, types).Fit() : app::AppFit::Neutral;
        if (fit == app::AppFit::None) continue;
        ui::FluentMenuItem item;
        item.command = app::CmdToolbarAppBase + static_cast<int>(i);
        item.text = Fill(Text(l10n::StringId::ShellOpenApp), L"{app}", info.name);
        item.icon = info.icon;
        items.push_back(std::move(item));
    }
    // 固定到工具栏 ▸ the programs Windows recommends for the selected types.
    if (s.appPrefs.toolbar_apps.size() < app::kMaxToolbarApps && !types.exts.empty()) {
        std::vector<std::wstring> exts;
        for (const auto& [ext, n] : types.exts) exts.push_back(ext);
        std::vector<std::wstring> exclude = s.appPrefs.toolbar_apps;
        for (size_t i = 0; i < s.appPrefs.toolbar_apps.size(); ++i) {
            const app::ToolbarAppInfo info = AppAt(s, i);
            if (!info.exe.empty()) exclude.push_back(info.exe);
        }
        auto& suggestions = MenuSuggestions();
        suggestions = app::SuggestToolbarApps(exts, exclude, 6);
        if (!suggestions.empty()) {
            ui::FluentMenuItem pin;
            pin.text = Text(l10n::StringId::PinToToolbar);
            pin.glyph = L"\xE718";
            for (size_t i = 0; i < suggestions.size(); ++i) {
                ui::FluentMenuItem child;
                child.command = app::CmdToolbarPinAppBase + static_cast<int>(i);
                child.text = suggestions[i].name;
                child.icon = suggestions[i].icon;
                pin.children.push_back(std::move(child));
            }
            items.push_back(std::move(pin));
        }
    }
    return items;
}

bool RunToolbarAppMenuCommand(AppState& s, int command) {
    if (command >= app::CmdToolbarAppBase && command < app::CmdToolbarAppBase + static_cast<int>(app::kMaxToolbarApps)) {
        RunToolbarApp(s, static_cast<size_t>(command - app::CmdToolbarAppBase));
        return true;
    }
    if (command >= app::CmdToolbarPinAppBase && command < app::CmdToolbarPinAppBase + 16) {
        const auto& suggestions = MenuSuggestions();
        const size_t i = static_cast<size_t>(command - app::CmdToolbarPinAppBase);
        if (i < suggestions.size()) PinToolbarApp(s, suggestions[i].exe);
        return true;
    }
    return false;
}

void FillToolbarAppRows(AppState& s, ui::ToolbarCustomizeSpec& spec) {
    spec.add_app_text = Text(l10n::StringId::ToolbarAddApp);
    spec.add_app_caption = Text(l10n::StringId::ToolbarAddAppCaption);
    spec.max_apps = app::kMaxToolbarApps;
    const std::vector<std::wstring> folder_types = FolderTypes(s, 12);
    std::vector<std::wstring> exclude = s.appPrefs.toolbar_apps;
    for (size_t i = 0; i < s.appPrefs.toolbar_apps.size(); ++i) {
        const app::ToolbarAppInfo info = AppNow(s, i);
        ui::ToolbarCustomizeRow row;
        row.value = info.path;
        row.label = info.name;
        row.icon = info.icon;
        row.caption = TypesCaption(info.exe, folder_types);
        row.checked = true;
        if (!info.exe.empty()) exclude.push_back(info.exe);
        spec.apps.push_back(std::move(row));
    }
    // Two unpinned programs this folder's files open with (此文件夹常用).
    if (!folder_types.empty()) {
        const std::vector<std::wstring> top(folder_types.begin(),
                                            folder_types.begin() + std::min<size_t>(folder_types.size(), 6));
        for (auto& suggestion : app::SuggestToolbarApps(top, exclude, 2)) {
            ui::ToolbarCustomizeRow row;
            row.value = suggestion.exe;
            row.label = suggestion.name;
            row.icon = suggestion.icon;
            row.badge = Text(l10n::StringId::ToolbarAppCommonHere);
            row.caption = TypesCaption(suggestion.exe, folder_types);
            spec.apps.push_back(std::move(row));
        }
    }
    spec.add_app = [&s, folder_types](HWND owner, ui::ToolbarCustomizeRow& row) {
        std::wstring path;
        if (!PickToolbarAppFile(s, owner, path)) return false;
        const app::ToolbarAppInfo info = app::DescribeToolbarAppNow(path);
        if (info.exe.empty() || !ui::IsDropLaunchProgram(info.exe)) {
            MessageBeep(MB_ICONWARNING);
            return false;
        }
        row.value = path;
        row.label = info.name;
        row.icon = info.icon;
        row.caption = TypesCaption(info.exe, folder_types);
        return true;
    };
}

bool ApplyToolbarAppRows(AppState& s, const std::vector<ui::ToolbarPickerTile>& rows) {
    std::vector<std::wstring> apps;
    for (const auto& row : rows)
        if (row.checked) app::AddToolbarAppPath(apps, row.value);
    if (apps == s.appPrefs.toolbar_apps) return false;
    s.appPrefs.toolbar_apps = std::move(apps);
    return true;
}

// ---- 选中时的「打开」按钮 ---------------------------------------------------

ToolbarOpenTarget ToolbarOpenTargetFor(AppState& s) {
    ToolbarOpenTarget out;
    const app::Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->SelectedCount() == 0) return out;
    // The first selected file decides (folders and shortcuts have no type of
    // their own); a selection of folders only shows no button at all.
    std::wstring ext;
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
        const auto& entry = tab->EntryAt(index);
        if (entry.is_dir) continue;
        // 选中里只要有文件就出现按钮；有扩展名的那个决定用哪个程序。
        out.has_file = true;
        const std::wstring type = app::ToolbarFileExt(entry.name);
        if (!type.empty() && type != L".lnk") { ext = type; break; }
    }
    out.ext = ext;
    if (!out.has_file) return out;
    if (ext.empty()) return out;   // 有文件但没有类型：只有占位按钮
    if (const std::wstring chosen = app::ToolbarOpenAppFor(s.appPrefs.toolbar_open_apps, ext);
        !chosen.empty()) {
        const app::ToolbarAppInfo info = app::ToolbarApp(chosen, s.hwnd, Work(s));
        out.exe = chosen;
        out.name = info.name.empty() ? app::ToolbarFileName(chosen) : info.name;
        out.icon = info.icon;
        return out;
    }
    const app::ToolbarDefaultApp fallback = app::ToolbarDefaultAppFor(ext, s.hwnd, Work(s));
    out.exe = fallback.exe;
    out.name = fallback.name;
    out.icon = fallback.icon;
    return out;
}

void OpenSelectionFromToolbar(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab)) return;
    std::vector<std::wstring> files;
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
        if (tab->EntryAt(index).is_dir) continue;
        std::wstring path = EntryFullPath(*tab, index);
        if (!path.empty()) files.push_back(std::move(path));
    }
    if (files.empty()) return;
    const ToolbarOpenTarget target = ToolbarOpenTargetFor(s);
    // Nothing remembered (or the default is still being read): the shell's own
    // open does exactly what a double-click does.
    const std::wstring exe = target.exe;
    for (const auto& file : files) {
        if (exe.empty()) s.ops.OpenWith(file);
        else s.ops.OpenWithApp(exe, file);
    }
}

void ShowToolbarOpenMenu(AppState& s, POINT screen) {
    if (!EnsureMenu(s)) return;
    const ToolbarOpenTarget target = ToolbarOpenTargetFor(s);
    if (target.ext.empty()) return;
    const std::wstring ext = target.ext;
    const std::wstring chosen = app::ToolbarOpenAppFor(s.appPrefs.toolbar_open_apps, ext);
    const app::ToolbarDefaultApp fallback = app::DescribeToolbarDefaultAppNow(ext);
    constexpr int kDefaultCommand = 36000;
    constexpr int kRememberedCommand = 36064;
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem def;
    def.command = kDefaultCommand;
    def.text = Text(l10n::StringId::OpenInDefaultApp);
    if (!fallback.name.empty()) def.text += L" · " + fallback.name;
    def.badge_text = ext;
    def.icon = fallback.icon;
    // 当前生效的那一项用底色 + 描边标出来（图标列被程序图标占着，圆点画不出来）。
    def.marked = chosen.empty();
    def.separator_after = true;
    items.push_back(std::move(def));
    const auto suggestions = app::SuggestToolbarApps({ext}, {}, 8);
    std::vector<std::wstring> listed;
    for (size_t i = 0; i < suggestions.size() && i < 8; ++i) {
        ui::FluentMenuItem row;
        row.command = kDefaultCommand + 1 + static_cast<int>(i);
        row.text = suggestions[i].name;
        row.icon = suggestions[i].icon;
        row.marked = !chosen.empty() && app::ProgramListHas({chosen}, suggestions[i].exe);
        listed.push_back(suggestions[i].exe);
        items.push_back(std::move(row));
    }
    // A remembered program Explorer does not offer for the type still gets a row.
    if (!chosen.empty() && !app::ProgramListHas(listed, chosen)) {
        ui::FluentMenuItem row;
        row.command = kRememberedCommand;
        row.text = target.name.empty() ? app::ToolbarFileName(chosen) : target.name;
        row.icon = target.icon;
        row.marked = true;
        items.push_back(std::move(row));
    }
    // 光标落在当前选中的那一项上（默认项或记住的程序），不是第一行。
    if (s.menu) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
            if (items[static_cast<size_t>(i)].marked) {
                s.menu->SetInitialHoverRow(i);
                break;
            }
    }
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    bool changed = false;
    if (cmd == kDefaultCommand)
        changed = app::SetToolbarOpenApp(s.appPrefs.toolbar_open_apps, ext, std::wstring());
    else if (cmd > kDefaultCommand && cmd <= kDefaultCommand + 8) {
        const size_t i = static_cast<size_t>(cmd - kDefaultCommand - 1);
        if (i < suggestions.size())
            changed = app::SetToolbarOpenApp(s.appPrefs.toolbar_open_apps, ext, suggestions[i].exe);
    }
    if (!changed) return;
    if (!s.isolatedTest) s.appPrefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

}  // namespace pulse
