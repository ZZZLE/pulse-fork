# Pulse fork：相对上游的改动

上游仓库：[jimmgreen/pulse](https://github.com/jimmgreen/pulse)

本文件只记录 **本 fork 自己新增或修改的部分**。没有提到的功能、构建方式、
目录结构、配置项，一律以上游原仓库为准，这里不重复。

---

## 红绿灯窗口按钮

### 背景

Pulse 的主窗口使用 `WS_EX_NOREDIRECTIONBITMAP` + DirectComposition 绘制，
客户区覆盖整个窗口（`WM_NCCALCSIZE` 返回 0）。这种窗口没有 DWM 重定向表面，
**DWM 画的系统标题栏按钮无法显示在客户区之上**——它一直在，只是被 Pulse 的
表面盖住，只在切换窗口效果（backdrop 重建）的瞬间露出一下。

因此窗口按钮改为自绘，位置沿用上游原本就预留好的右侧 `46 × 3 DIP` 区域
（`MakeTitleChrome` 的 `chrome_left = window_w - ctrlW * 3`）。

### 行为

- 主窗口右上角三个圆点，从左到右：**最小化（绿）、最大化/还原（黄）、关闭（红）**
  —— 即 Windows 的按钮顺序，配色是 macOS 红绿灯。
- 悬停时显示对应符号，符号用各自颜色的深色版（不是黑色）。
- 黄点的符号随窗口状态切换：
  - 普通状态 → 两个实心三角**尖端朝外**（展开）
  - 已最大化 → 两个实心三角**尖端朝内**（还原）
- 所有自绘对话框（picker / 保存 / 重命名 / 确认 / 连接服务器 / 扩展面板等）的
  标题栏按钮走同一个 `Painter::DrawTitleBarButton`，也改为红点等对应样式。

### 参数

主窗口（`ui_renderer.cpp`，数值为 DIP，实际乘以 `scale_`）：

| 项 | 值 |
|---|---|
| 圆点半径 | `7.2` |
| 三角长度（x 方向） | `2.4` |
| 三角半高（扁度） | `2.4` |
| 最大化：尖端距中心 | `3.4` |
| 还原：尖端距中心 | `1.2` |

对话框（`fluent_components.cpp`，以圆点半径 `r` 为基准）：

| 项 | 值 |
|---|---|
| 圆点半径 | `Px(7.2f)`，与主窗口一致 |
| 三角长度 | `r * 0.33` |
| 三角半高 | `r * 0.33` |
| 最大化：尖端距中心 | `r * 0.47` |
| 还原：尖端距中心 | `r * 0.17` |

颜色：

| 圆点 | 填充 | 悬停提亮 | 符号深色 |
|---|---|---|---|
| 绿（最小化） | `#30D158` | `#4FE077` | `#0B6E2A` |
| 黄（最大化/还原） | `#FFD60A` | `#FFE14F` | `#8A6D00` |
| 红（关闭） | `#FF453A` | `#FF6F66` | `#99160F` |

关闭的 `×`：半长 `2.6`、线宽 `1.8`（比原来的更长更粗的版本短而粗）。

### 涉及文件

| 文件 | 改动 |
|---|---|
| `src/app/window_factory.h` | `kMainWindowStyle` 增加 `WS_SYSMENU` |
| `src/app/app_main.cpp` | 对应的 `static_assert` 改为要求 `WS_SYSMENU` |
| `src/ui/ui_renderer.cpp` | 主窗口三个圆点的绘制与悬停符号 |
| `src/ui/ui_hit_test.cpp` | 三个按钮的命中区域顺序（左 最小化 / 中 最大化 / 右 关闭） |
| `src/ui/ui_compositor.h` | 新增 `D2DFactory()` 访问器（实心三角需要 factory 创建几何） |
| `src/ui/fluent_components.cpp` | `Painter::DrawTitleBarButton` 的 `Minimize/Maximize/Restore/Close` 角色改为红绿灯圆点 |

### 下次合并上游时的注意事项

1. 上游若再次调整标题栏布局（`MakeTitleChrome` 的 `ctrlW` 或 `chrome_left`
   计算方式），上表的位置基准要重新核对——圆点中心取的是
   `right - (3 - cell) * ctrlW + ctrlW / 2`。
2. `WS_SYSMENU` 要保持与 `static_assert` 一致：断言现在要求存在，如果上游把
   它改回「不要」，两处都要跟着改，否则编译不过（这是有意留的护栏）。
3. `DrawTitleBarButton` 是**所有对话框共用**的入口，上游如果新增别的
   `TitleBarButtonRole` 值，需要在上面的 `switch` 里补对应颜色。
4. 实心三角用 `ID2D1Factory1::CreatePathGeometry`（不是 device context 上的
   方法），这是 `ui_compositor.h` 增加 `D2DFactory()` 的原因。
5. 三角的几何约束：**尖端朝外时底边朝内**，两个三角的底边会在中心交汇，
   所以 `最大化的 off` 必须大于 `len × 0.707`（否则糊成一个点）；
   **尖端朝内时底边朝外**，不占用中心，`还原的 off` 可以很小。

---

## 工具栏「打开」按钮

见提交 `0e805f2`，包含：

- 选中文件时在排序/筛选/分组左侧显示「打开」按钮，点击用默认程序打开；
  `⌄` 选择按扩展名记住的默认打开程序。
- Electron 应用（Photon Studio 等）通过其自带 CLI 打开，新增
  `src/ops/app_cli_open.cpp`。
- `CMakeLists.txt` 末尾有一段自动检测：任何编译 `ops_manager.cpp` 的目标
  都会补上 `src/ops/app_cli_open.cpp`（含 `PulsePickerUiProof.dll`）。

---

## 其它上游对齐

以下是上游 1.0.62 合并时产生的适配，不含独立功能：

- 上下文菜单的 `CMF_EXPLORE` 标志与火绒目录动词折叠（`src/shell_host/main.cpp`）。
- 托盘「立即重启 Pulse」（`WM_RESTART_PULSE`）。
- 扩展清单接受 `contributes`，资源上限与 `manifest.schema.json` 对齐（15 项）。
