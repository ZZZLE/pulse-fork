# Pulse

<img src="assets/pulse-logo.png" alt="Pulse" width="80">

> 本仓库是 **[jimmgreen/pulse](https://github.com/jimmgreen/pulse)** 的 fork，
> **只改动下面列出的部分**。未改动的功能、构建方式、下载与历史版本，请以上游仓库为准。

## 本 fork 的改动

### 红绿灯窗口按钮

Pulse 的主窗口用 `WS_EX_NOREDIRECTIONBITMAP` + DirectComposition 绘制整个客户区，
DWM 画的系统标题栏按钮会被自绘表面盖住（只在切换窗口效果时闪现一下）。
因此窗口按钮改为 Pulse 自绘：

- 右上角三个圆点，**Windows 顺序**：最小化（绿）、最大化/还原（黄）、关闭（红）
- 悬停显示符号，用各自圆点的深色版：绿 `−`、黄实心双三角、红 `×`
- 黄点随窗口状态切换：普通状态三角尖端朝外（展开），已最大化时尖端朝内（还原）
- 所有自绘对话框（picker、保存、重命名、确认等）共用同一套样式
- `kMainWindowStyle` 增加 `WS_SYSMENU`，并同步静态断言

完整参数表、涉及文件、**下次合并上游时的注意事项**见 [FORK-CHANGES.md](FORK-CHANGES.md)。

### 工具栏「打开」按钮

选中文件时出现在排序/筛选/分组左侧，点击用该类型的默认程序打开；
右侧 `⌄` 可按扩展名记住指定的程序。Electron 应用（如 Photon Studio）
通过其自带 CLI 打开。

### 其它

- 上下文菜单：`CMF_EXPLORE`，火绒的目录动词折叠成一个子菜单
- 托盘菜单「立即重启 Pulse」
- 扩展清单接受 `contributes` 字段，资源上限与 `manifest.schema.json` 对齐（15 项）

## 上游

[jimmgreen/pulse](https://github.com/jimmgreen/pulse)：功能介绍、下载、构建说明与历史版本。
