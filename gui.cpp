#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdio> 
#include <string_view>
#include <charconv>
#include <ranges>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------
// [极简 JSON 模块 - 零分配纯净版 + 内存映射支持]
// ---------------------------------------------------------
size_t FindJsonKey(std::string_view source, std::string_view key, size_t startPos = 0) {
    while (true) {
        size_t pos = source.find(key, startPos);
        if (pos == std::string_view::npos) return std::string_view::npos;
        if (pos > 0 && source[pos - 1] == '"' && 
            (pos + key.length() < source.length()) && source[pos + key.length()] == '"') {
            return pos - 1; 
        }
        startPos = pos + key.length();
    }
}

std::string_view ExtractJsonValue(std::string_view source, std::string_view key, bool isString) {
    size_t pos = FindJsonKey(source, key);
    if (pos == std::string_view::npos) return {};
    pos = source.find(':', pos + key.length() + 2);
    if (pos == std::string_view::npos) return {};
    pos++; 
    while (pos < source.length() && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\n' || source[pos] == '\r')) pos++;
    
    if (isString) {
        if (source[pos] != '"') return {};
        pos++; 
        auto endPos = pos;
        while (endPos < source.length() && source[endPos] != '"') {
            if (source[endPos] == '\\') endPos++; 
            endPos++;
        }
        return (endPos < source.length()) ? source.substr(pos, endPos - pos) : std::string_view{};
    } else {
        auto endPos = pos;
        while (endPos < source.length() && source[endPos] != ',' && source[endPos] != '}' && source[endPos] != ' ' && source[endPos] != '\n' && source[endPos] != '\r') endPos++;
        return source.substr(pos, endPos - pos);
    }
}

template<typename Callback>
void ForEachJsonObject(std::string_view source, std::string_view arrayKey, Callback&& cb) {
    size_t pos = FindJsonKey(source, arrayKey);
    if (pos == std::string_view::npos) return;
    pos = source.find(':', pos + arrayKey.length() + 2);
    if (pos == std::string_view::npos) return;
    pos = source.find('[', pos);
    if (pos == std::string_view::npos) return;
    
    int depth = 0;
    size_t objStart = 0;
    for (size_t i = pos; i < source.length(); ++i) {
        if (source[i] == '{') {
            if (depth == 0) objStart = i;
            depth++;
        } else if (source[i] == '}') {
            depth--;
            if (depth == 0) cb(source.substr(objStart, i - objStart + 1));
        } else if (source[i] == ']' && depth == 0) break;
    }
}
// ---------------------------------------------------------

int g_dpi = 96;
int DPIScale(int value) { return MulDiv(value, g_dpi, 96); }
float DPIScaleF(float value) { return value * (g_dpi / 96.0f); }

std::wstring Utf8ToWstring(std::string_view str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

struct Pull {
    std::wstring name, item_type, rank_type, uigf_gacha_type, poolName;
    long long id;
};

struct Stats {
    std::vector<int> all_pities, up_pities, up_win_pities;  
    double avg_all = 0.0, avg_up = 0.0, avg_win = -1.0; 
    std::unordered_map<int, int> freq_all, freq_up;
};

Stats statsChar, statsWep;
HWND hOutEdit, hCharEdit, hWepEdit, hPoolMapEdit;

// 解析逗号分隔的常驻名单
std::unordered_set<std::wstring> ParseCommaSeparated(const std::wstring& text) {
    std::unordered_set<std::wstring> result;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L'，') {
            if (!cur.empty()) {
                cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
                if (!cur.empty()) result.insert(cur);
            }
            cur.clear();
        } else cur += c;
    }
    if (!cur.empty()) {
        cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
        if (!cur.empty()) result.insert(cur);
    }
    return result;
}

// 解析 "卡池:角色" 的键值对映射
std::unordered_map<std::wstring, std::wstring> ParsePoolMap(const std::wstring& text) {
    std::unordered_map<std::wstring, std::wstring> result;
    std::wstring cur_pool, cur_up, cur;
    bool reading_up = false;
    for (wchar_t c : text) {
        if ((c == L':' || c == L'：') && !reading_up) {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
            cur_pool = cur; cur.clear(); reading_up = true;
        } else if (c == L',' || c == L'，') {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
            cur_up = cur;
            if (!cur_pool.empty() && !cur_up.empty()) result[cur_pool] = cur_up;
            cur.clear(); cur_pool.clear(); cur_up.clear(); reading_up = false;
        } else cur += c;
    }
    if (reading_up) {
        cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
        cur_up = cur;
        if (!cur_pool.empty() && !cur_up.empty()) result[cur_pool] = cur_up;
    }
    return result;
}

Stats Calculate(const std::vector<Pull>& pulls, bool isWeapon, const std::unordered_set<std::wstring>& standard_names, const std::unordered_map<std::wstring, std::wstring>& pool_map) {
    Stats s;
    int current_pity = 0, pity_since_last_up = 0;
    bool next_guaranteed = false;
    
    for (const auto& p : pulls) {
        bool isSpecial = false;
        if (isWeapon) {
            if (p.item_type != L"Weapon") continue;
            if (p.uigf_gacha_type.find(L"constant") == std::wstring::npos && p.uigf_gacha_type.find(L"standard") == std::wstring::npos && p.uigf_gacha_type.find(L"beginner") == std::wstring::npos)
                isSpecial = true;
        } else {
            if (p.item_type != L"Character") continue;
            if (p.uigf_gacha_type.find(L"special") != std::wstring::npos) isSpecial = true;
        }
        
        if (!isSpecial) continue;
        
        current_pity++; pity_since_last_up++;
        
        if (p.rank_type == L"6") {
            s.all_pities.push_back(current_pity); s.freq_all[current_pity]++;
            
            bool isUP = false;
            // 核心逻辑：尝试匹配卡池，未匹配到（包含武器池传空表的情况）直接 fallback 到主判断逻辑
            auto it = pool_map.find(p.poolName);
            if (it != pool_map.end()) {
                isUP = (p.name == it->second);
            } else {
                isUP = !standard_names.contains(p.name);
            }
            
            if (isUP) {
                s.up_pities.push_back(pity_since_last_up); s.freq_up[pity_since_last_up]++;
                if (!next_guaranteed) s.up_win_pities.push_back(current_pity);
                next_guaranteed = false; pity_since_last_up = 0;
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
    
    wchar_t poolBuf[2048]; GetWindowTextW(hPoolMapEdit, poolBuf, 2048);
    std::unordered_map<std::wstring, std::wstring> poolMap = ParsePoolMap(poolBuf);
    
    wchar_t wepBuf[4096]; GetWindowTextW(hWepEdit, wepBuf, 4096);
    std::unordered_set<std::wstring> stdWeps = ParseCommaSeparated(wepBuf);

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) { CloseHandle(hFile); return; }
    HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) { CloseHandle(hFile); return; }
    const char* mapData = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!mapData) { CloseHandle(hMap); CloseHandle(hFile); return; }

    std::string_view bufferView(mapData, fileSize);
    std::vector<Pull> pulls; pulls.reserve(1000); 

    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
        Pull p;
        p.name = Utf8ToWstring(ExtractJsonValue(itemStr, "name", true));
        p.item_type = Utf8ToWstring(ExtractJsonValue(itemStr, "item_type", true));
        p.rank_type = Utf8ToWstring(ExtractJsonValue(itemStr, "rank_type", true));
        
        // 兼容不同导出工具：poolName, gacha_name 或 poolname
        p.poolName = Utf8ToWstring(ExtractJsonValue(itemStr, "poolName", true));
        if (p.poolName.empty()) p.poolName = Utf8ToWstring(ExtractJsonValue(itemStr, "gacha_name", true));
        if (p.poolName.empty()) p.poolName = Utf8ToWstring(ExtractJsonValue(itemStr, "poolname", true));
        
        std::wstring raw_type = Utf8ToWstring(ExtractJsonValue(itemStr, "uigf_gacha_type", true));
        std::ranges::transform(raw_type, raw_type.begin(), ::towlower);
        p.uigf_gacha_type = raw_type;
        
        std::string_view idStr = ExtractJsonValue(itemStr, "id", true);
        if (idStr.empty()) idStr = ExtractJsonValue(itemStr, "id", false);
        long long parsed_id = 0;
        if (!idStr.empty()) std::from_chars(idStr.data(), idStr.data() + idStr.size(), parsed_id);
        p.id = parsed_id;
        
        pulls.push_back(std::move(p));
    });
    
    UnmapViewOfFile(mapData); CloseHandle(hMap); CloseHandle(hFile);
    if (pulls.empty()) { SetWindowTextW(hOutEdit, L"JSON 解析失败或无数据。"); return; }
    
    std::ranges::sort(pulls, {}, [](const Pull& p){ return std::abs(p.id); });
    
    statsChar = Calculate(pulls, false, stdChars, poolMap);
    // 武器池直接传入空表，自然触发兼容/排除模式
    std::unordered_map<std::wstring, std::wstring> emptyMap;
    statsWep = Calculate(pulls, true, stdWeps, emptyMap); 
    
    wchar_t winCharStr[64] = L"[无数据]";
    if (statsChar.avg_win >= 0) swprintf(winCharStr, 64, L"%.2f 抽", statsChar.avg_win);
    wchar_t winWepStr[64] = L"[无数据]";
    if (statsWep.avg_win >= 0) swprintf(winWepStr, 64, L"%.2f 抽", statsWep.avg_win);

    wchar_t outMsg[2048];
    swprintf(outMsg, 2048, 
        L"【角色卡池 (特许寻访)】总计六星: %zu | 出当期 UP: %zu\r\n"
        L" ▶ 综合六星 (含歪) 出货平均期望: \t%.2f 抽\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望: \t%.2f 抽\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望: \t\t%ls\r\n\r\n"
        L"【武器卡池 (武库申领)】总计六星: %zu | 出当期 UP: %zu\r\n"
        L" ▶ 综合六星 (含歪) 出货平均期望: \t%.2f 抽\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望: \t%.2f 抽\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望: \t\t%ls",
        statsChar.all_pities.size(), statsChar.up_pities.size(), statsChar.avg_all, statsChar.avg_up, winCharStr,
        statsWep.all_pities.size(), statsWep.up_pities.size(), statsWep.avg_all, statsWep.avg_up, winWepStr
    );
    SetWindowTextW(hOutEdit, outMsg);
}

void DrawKDE(Gdiplus::Graphics& g, Gdiplus::Rect rect, const std::unordered_map<int, int>& freq_all, const std::unordered_map<int, int>& freq_up, const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255)); g.FillRectangle(&bgBrush, rect);
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

    float plotX = rect.X + DPIScaleF(55.0f), plotY = rect.Y + DPIScaleF(45.0f);
    float plotW = rect.Width - DPIScaleF(85.0f), plotH = rect.Height - DPIScaleF(75.0f);
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 150, 150, 150), DPIScaleF(1.5f));
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 235, 235, 235), DPIScaleF(1.0f)); 
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY, plotX, plotY + plotH);

    auto getPt = [&](int x, double y) { return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW, plotY + plotH - (float)y / max_y * plotH); };
    Gdiplus::Font tickFont(&fontFamily, DPIScale(11), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));
    
    for (int i = 0; i <= 4; ++i) {
        float py = plotY + plotH - (float)i / 4.0f * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[32]; swprintf(y_label, 32, L"%.1f%%", (max_y / 4.0) * i * 100.0);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - wcslen(y_label)*DPIScaleF(5.5f) - DPIScaleF(8.0f), py - DPIScaleF(6.0f)), &tickBrush);
    }
    for (int x = 0; x <= max_x; x += (max_x > 140 ? 20 : 10)) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        g.DrawString(x_label, -1, &tickFont, Gdiplus::PointF(px - (x<10?4.0f:x<100?8.0f:12.0f)*DPIScaleF(1.0f), plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    if (!freq_all.empty()) {
        std::vector<Gdiplus::PointF> pts; pts.push_back(getPt(0, 0));
        for (int x = 1; x <= max_x; x++) pts.push_back(getPt(x, kde_all[x]));
        Gdiplus::Pen penAll(Gdiplus::Color(255, 65, 140, 240), DPIScaleF(2.5f)); g.DrawCurve(&penAll, pts.data(), pts.size(), 0.3f);
    }
    if (!freq_up.empty()) {
        std::vector<Gdiplus::PointF> pts; pts.push_back(getPt(0, 0));
        for (int x = 1; x <= max_x; x++) pts.push_back(getPt(x, kde_up[x]));
        Gdiplus::Pen penUp(Gdiplus::Color(255, 240, 80, 80), DPIScaleF(2.5f)); g.DrawCurve(&penUp, pts.data(), pts.size(), 0.3f);
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
            
            HWND hL1 = CreateWindowW(L"STATIC", L"支持“限定角色卡池:当期UP角色”映射。未包含的限定角色卡池将仅排查常驻六星角色名单。", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(15), DPIScale(800), DPIScale(20), hwnd, NULL, NULL, NULL);
            
            HWND hL_Char = CreateWindowW(L"STATIC", L"常驻六星角色:", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(45), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
            hCharEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"骏卫,黎风,别礼,余烬,艾尔黛拉", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, DPIScale(120), DPIScale(40), DPIScale(690), DPIScale(26), hwnd, NULL, NULL, NULL);

            HWND hL_PoolMap = CreateWindowW(L"STATIC", L"当期UP角色:", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(75), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
            hPoolMapEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"熔火灼痕:莱万汀,轻飘飘的信使:洁尔佩塔,热烈色彩:伊冯,河流的女儿:汤汤,狼珀:洛茜", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, DPIScale(120), DPIScale(70), DPIScale(690), DPIScale(26), hwnd, NULL, NULL, NULL);

            HWND hL_Wep = CreateWindowW(L"STATIC", L"常驻六星武器:", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(105), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
            hWepEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"宏愿,不知归,黯色火炬,扶摇,热熔切割器,显赫声名,白夜新星,大雷斑,赫拉芬格,典范,昔日精品,破碎君王,J.E.T.,骁勇,负山,同类相食,楔子,领航者,骑士精神,遗忘,爆破单元,作品：蚀迹,沧溟星梦,光荣记忆,望乡", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, DPIScale(120), DPIScale(100), DPIScale(690), DPIScale(26), hwnd, NULL, NULL, NULL);

            hOutEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"等待拖入文件...", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, DPIScale(20), DPIScale(135), DPIScale(790), DPIScale(160), hwnd, NULL, NULL, NULL);
            
            SendMessage(hL1, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hL_Char, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hCharEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hL_PoolMap, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hPoolMapEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hL_Wep, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hWepEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
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
            DrawKDE(g, Gdiplus::Rect(DPIScale(20), DPIScale(305), DPIScale(790), DPIScale(250)), statsChar.freq_all, statsChar.freq_up, L"终末地角色抽取期望 - 核密度分布", 130);
            DrawKDE(g, Gdiplus::Rect(DPIScale(20), DPIScale(565), DPIScale(790), DPIScale(250)), statsWep.freq_all, statsWep.freq_up, L"终末地武器抽取期望 - 核密度分布", 80);
            EndPaint(hwnd, &ps);
            break;
        }
        case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetProcessDPIAware(); 
    HDC hdcScreen = GetDC(NULL); g_dpi = GetDeviceCaps(hdcScreen, LOGPIXELSX); ReleaseDC(NULL, hdcScreen);

    ULONG_PTR gdiplusToken; Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASSW wc = {0}; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"EndfieldStatsClass"; RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"终末地抽卡记录分析与可视化", WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, DPIScale(850), DPIScale(930), NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
