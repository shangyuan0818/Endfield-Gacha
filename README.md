# Endfield-Gacha 终末地抽卡工具

Gacha tracker and visualizer for Arknights: Endfield. Built with C++20 &amp; Win32 API.

《明日方舟：终末地》寻访(抽卡)数据保存，分析与可视化。使用C++20与Win32 API高效处理数据。



## How to use 如何使用
1. Run `main.exe` (the exporter) and input your gacha link.
   运行用于保存抽卡数据的 `main.exe` 主程序，并输入你的抽卡链接。
2. You can find the `uigf_endfield.json` gacha data saved by `main.exe` in the current running directory.
   你可以在运行目录中找到 `main.exe` 程序保存的 `uigf_endfield.json` 文件。
3. Run `gui.exe` (the analyzer) and drag `uigf_endfield.json` onto the window.
   运行用于分析与可视化抽卡数据的 `gui.exe` 图形界面程序，并将 `uigf_endfield.json` 拖拽到程序窗口中。

> [!IMPORTANT]
> Since version 1.4, the in-game headhunting record only covers **the last 90 days**. Records older than
> that are dropped by the official API and can never be fetched again. `main.exe` merges each run into
> the existing `uigf_endfield.json` incrementally, so run it regularly and keep that file — it is the only
> long-term archive of your pulls.
>
> 自 1.4 版本起，游戏内【寻访记录】只支持查询**最近 90 天**的记录，更早的记录会被官方接口丢弃且无法再取回。
> `main.exe` 是增量合并到已有的 `uigf_endfield.json` 的，所以请定期运行并保留该文件 —— 它是你抽卡历史的唯一长期存档。



## How to compile 如何编译
1. Download and install [Build Tools for Visual Studio 2026](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2026).
   下载并安装 [Visual Studio 2026 生成工具](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2026)。
2. Open the **"x64 Native Tools Command Prompt for VS"** application.
   打开 **"x64 Native Tools Command Prompt for VS"** 应用。
3. Copy the command from `Compile.txt` and paste it into the command prompt, then press Enter to run.
   打开 `Compile.txt`，把命令复制粘贴到命令行应用中，按下回车运行。



## Window 窗口
The window is resizable and maximizable. Charts scale with the window width, and the four chart rows
share the remaining height (growing on tall displays). When the content does not fit — on a 1080p
screen, for example — a vertical scrollbar appears. This mirrors the Apple version, whose macOS view
wraps the whole content in a `ScrollView`.

窗口可自由缩放与最大化。图表宽度跟随窗口，四行图表平分剩余高度（屏幕越高图表越大）；内容放不下时
（例如 1080p 屏）会出现垂直滚动条。该行为与 Apple 版一致 —— 其 macOS 界面同样把整个内容区套在
`ScrollView` 中。



## Compatibility 兼容性
### Windows
- **System 系统**: Windows 10 or higher (视窗 10 或更高版本)
- **Minimum System 最低系统**: Windows 7 SP1 with installed [Microsoft Visual C++ Redistributable](https://visualstudio.microsoft.com/downloads/#microsoft-visual-c-v14-redistributable).
- **CPU 处理器**: x86, x86_64, and arm64 (32-bit and 64-bit / 32位 与 64位)

> ### Apple (macOS &amp; iOS)
> Please check the Swift 6 version here 请查看该 Swift 6 版本: [Endfield-Gacha-Apple](https://github.com/shangyuan0818/Endfield-Gacha-Apple)



## Demonstration 效果展示
<img width="947" height="901" alt="image" src="https://github.com/user-attachments/assets/353bcf98-0afb-40a6-8923-44bf093adb45" />
