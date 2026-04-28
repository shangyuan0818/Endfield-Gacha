# Endfield-Gacha 终末地抽卡工具
Gacha tracker and visualizer for Arknights: Endfield. Built with C++20 &amp; Win32 API.

《明日方舟：终末地》寻访(抽卡)数据保存，分析与可视化。使用C++20与Win32 API高效处理数据。



# How to use 如何使用
1. Run `main.exe` (the exporter) and input your gacha link.
   运行用于保存抽卡数据的 `main.exe` 主程序，并输入你的抽卡链接。
2. You can find the `uigf_endfield.json` gacha data saved by `main.exe` in the current running directory.
   你可以在运行目录中找到 `main.exe` 程序保存的 `uigf_endfield.json` 文件。
3. Run `gui.exe` (the analyzer) and drag `uigf_endfield.json` onto the window.
   运行用于分析与可视化抽卡数据的 `gui.exe` 图形界面程序，并将 `uigf_endfield.json` 拖拽到程序窗口中。


# How to compile 如何编译
1. Download and install [Build Tools for Visual Studio 2026](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2026).
   下载并安装 Visual Studio 2026 生成工具。
2. Open the **"x64 Native Tools Command Prompt for VS"** application.
   打开 **"x64 Native Tools Command Prompt for VS"** 应用。
3. Copy the command from `Compile.txt` and paste it into the command prompt, then press Enter to run.
   打开 `Compile.txt`，把命令复制粘贴到命令行应用中，按下回车运行。


# Compatibility 兼容性
* **System 系统**: Windows 10 or higher (视窗 10 或更高版本)
* **Minimum System 最低系统**: Windows 7 SP1 with installed [Microsoft Visual C++ Redistributable](https://visualstudio.microsoft.com/downloads/#microsoft-visual-c-v14-redistributable).
* **CPU 处理器**: x86, x86_64, and arm64 (32-bit and 64-bit / 32位和64位)

> * **macOS**: Please check the macOS SwiftUI version here 请查看该SwiftUI版本: [Endfield-Gacha-macOS](https://github.com/shangyuan0818/Endfield-Gacha-macOS)

# Demonstration 效果展示
<img width="1924" height="1397" alt="image" src="https://github.com/user-attachments/assets/eb1a59db-e15d-4301-8947-5ed5eb6f14cf" />
