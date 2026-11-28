// app_cli_open.h — 用某些编辑器自带的 CLI 打开文件。
//
// Photon Studio (Electron) 的文件关联命令是 `"Photon Studio.exe" "%1"`，但它并不
// 接受这种启动方式：只有 app 里已经存在一个编辑器窗口时，它的 second-instance
// 处理才会把命令行里的路径交给那个窗口（app 内的 openPathsFromArgv + flush）；
// 停在主页时路径被直接丢弃 —— 双击或 ShellExecute 都只是"没反应"。
//
// 这类程序在安装目录里带了一个 CLI，可靠入口是它：
//   <root>\resources\app.asar.unpacked\build\cli\win32-x64\photon.exe
//     execute session.open --input <json> --show
// 命中条件（Electron 布局 + CLI 名是主程序名的前缀）时用它打开。
// 其它 Electron 程序的 CLI 语法各不相同，所以不做猜测：只认这种"带桥的"安装。
#pragma once

#include <windows.h>

#include <string>

namespace pulse::ops {

// The app ships a CLI this code knows how to drive (see above). Cached.
bool AppHasBundledCli(const std::wstring& app_exe);

// Opens `file` through that CLI. Runs the CLI synchronously (it hands the file
// over and exits: under a second in practice) and returns whether the app
// accepted it — the exit code is the app's own answer. False when there is no
// CLI to use, or it failed; the caller then falls back to the plain launch.
bool TryOpenWithBundledCli(const std::wstring& app_exe, const std::wstring& file);

}  // namespace pulse::ops
