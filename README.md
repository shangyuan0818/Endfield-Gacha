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
> The in-game headhunting record only covers **the last 90 days**. Records older than that are dropped
> by the official API and can never be fetched again. `main.exe` merges each run into the existing
> `uigf_endfield.json` incrementally, so run it regularly and keep that file — it is the only long-term
> archive of your pulls. If anything goes wrong during a run, the whole update is cancelled and the file is
> left untouched — just run it again.
>
> 游戏内【寻访记录】只支持查询**最近 90 天**的记录，更早的记录会被官方接口丢弃且无法再取回。
> `main.exe` 是增量合并到已有的 `uigf_endfield.json` 的，所以请定期运行并保留该文件 —— 它是你抽卡历史的唯一长期存档。
> 运行过程中任何一步出错，整次更新都会取消，文件保持原样，重新运行即可。

> [!NOTE]
> The exported file also carries a non-standard top-level `non_pull_events` key. The official record API mixes
> non-pull events (such as the headhunting testimonial granted every 60 pulls) into the same list; they are kept
> verbatim under that key instead of the UIGF `list`, so `list` stays "one entry = one pull" for every other
> UIGF tool. Other tools can safely ignore the extra key.
>
> The `time` and `timezone` fields are always written in UTC+8, regardless of your device's time zone;
> `gacha_ts` is the exact timestamp.
>
> 导出的文件里还有一个非 UIGF 标准的顶层键 `non_pull_events`。官方记录接口会把非抽卡事件（例如每 60 抽发放的
> 【寻访情报书】）混在同一个列表里返回；这些事件被原样保存在该键下，而不是放进 UIGF 的 `list`，这样 `list` 对
> 所有第三方 UIGF 工具都保持「每一条都是一次抽卡」的语义。其它工具可以直接忽略这个键。
>
> 文件中的 `time` 与 `timezone` 固定按 UTC+8 写出，与设备所在的时区无关；精确时刻以 `gacha_ts` 为准。



## How to compile 如何编译
1. Download and install [Build Tools for Visual Studio 2026](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2026).
   下载并安装 [Visual Studio 2026 生成工具](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2026)。
2. Open the **"x64 Native Tools Command Prompt for VS"** application.
   打开 **"x64 Native Tools Command Prompt for VS"** 应用。
3. Go to the folder that contains `main.cpp`, `gui.cpp` and `JsonScan.h` (all three are required).
   进入存放 `main.cpp`、`gui.cpp` 与 `JsonScan.h` 的文件夹（三个文件缺一不可）。
4. Copy the command from `Compile.txt` and paste it into the command prompt, then press Enter to run.
   打开 `Compile.txt`，把命令复制粘贴到命令行应用中，按下回车运行。



## Compatibility 兼容性
### Windows
- **System 系统**: Windows 10 or higher (视窗 10 或更高版本)
- **Minimum System 最低系统**: Windows 7 SP1 with installed [Microsoft Visual C++ Redistributable](https://visualstudio.microsoft.com/downloads/#microsoft-visual-c-v14-redistributable).
- **CPU 处理器**: x86, x86_64, and arm64 (32-bit and 64-bit / 32位 与 64位)

> ### Apple (macOS &amp; iOS)
> Please check the Swift 6 version here 请查看该 Swift 6 版本: [Endfield-Gacha-Apple](https://github.com/shangyuan0818/Endfield-Gacha-Apple)



## Demonstration 效果展示
<img width="947" height="901" alt="image" src="https://github.com/user-attachments/assets/353bcf98-0afb-40a6-8923-44bf093adb45" />
