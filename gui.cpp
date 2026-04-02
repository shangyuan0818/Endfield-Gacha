#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
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

// #pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

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
        if (pos >= source.length() || source[pos] != '"') return {};
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
        char c = source[i];
        if (c == '"') {
            for (++i; i < source.length(); ++i) {
                if (source[i] == '\\') { ++i; continue; }
                if (source[i] == '"') break;
            }
            continue;
        }
        if (c == '{') {
            if (depth == 0) objStart = i;
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) cb(source.substr(objStart, i - objStart + 1));
        } else if (c == ']' && depth == 0) break;
    }
}
// ---------------------------------------------------------

int g_dpi = 96;
int DPIScale(int value) { return MulDiv(value, g_dpi, 96); }
float DPIScaleF(float value) { return value * (g_dpi / 96.0f); }

std::wstring Utf8ToWstring(std::string_view str) {
    if (str.empty()) return {};
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
    
    double std_all = 0.0, std_up = 0.0;
    double cv_all = 0.0, cv_up = 0.0;           
    double ci_all_err = 0.0, ci_up_err = 0.0;   
    int win_5050 = 0, lose_5050 = 0;            
    double win_rate_5050 = -1.0;                
    
    int max_pity_all = 0, max_pity_up = 0;
    std::vector<double> hazard_all, hazard_up;  
    
    double ks_d_all = 0.0;
    bool ks_is_normal = true; 
};

Stats statsChar, statsWep;
HWND hOutEdit, hCharEdit, hWepEdit, hPoolMapEdit;
static HBITMAP g_hChartBmp = NULL;  // 预渲染图表缓存 (消灭 WM_PAINT 卡顿)

std::unordered_set<std::wstring> ParseCommaSeparated(const std::wstring& text) {
    std::unordered_set<std::wstring> result; std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L'\uFF0C') {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n")); 
            if (!cur.empty()) cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
            if (!cur.empty()) result.insert(cur);
            cur.clear();
        } else cur += c;
    }
    cur.erase(0, cur.find_first_not_of(L" \t\r\n")); 
    if (!cur.empty()) cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
    if (!cur.empty()) result.insert(cur);
    return result;
}

std::unordered_map<std::wstring, std::wstring> ParsePoolMap(const std::wstring& text) {
    std::unordered_map<std::wstring, std::wstring> result; std::wstring cur_pool, cur_up, cur; bool reading_up = false;
    for (wchar_t c : text) {
        if ((c == L':' || c == L'\uFF1A') && !reading_up) {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1); cur_pool = cur; cur.clear(); reading_up = true;
        } else if (c == L',' || c == L'\uFF0C') {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1); cur_up = cur;
            if (!cur_pool.empty() && !cur_up.empty()) result[cur_pool] = cur_up;
            cur.clear(); cur_pool.clear(); cur_up.clear(); reading_up = false;
        } else cur += c;
    }
    if (reading_up) {
        cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1); cur_up = cur;
        if (!cur_pool.empty() && !cur_up.empty()) result[cur_pool] = cur_up;
    }
    return result;
}

// -------------------------------------------------------
// 理论 CDF 预计算表 (启动时一次性生成, 后续 O(1) 查表)
// 角色池: 0.8% (1~65), 5.8%+5%/抽 (66~79), 100% (80)
// 武器池: 4% (1~39), 100% (40)
// -------------------------------------------------------
static double g_cdf_char[81] = {};  // [0]=0, [1..80]=CDF
static double g_cdf_wep[41]  = {};  // [0]=0, [1..40]=CDF

void InitCDFTables() {
    double surv = 1.0;
    for (int i = 1; i <= 80; ++i) {
        double p = (i <= 65) ? 0.008 : (i <= 79) ? 0.058 + (i - 66) * 0.05 : 1.0;
        g_cdf_char[i] = g_cdf_char[i-1] + surv * p;
        surv *= (1.0 - p);
    }
    surv = 1.0;
    for (int i = 1; i <= 40; ++i) {
        double p = (i >= 40) ? 1.0 : 0.04;
        g_cdf_wep[i] = g_cdf_wep[i-1] + surv * p;
        surv *= (1.0 - p);
    }
}

// -------------------------------------------------------
// 基于频率表的 K-S 检验 — O(max_pity), CDF 查表 O(1)
// -------------------------------------------------------
double ComputeKS(const std::unordered_map<int, int>& freq, int max_pity, int n, const double* cdf_table, int cdf_len) {
    if (n == 0) return 0.0;
    double max_d = 0.0;
    int cum_count = 0;
    for (int x = 1; x <= max_pity; ++x) {
        auto it = freq.find(x);
        int count_x = (it != freq.end()) ? it->second : 0;
        double f_val = (x < cdf_len) ? cdf_table[x] : 1.0;
        double fn_before = (double)cum_count / n;
        cum_count += count_x;
        double fn_after  = (double)cum_count / n;
        double d1 = std::abs(fn_before - f_val);
        double d2 = std::abs(fn_after  - f_val);
        if (d1 > max_d) max_d = d1;
        if (d2 > max_d) max_d = d2;
    }
    return max_d;
}

Stats Calculate(const std::vector<Pull>& pulls, bool isWeapon, const std::unordered_set<std::wstring>& standard_names, const std::unordered_map<std::wstring, std::wstring>& pool_map) {
    Stats s;
    int current_pity = 0, pity_since_last_up = 0;
    bool had_non_up = false;
    
    // 在线累加器: 避免后续额外遍历
    long long sum_all = 0, sum_sq_all = 0;
    long long sum_up = 0, sum_sq_up = 0;
    long long sum_win = 0;
    
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
            if (current_pity > s.max_pity_all) s.max_pity_all = current_pity;
            sum_all += current_pity; sum_sq_all += (long long)current_pity * current_pity;
            
            bool isUP = false;
            auto it = pool_map.find(p.poolName);
            if (it != pool_map.end()) isUP = (p.name == it->second);
            else isUP = !standard_names.contains(p.name);
            
            if (isUP) {
                s.up_pities.push_back(pity_since_last_up); s.freq_up[pity_since_last_up]++;
                if (pity_since_last_up > s.max_pity_up) s.max_pity_up = pity_since_last_up;
                sum_up += pity_since_last_up; sum_sq_up += (long long)pity_since_last_up * pity_since_last_up;
                if (!had_non_up) { 
                    s.up_win_pities.push_back(current_pity); 
                    s.win_5050++; 
                    sum_win += current_pity; 
                }
                had_non_up = false; pity_since_last_up = 0;
            } else {
                if (!had_non_up) s.lose_5050++; 
                had_non_up = true; 
            }
            current_pity = 0;
        }
    }
    
    // --- 统计量计算 (直接从在线累加器得出, 无需额外遍历) ---
    size_t n_all = s.all_pities.size();
    size_t n_up  = s.up_pities.size();
    size_t n_win = s.up_win_pities.size();
    
    if (n_all > 0) {
        s.avg_all = (double)sum_all / n_all;
        double var = (double)sum_sq_all / n_all - s.avg_all * s.avg_all;
        s.std_all = std::sqrt(var > 0 ? var : 0);
        s.cv_all = (s.avg_all > 0) ? s.std_all / s.avg_all : 0;
        s.ci_all_err = 1.96 * s.std_all / std::sqrt((double)n_all);
        
        // 经验风险函数 (hazard)
        s.hazard_all.resize(s.max_pity_all + 1, 0.0);
        int survivors = (int)n_all;
        for (int x = 1; x <= s.max_pity_all; ++x) {
            if (survivors > 0) {
                auto it = s.freq_all.find(x);
                int cnt = (it != s.freq_all.end()) ? it->second : 0;
                s.hazard_all[x] = (double)cnt / survivors;
                survivors -= cnt;
            }
        }
        
        // K-S 检验: O(max_pity), CDF 查表 O(1)
        const double* cdf_tbl = isWeapon ? g_cdf_wep : g_cdf_char;
        int cdf_len = isWeapon ? 41 : 81;
        s.ks_d_all = ComputeKS(s.freq_all, s.max_pity_all, (int)n_all, cdf_tbl, cdf_len);
        double d_crit = 1.36 / std::sqrt((double)n_all);
        s.ks_is_normal = (s.ks_d_all <= d_crit); 
    }
    
    if (n_up > 0) {
        s.avg_up = (double)sum_up / n_up;
        double var = (double)sum_sq_up / n_up - s.avg_up * s.avg_up;
        s.std_up = std::sqrt(var > 0 ? var : 0);
        s.cv_up = (s.avg_up > 0) ? s.std_up / s.avg_up : 0;
        s.ci_up_err = 1.96 * s.std_up / std::sqrt((double)n_up);
        
        s.hazard_up.resize(s.max_pity_up + 1, 0.0);
        int survivors = (int)n_up;
        for (int x = 1; x <= s.max_pity_up; ++x) {
            if (survivors > 0) {
                auto it = s.freq_up.find(x);
                int cnt = (it != s.freq_up.end()) ? it->second : 0;
                s.hazard_up[x] = (double)cnt / survivors;
                survivors -= cnt;
            }
        }
    }
    
    if (n_win > 0) s.avg_win = (double)sum_win / n_win;
    if (s.win_5050 + s.lose_5050 > 0) s.win_rate_5050 = (double)s.win_5050 / (s.win_5050 + s.lose_5050);
        
    return s;
}

void ProcessFile(const std::wstring& path) {
    wchar_t charBuf[1024]; GetWindowTextW(hCharEdit, charBuf, 1024);
    std::unordered_set<std::wstring> stdChars = ParseCommaSeparated(charBuf);
    
    wchar_t poolBuf[4096]; GetWindowTextW(hPoolMapEdit, poolBuf, 4096);
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
    
    // 跳过 UTF-8 BOM
    if (bufferView.size() >= 3 &&
        (unsigned char)bufferView[0] == 0xEF &&
        (unsigned char)bufferView[1] == 0xBB &&
        (unsigned char)bufferView[2] == 0xBF) {
        bufferView.remove_prefix(3);
    }
    
    std::vector<Pull> pulls; pulls.reserve(1000); 

    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
        Pull p;
        p.name = Utf8ToWstring(ExtractJsonValue(itemStr, "name", true));
        p.item_type = Utf8ToWstring(ExtractJsonValue(itemStr, "item_type", true));
        p.rank_type = Utf8ToWstring(ExtractJsonValue(itemStr, "rank_type", true));
        
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
    
    // 按 abs(id) 排序: 武器ID为负避免与角色ID冲突
    std::ranges::sort(pulls, {}, [](const Pull& p){ return std::abs(p.id); });
    
    statsChar = Calculate(pulls, false, stdChars, poolMap);
    statsWep = Calculate(pulls, true, stdWeps, {}); 
    
    wchar_t winCharStr[64] = L"[无数据]";
    if (statsChar.avg_win >= 0) swprintf(winCharStr, 64, L"%.2f 抽", statsChar.avg_win);
    wchar_t winWepStr[64] = L"[无数据]";
    if (statsWep.avg_win >= 0) swprintf(winWepStr, 64, L"%.2f 抽", statsWep.avg_win);

    wchar_t outMsg[2048];
    swprintf(outMsg, 2048, 
        L"【角色卡池 (特许寻访)】 总计六星: %zu | 出当期 UP: %zu\r\n"
        L" ▶ 综合六星 (含歪) 出货平均期望:     %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (%d胜%d负)\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\r\n\r\n"
        L"【武器卡池 (武库申领)】 总计六星: %zu | 出当期 UP: %zu\r\n"
        L" ▶ 综合六星 (含歪) 出货平均期望:     %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (%d胜%d负)\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls",
        statsChar.all_pities.size(), statsChar.up_pities.size(), 
        statsChar.avg_all, (std::max)(1.0, statsChar.avg_all - statsChar.ci_all_err), statsChar.avg_all + statsChar.ci_all_err, statsChar.cv_all * 100.0, statsChar.ks_d_all, (statsChar.all_pities.empty() ? L"-" : (statsChar.ks_is_normal ? L"符合理论模型" : L"拒绝原假设: 偏离过大")),
        statsChar.avg_up, (std::max)(1.0, statsChar.avg_up - statsChar.ci_up_err), statsChar.avg_up + statsChar.ci_up_err, statsChar.win_rate_5050 >= 0 ? statsChar.win_rate_5050 * 100.0 : 0.0, statsChar.win_5050, statsChar.lose_5050, winCharStr,
        statsWep.all_pities.size(), statsWep.up_pities.size(), 
        statsWep.avg_all, (std::max)(1.0, statsWep.avg_all - statsWep.ci_all_err), statsWep.avg_all + statsWep.ci_all_err, statsWep.cv_all * 100.0, statsWep.ks_d_all, (statsWep.all_pities.empty() ? L"-" : (statsWep.ks_is_normal ? L"符合理论模型" : L"拒绝原假设: 偏离过大")),
        statsWep.avg_up, (std::max)(1.0, statsWep.avg_up - statsWep.ci_up_err), statsWep.avg_up + statsWep.ci_up_err, statsWep.win_rate_5050 >= 0 ? statsWep.win_rate_5050 * 100.0 : 0.0, statsWep.win_5050, statsWep.lose_5050, winWepStr
    );
    SetWindowTextW(hOutEdit, outMsg);
}

void DrawKDE(Gdiplus::Graphics& g, Gdiplus::Rect rect, const std::unordered_map<int, int>& freq_all, const std::unordered_map<int, int>& freq_up, const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255)); g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont, Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f), (float)rect.Y + DPIScaleF(12.0f)), &textBrush);
    
    if (freq_all.empty() && freq_up.empty()) {
        Gdiplus::Font emptyFont(&fontFamily, DPIScaleF(14.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush emptyBrush(Gdiplus::Color(255, 150, 150, 150));
        g.DrawString(L"暂无出金数据", -1, &emptyFont, Gdiplus::PointF((float)rect.X + (float)rect.Width/2.0f - DPIScaleF(50.0f), (float)rect.Y + (float)rect.Height/2.0f), &emptyBrush);
        return;
    }

    // FIX: 同时遍历 freq_all 和 freq_up 确定 max_x
    int max_x = limit_base;
    for (auto const& [v, c] : freq_all) if (v > max_x) max_x = v;
    for (auto const& [v, c] : freq_up) if (v > max_x) max_x = v;
    max_x = ((max_x / 10) + 1) * 10;
    double bandwidth = 4.0; 
    
    // KDE 散射法: 对每个 freq entry 向两侧扩散, O(|freq| * spread)
    // 比原始聚集法 O(max_x * |freq|) 快 ~10x (freq 通常只有 ~10 entry, spread ~32)
    auto calcKDE = [&](const std::unordered_map<int, int>& freqs) {
        std::vector<double> curve(max_x + 1, 0.0);
        int total = 0; for (auto const& [v, c] : freqs) total += c;
        if (total == 0) return curve;
        int spread = (int)(4.0 * bandwidth) + 1; // 4σ 截断
        for (auto const& [v, c] : freqs) {
            int lo = (std::max)(1, v - spread), hi = (std::min)(max_x, v + spread);
            for (int x = lo; x <= hi; ++x) {
                double u = (x - v) / bandwidth;
                curve[x] += c * std::exp(-0.5 * u * u);
            }
        }
        double inv_total = 1.0 / total;
        for (int x = 1; x <= max_x; ++x) curve[x] *= inv_total;
        return curve;
    };

    auto kde_all = calcKDE(freq_all), kde_up = calcKDE(freq_up);
    double max_y = 0.0001;
    for (double val : kde_all) max_y = (std::max)(max_y, val);
    for (double val : kde_up) max_y = (std::max)(max_y, val);
    max_y *= 1.25; 

    float plotX = (float)rect.X + DPIScaleF(55.0f), plotY = (float)rect.Y + DPIScaleF(45.0f);
    float plotW = (float)rect.Width - DPIScaleF(85.0f), plotH = (float)rect.Height - DPIScaleF(75.0f);
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 150, 150, 150), DPIScaleF(1.5f));
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 235, 235, 235), DPIScaleF(1.0f)); 
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY, plotX, plotY + plotH);

    auto getPt = [&](int x, double y) { return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW, plotY + plotH - (float)(y / max_y) * plotH); };
    Gdiplus::Font tickFont(&fontFamily, DPIScaleF(11.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));
    
    for (int i = 0; i <= 4; ++i) {
        float py = plotY + plotH - (float)i / 4.0f * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[32]; swprintf(y_label, 32, L"%.1f%%", (max_y / 4.0) * i * 100.0);
        float labelW = (float)wcslen(y_label) * DPIScaleF(5.5f) + DPIScaleF(8.0f);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - labelW, py - DPIScaleF(6.0f)), &tickBrush);
    }
    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        float xoff = (x < 10 ? 4.0f : x < 100 ? 8.0f : 12.0f) * DPIScaleF(1.0f);
        g.DrawString(x_label, -1, &tickFont, Gdiplus::PointF(px - xoff, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    if (!freq_all.empty()) {
        std::vector<Gdiplus::PointF> pts; pts.push_back(getPt(0, 0));
        for (int x = 1; x <= max_x; x++) pts.push_back(getPt(x, kde_all[x]));
        Gdiplus::Pen penAll(Gdiplus::Color(255, 65, 140, 240), DPIScaleF(2.5f)); g.DrawCurve(&penAll, pts.data(), (int)pts.size(), 0.3f);
    }
    if (!freq_up.empty()) {
        std::vector<Gdiplus::PointF> pts; pts.push_back(getPt(0, 0));
        for (int x = 1; x <= max_x; x++) pts.push_back(getPt(x, kde_up[x]));
        Gdiplus::Pen penUp(Gdiplus::Color(255, 240, 80, 80), DPIScaleF(2.5f)); g.DrawCurve(&penUp, pts.data(), (int)pts.size(), 0.3f);
    }
    
    Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 65, 140, 240)), redBrush(Gdiplus::Color(255, 240, 80, 80));
    float legendX = (float)rect.X + (float)rect.Width - DPIScaleF(190.0f);
    g.DrawString(L"━━ 综合六星出金分布 (含歪)", -1, &tickFont, Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(15.0f)), &blueBrush);
    g.DrawString(L"━━ 当期限定 UP 出金分布", -1, &tickFont, Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(35.0f)), &redBrush);
}

void DrawHazard(Gdiplus::Graphics& g, Gdiplus::Rect rect, const std::vector<double>& hazard_all, const std::vector<double>& hazard_up, const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255)); g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont, Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f), (float)rect.Y + DPIScaleF(12.0f)), &textBrush);
    
    if (hazard_all.empty() && hazard_up.empty()) return;

    int max_x = limit_base;
    if (!hazard_all.empty() && (int)hazard_all.size() - 1 > max_x) max_x = (int)hazard_all.size() - 1;
    if (!hazard_up.empty() && (int)hazard_up.size() - 1 > max_x) max_x = (int)hazard_up.size() - 1;
    max_x = ((max_x / 10) + 1) * 10;

    double max_y = 0.1;
    for (double v : hazard_all) max_y = (std::max)(max_y, v);
    for (double v : hazard_up) max_y = (std::max)(max_y, v);
    if (max_y > 0.8) max_y = 1.05; 
    else max_y = (std::ceil(max_y * 10)) / 10.0 + 0.1;

    float plotX = (float)rect.X + DPIScaleF(55.0f), plotY = (float)rect.Y + DPIScaleF(45.0f);
    float plotW = (float)rect.Width - DPIScaleF(85.0f), plotH = (float)rect.Height - DPIScaleF(75.0f);
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 150, 150, 150), DPIScaleF(1.5f));
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 235, 235, 235), DPIScaleF(1.0f)); 
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY, plotX, plotY + plotH);

    auto getPt = [&](int x, double y) { return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW, plotY + plotH - (float)(y / max_y) * plotH); };
    Gdiplus::Font tickFont(&fontFamily, DPIScaleF(11.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));
    
    for (int i = 0; i <= 4; ++i) {
        float py = plotY + plotH - (float)i / 4.0f * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[32]; swprintf(y_label, 32, L"%.0f%%", (max_y / 4.0) * i * 100.0);
        float labelW = (float)wcslen(y_label) * DPIScaleF(5.5f) + DPIScaleF(8.0f);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - labelW, py - DPIScaleF(6.0f)), &tickBrush);
    }
    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        float xoff = (x < 10 ? 4.0f : x < 100 ? 8.0f : 12.0f) * DPIScaleF(1.0f);
        g.DrawString(x_label, -1, &tickFont, Gdiplus::PointF(px - xoff, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    float barW = (std::max)(1.5f, plotW / max_x * 0.4f);
    if (hazard_all.size() > 1) {
        Gdiplus::SolidBrush brushAll(Gdiplus::Color(180, 65, 140, 240)); 
        for (size_t x = 1; x < hazard_all.size(); x++) {
            if (hazard_all[x] > 0) {
                Gdiplus::PointF p = getPt((int)x, hazard_all[x]);
                g.FillRectangle(&brushAll, p.X - barW, p.Y, barW, plotY + plotH - p.Y);
            }
        }
    }
    if (hazard_up.size() > 1) {
        Gdiplus::SolidBrush brushUp(Gdiplus::Color(180, 240, 80, 80));
        for (size_t x = 1; x < hazard_up.size(); x++) {
            if (hazard_up[x] > 0) {
                Gdiplus::PointF p = getPt((int)x, hazard_up[x]);
                g.FillRectangle(&brushUp, p.X, p.Y, barW, plotY + plotH - p.Y);
            }
        }
    }

    Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 65, 140, 240)), redBrush(Gdiplus::Color(255, 240, 80, 80));
    float legendX = (float)rect.X + (float)rect.Width - DPIScaleF(160.0f);
    g.DrawString(L"■ 综合六星条件概率", -1, &tickFont, Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(15.0f)), &blueBrush);
    g.DrawString(L"■ 限定 UP 条件概率", -1, &tickFont, Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(35.0f)), &redBrush);
}

// -------------------------------------------------------
// 预渲染图表到离屏位图 (仅在数据变化时调用一次)
// WM_PAINT 只需 BitBlt 这个缓存，拖动/露出时零延迟
// -------------------------------------------------------
void RebuildChartCache(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    
    HDC hdcWnd = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcWnd);
    
    if (g_hChartBmp) DeleteObject(g_hChartBmp);
    g_hChartBmp = CreateCompatibleBitmap(hdcWnd, w, h);
    
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hChartBmp);
    FillRect(hdcMem, &rc, (HBRUSH)(COLOR_WINDOW + 1));
    
    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        
        DrawKDE(g, Gdiplus::Rect(DPIScale(20), DPIScale(360), DPIScale(600), DPIScale(250)), statsChar.freq_all, statsChar.freq_up, L"角色期望核密度", 130);
        DrawHazard(g, Gdiplus::Rect(DPIScale(640), DPIScale(360), DPIScale(600), DPIScale(250)), statsChar.hazard_all, statsChar.hazard_up, L"角色经验风险函数", 130);
        
        DrawKDE(g, Gdiplus::Rect(DPIScale(20), DPIScale(615), DPIScale(600), DPIScale(250)), statsWep.freq_all, statsWep.freq_up, L"武器期望核密度", 80);
        DrawHazard(g, Gdiplus::Rect(DPIScale(640), DPIScale(615), DPIScale(600), DPIScale(250)), statsWep.hazard_all, statsWep.hazard_up, L"武器经验风险函数", 80);
    }
    
    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWnd);
}

static HFONT hFont = NULL;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE: {
            DragAcceptFiles(hwnd, TRUE);
            // HFONT hFont = CreateFontW(DPIScale(17), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
            hFont = CreateFontW(-DPIScale(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

            HWND hL1 = CreateWindowW(L"STATIC", L"支持\x201C限定角色卡池:当期UP角色\x201D映射。未包含的限定角色卡池将仅排查常驻六星角色名单。", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(15), DPIScale(1000), DPIScale(20), hwnd, NULL, NULL, NULL);
            HWND hL_Char = CreateWindowW(L"STATIC", L"常驻六星角色:", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(45), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
            hCharEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W", L"骏卫,黎风,别礼,余烬,艾尔黛拉", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, DPIScale(120), DPIScale(40), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);
            HWND hL_PoolMap = CreateWindowW(L"STATIC", L"当期UP角色:", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(75), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
            hPoolMapEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W", L"熔火灼痕:莱万汀,轻飘飘的信使:洁尔佩塔,热烈色彩:伊冯,河流的女儿:汤汤,狼珀:洛茜", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, DPIScale(120), DPIScale(70), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);
            HWND hL_Wep = CreateWindowW(L"STATIC", L"常驻六星武器:", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(105), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
            hWepEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W", L"宏愿,不知归,黯色火炬,扶摇,热熔切割器,显赫声名,白夜新星,大雷斑,赫拉芬格,典范,昔日精品,破碎君王,J.E.T.,骁勇,负山,同类相食,楔子,领航者,骑士精神,遗忘,爆破单元,作品：蚀迹,沧溟星梦,光荣记忆,望乡", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, DPIScale(120), DPIScale(100), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);

            hOutEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W", L"等待拖入文件...", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, DPIScale(20), DPIScale(135), DPIScale(1220), DPIScale(215), hwnd, NULL, NULL, NULL);
            
            DWORD tabStops[] = { 200 }; 
            SendMessage(hOutEdit, EM_SETTABSTOPS, 1, (LPARAM)tabStops);
            SendMessage(hOutEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor(COLOR_3DFACE));

            SendMessage(hL1, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hL_Char, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hCharEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hL_PoolMap, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hPoolMapEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hL_Wep, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hWepEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hOutEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            RebuildChartCache(hwnd); // 初始化空图表缓存
            break;
        }
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam; wchar_t filePath[MAX_PATH];
            DragQueryFileW(hDrop, 0, filePath, MAX_PATH); DragFinish(hDrop);
            ProcessFile(filePath); 
            RebuildChartCache(hwnd);     // 数据变化时重建图表缓存
            InvalidateRect(hwnd, NULL, FALSE); // FALSE = 不擦除背景
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            if (g_hChartBmp) {
                // 从预渲染缓存 BitBlt, 零计算零延迟
                HDC hdcMem = CreateCompatibleDC(hdc);
                HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hChartBmp);
                BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
                       ps.rcPaint.right - ps.rcPaint.left,
                       ps.rcPaint.bottom - ps.rcPaint.top,
                       hdcMem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                SelectObject(hdcMem, hOld);
                DeleteDC(hdcMem);
            }
            EndPaint(hwnd, &ps);
            break;
        }
        // case WM_DESTROY: PostQuitMessage(0); break;
        case WM_ERASEBKGND:
            return 1; // 跳过背景擦除, 消灭闪烁
        case WM_DESTROY: {
            if (g_hChartBmp) { DeleteObject(g_hChartBmp); g_hChartBmp = NULL; }
            if (hFont) DeleteObject(hFont);
            PostQuitMessage(0); 
            break;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    LoadLibrary(L"Msftedit.dll");

    SetProcessDPIAware(); 
    HDC hdcScreen = GetDC(NULL); g_dpi = GetDeviceCaps(hdcScreen, LOGPIXELSX); ReleaseDC(NULL, hdcScreen);

    ULONG_PTR gdiplusToken; Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    InitCDFTables(); // 预计算理论 CDF 查找表

    WNDCLASSW wc = {0}; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"EndfieldStatsClass"; RegisterClassW(&wc);

    DWORD dwStyle = (WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;
    // RECT rect = { 0, 0, DPIScale(1280), DPIScale(930) };
    RECT rect = { 0, 0, DPIScale(1280), DPIScale(900) };
    AdjustWindowRectEx(&rect, dwStyle, FALSE, 0);

    int windowWidth = rect.right - rect.left;
    int windowHeight = rect.bottom - rect.top;

    // HWND hwnd = CreateWindowW(wc.lpszClassName, L"终末地抽卡记录分析与可视化", WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, DPIScale(1280), DPIScale(880), NULL, NULL, hInstance, NULL);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"终末地抽卡记录分析与可视化", 
        dwStyle, CW_USEDEFAULT, CW_USEDEFAULT, 
        windowWidth, windowHeight, // <--- 使用计算后的大小
        NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
