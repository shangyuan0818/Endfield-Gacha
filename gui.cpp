#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <unordered_set> // 优化1：替换 set，使用 O(1) 哈希表
#include <unordered_map> // 优化2：替换 map，使用 O(1) 哈希表
#include <fstream>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

//#include <nlohmann/json.hpp>
#include "json.hpp"
using json = nlohmann::json;

// 全局 DPI 缩放变量与辅助函数
int g_dpi = 96;
int DPIScale(int value) { return MulDiv(value, g_dpi, 96); }
float DPIScaleF(float value) { return value * (g_dpi / 96.0f); }

std::wstring Utf8ToWstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

struct Pull {
    std::wstring name, item_type, rank_type, uigf_gacha_type;
    long long id;
};

struct Stats {
    std::vector<int> all_pities, up_pities, up_win_pities;  
    double avg_all = 0.0, avg_up = 0.0, avg_win = -1.0; 
    std::unordered_map<int, int> freq_all, freq_up; // 性能提升：替换为 unordered_map
};

Stats statsChar, statsWep;
HWND hOutEdit, hCharEdit, hWepEdit;

// 性能提升：返回 unordered_set
std::unordered_set<std::wstring> ParseCommaSeparated(const std::wstring& text) {
    std::unordered_set<std::wstring> result;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L'，') {
            if (!cur.empty()) {
                cur.erase(0, cur.find_first_not_of(L" \t\r\n"));
                cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
                if (!cur.empty()) result.insert(cur);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        cur.erase(0, cur.find_first_not_of(L" \t\r\n"));
        cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
        if (!cur.empty()) result.insert(cur);
    }
    return result;
}

// 接收 unordered_set 作为参数
Stats Calculate(const std::vector<Pull>& pulls, bool isWeapon, const std::unordered_set<std::wstring>& standard_names) {
    Stats s;
    int current_pity = 0, pity_since_last_up = 0;
    bool next_guaranteed = false;
    
    for (const auto& p : pulls) {
        bool isSpecial = false;
        
        // 优化3：热点循环内不再分配字符串，直接使用已在解析阶段转好的小写类型
        if (isWeapon) {
            if (p.item_type != L"Weapon") continue;
            if (p.uigf_gacha_type.find(L"constant") == std::wstring::npos && 
                p.uigf_gacha_type.find(L"standard") == std::wstring::npos &&
                p.uigf_gacha_type.find(L"beginner") == std::wstring::npos) {
                isSpecial = true;
            }
        } else {
            if (p.item_type != L"Character") continue;
            if (p.uigf_gacha_type.find(L"special") != std::wstring::npos) {
                isSpecial = true;
            }
        }
        
        if (!isSpecial) continue;
        
        current_pity++;
        pity_since_last_up++;
        
        if (p.rank_type == L"6") {
            s.all_pities.push_back(current_pity);
            s.freq_all[current_pity]++;
            
            // O(1) 极速查找
            bool isUP = !standard_names.contains(p.name);
            
            if (isUP) {
                s.up_pities.push_back(pity_since_last_up);
                s.freq_up[pity_since_last_up]++;
                if (!next_guaranteed) s.up_win_pities.push_back(current_pity);
                next_guaranteed = false;
                pity_since_last_up = 0;
            } else {
                next_guaranteed = true; 
            }
            current_pity = 0;
        }
    }
    
    if (!s.all_pities.empty()) s.avg_all = std::accumulate(s.all_pities.begin(), s.all_pities.end(), 0.0) / s.all_pities.size();
    if (!s.up_pities.empty()) s.avg_up = std::accumulate(s.up_pities.begin(), s.up_pities.end(), 0.0) / s.up_pities.size();
    if (!s.up_win_pities.empty()) s.avg_win = std::accumulate(s.up_win_pities.begin(), s.up_win_pities.end(), 0.0) / s.up_win_pities.size();
        
    return s;
}

void ProcessFile(const std::wstring& path) {
    wchar_t charBuf[1024]; GetWindowTextW(hCharEdit, charBuf, 1024);
    std::unordered_set<std::wstring> stdChars = ParseCommaSeparated(charBuf);
    
    wchar_t wepBuf[4096]; GetWindowTextW(hWepEdit, wepBuf, 4096);
    std::unordered_set<std::wstring> stdWeps = ParseCommaSeparated(wepBuf);

    std::ifstream ifs(path);
    if (!ifs.is_open()) return;
    
    std::vector<Pull> pulls;
    try {
        json j; ifs >> j;
        for (auto& item : j["list"]) {
            Pull p;
            p.name = Utf8ToWstring(item.value("name", ""));
            p.item_type = Utf8ToWstring(item.value("item_type", ""));
            p.rank_type = Utf8ToWstring(item.value("rank_type", ""));
            
            // 优化3源头：在只执行一次的解析阶段，一次性将 gacha_type 转为小写并存入结构体
            std::wstring raw_type = Utf8ToWstring(item.value("uigf_gacha_type", ""));
            std::transform(raw_type.begin(), raw_type.end(), raw_type.begin(), ::towlower);
            p.uigf_gacha_type = raw_type;
            
            if (item.contains("id")) {
                if (item["id"].is_string()) p.id = std::stoll(item["id"].get<std::string>());
                else if (item["id"].is_number()) p.id = item["id"].get<long long>();
            } else p.id = 0;
            pulls.push_back(p);
        }
    } catch (...) { 
        SetWindowTextW(hOutEdit, L"JSON 解析失败，请检查文件格式。"); 
        return; 
    }
    
    std::ranges::sort(pulls, [](const Pull& a, const Pull& b){ return std::abs(a.id) < std::abs(b.id); });
    
    statsChar = Calculate(pulls, false, stdChars);
    statsWep = Calculate(pulls, true, stdWeps);
    
    std::wstringstream wss; wss << std::fixed << std::setprecision(2);
    
    wss << L"【角色卡池 (特许寻访)】总计六星: " << statsChar.all_pities.size() << L" | 出当期 UP: " << statsChar.up_pities.size() << L"\r\n";
    wss << L" ▶ 综合六星 (含歪) 出货平均期望: \t" << statsChar.avg_all << L" 抽\r\n";
    wss << L" ▶ 抽到当期限定 UP 的综合平均期望: \t" << statsChar.avg_up << L" 抽\r\n";
    if (statsChar.avg_win >= 0) wss << L" ▶ 赢下小保底 (不歪) 的出货期望: \t\t" << statsChar.avg_win << L" 抽\r\n\r\n";
    else wss << L" ▶ 赢下小保底 (不歪) 的出货期望: \t\t" << L"[无数据]" << L"\r\n\r\n";
    
    wss << L"【武器卡池 (特许武器)】总计六星: " << statsWep.all_pities.size() << L" | 出当期 UP: " << statsWep.up_pities.size() << L"\r\n";
    wss << L" ▶ 综合六星 (含歪) 出货平均期望: \t" << statsWep.avg_all << L" 抽\r\n";
    wss << L" ▶ 抽到当期限定 UP 的综合平均期望: \t" << statsWep.avg_up << L" 抽\r\n";
    if (statsWep.avg_win >= 0) wss << L" ▶ 赢下小保底 (不歪) 的出货期望: \t\t" << statsWep.avg_win << L" 抽\r\n";
    else wss << L" ▶ 赢下小保底 (不歪) 的出货期望: \t\t" << L"[无数据]";
    
    SetWindowTextW(hOutEdit, wss.str().c_str());
}

// 修改参数类型为 const std::unordered_map
void DrawKDE(Gdiplus::Graphics& g, Gdiplus::Rect rect, const std::unordered_map<int, int>& freq_all, const std::unordered_map<int, int>& freq_up, const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255));
    g.FillRectangle(&bgBrush, rect);
    
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScale(15), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont, Gdiplus::PointF(rect.X + DPIScale(15), rect.Y + DPIScale(12)), &textBrush);
    
    if (freq_all.empty() && freq_up.empty()) {
        Gdiplus::Font emptyFont(&fontFamily, DPIScale(14), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush emptyBrush(Gdiplus::Color(255, 150, 150, 150));
        g.DrawString(L"暂无出金数据", -1, &emptyFont, Gdiplus::PointF(rect.X + rect.Width/2 - DPIScale(50), rect.Y + rect.Height/2), &emptyBrush);
        return;
    }

    int max_x = limit_base;
    for (auto const& [v, c] : freq_up) if (v > max_x) max_x = v;
    max_x = ((max_x / 10) + 1) * 10;

    double bandwidth = 4.0; 
    // Lambda 函数参数同步更新
    auto calcKDE = [&](const std::unordered_map<int, int>& freqs) {
        std::vector<double> curve(max_x + 1, 0.0);
        int total = 0; for (auto const& [v, c] : freqs) total += c;
        if (total == 0) return curve;
        for (int x = 1; x <= max_x; x++) {
            double sum = 0;
            for (auto const& [v, c] : freqs) { double u = (x - v) / bandwidth; sum += c * std::exp(-0.5 * u * u); }
            curve[x] = sum / total; 
        }
        return curve;
    };

    auto kde_all = calcKDE(freq_all), kde_up = calcKDE(freq_up);
    double max_y = 0.0001;
    for (double val : kde_all) max_y = max(max_y, val);
    for (double val : kde_up) max_y = max(max_y, val);
    max_y *= 1.25; 

    float padLeft = DPIScaleF(55.0f);
    float padRight = DPIScaleF(30.0f);
    float padBottom = DPIScaleF(30.0f);
    float padTop = DPIScaleF(45.0f);
    
    float plotX = rect.X + padLeft;
    float plotY = rect.Y + padTop;
    float plotW = rect.Width - padLeft - padRight;
    float plotH = rect.Height - padTop - padBottom;

    Gdiplus::Pen axisPen(Gdiplus::Color(255, 150, 150, 150), DPIScaleF(1.5f));
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 235, 235, 235), DPIScaleF(1.0f)); 
    
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY, plotX, plotY + plotH);

    auto getPt = [&](int x, double y) {
        float px = plotX + (float)x / (float)max_x * plotW;
        float py = plotY + plotH - (float)y / max_y * plotH;
        return Gdiplus::PointF(px, py);
    };

    Gdiplus::Font tickFont(&fontFamily, DPIScale(11), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));
    
    int y_steps = 4;
    for (int i = 0; i <= y_steps; ++i) {
        double val = (max_y / y_steps) * i;
        float py = plotY + plotH - (float)i / y_steps * plotH;
        
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        
        std::wstringstream ywss;
        ywss << std::fixed << std::setprecision(1) << (val * 100.0) << L"%";
        std::wstring y_label = ywss.str();
        
        float textOffset = y_label.length() * DPIScaleF(5.5f); 
        g.DrawString(y_label.c_str(), -1, &tickFont, Gdiplus::PointF(plotX - textOffset - DPIScaleF(8.0f), py - DPIScaleF(6.0f)), &tickBrush);
    }

    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        
        std::wstring x_label = std::to_wstring(x);
        float textOffset = (x < 10) ? DPIScaleF(4.0f) : ((x < 100) ? DPIScaleF(8.0f) : DPIScaleF(12.0f));
        g.DrawString(x_label.c_str(), -1, &tickFont, Gdiplus::PointF(px - textOffset, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    if (!freq_all.empty()) {
        std::vector<Gdiplus::PointF> pts; pts.push_back(getPt(0, 0));
        for (int x = 1; x <= max_x; x++) pts.push_back(getPt(x, kde_all[x]));
        Gdiplus::Pen penAll(Gdiplus::Color(255, 65, 140, 240), DPIScaleF(2.5f));
        g.DrawCurve(&penAll, pts.data(), pts.size(), 0.3f);
    }
    
    if (!freq_up.empty()) {
        std::vector<Gdiplus::PointF> pts; pts.push_back(getPt(0, 0));
        for (int x = 1; x <= max_x; x++) pts.push_back(getPt(x, kde_up[x]));
        Gdiplus::Pen penUp(Gdiplus::Color(255, 240, 80, 80), DPIScaleF(2.5f));
        g.DrawCurve(&penUp, pts.data(), pts.size(), 0.3f);
    }
    
    Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 65, 140, 240)), redBrush(Gdiplus::Color(255, 240, 80, 80));
    g.DrawString(L"━━ 综合六星出金分布 (含歪)", -1, &tickFont, Gdiplus::PointF(rect.X + rect.Width - DPIScale(190), rect.Y + DPIScale(15)), &blueBrush);
    g.DrawString(L"━━ 当期限定 UP 出金分布", -1, &tickFont, Gdiplus::PointF(rect.X + rect.Width - DPIScale(190), rect.Y + DPIScale(35)), &redBrush);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE: {
            DragAcceptFiles(hwnd, TRUE);
            HFONT hFont = CreateFontW(DPIScale(17), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
            
            HWND hL1 = CreateWindowW(L"STATIC", L"请确认下方常驻名单（如果游戏更新了新常驻，请用逗号补充）。拖入UIGF(.json)即可分析：", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(15), DPIScale(800), DPIScale(20), hwnd, NULL, NULL, NULL);
            HWND hL_Char = CreateWindowW(L"STATIC", L"常驻角色:", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(45), DPIScale(75), DPIScale(20), hwnd, NULL, NULL, NULL);
            hCharEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"骏卫,黎风,别礼,余烬,艾尔黛拉", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, DPIScale(100), DPIScale(40), DPIScale(710), DPIScale(26), hwnd, NULL, NULL, NULL);

            HWND hL_Wep = CreateWindowW(L"STATIC", L"常驻武器:", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(75), DPIScale(75), DPIScale(20), hwnd, NULL, NULL, NULL);
            hWepEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"宏愿,不知归,黯色火炬,扶摇,热熔切割器,显赫声名,白夜新星,大雷斑,赫拉芬格,典范,昔日精品,破碎君王,J.E.T.,骁勇,负山,同类相食,楔子,领航者,骑士精神,遗忘,爆破单元,作品：蚀迹,沧溟星梦,光荣记忆,望乡", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, DPIScale(100), DPIScale(70), DPIScale(710), DPIScale(26), hwnd, NULL, NULL, NULL);
            
            hOutEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"等待拖入文件...", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, DPIScale(20), DPIScale(105), DPIScale(790), DPIScale(180), hwnd, NULL, NULL, NULL);
            
            SendMessage(hL1, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hL_Char, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hCharEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hL_Wep, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hWepEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hOutEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            break;
        }
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam; wchar_t filePath[MAX_PATH];
            DragQueryFileW(hDrop, 0, filePath, MAX_PATH); DragFinish(hDrop);
            ProcessFile(filePath); InvalidateRect(hwnd, NULL, TRUE);
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            Gdiplus::Graphics g(hdc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            DrawKDE(g, Gdiplus::Rect(DPIScale(20), DPIScale(295), DPIScale(790), DPIScale(250)), statsChar.freq_all, statsChar.freq_up, L"终末地角色抽取期望 - 核密度分布", 130);
            DrawKDE(g, Gdiplus::Rect(DPIScale(20), DPIScale(555), DPIScale(790), DPIScale(250)), statsWep.freq_all, statsWep.freq_up, L"终末地武器抽取期望 - 核密度分布", 80);
            EndPaint(hwnd, &ps);
            break;
        }
        case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetProcessDPIAware(); 
    HDC hdcScreen = GetDC(NULL);
    g_dpi = GetDeviceCaps(hdcScreen, LOGPIXELSX);
    ReleaseDC(NULL, hdcScreen);

    ULONG_PTR gdiplusToken; Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASSW wc = {0}; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"EndfieldStatsClass"; RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"终末地抽卡记录分析与可视化", WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, DPIScale(850), DPIScale(910), NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
