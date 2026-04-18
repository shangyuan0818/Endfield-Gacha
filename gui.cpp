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
#include <memory_resource> // C++17/20 PMR 栈内存池
#include <array>
#include <cstdint>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------
// [C++20 强类型枚举：将字符串降维为单字节，对 L1 缓存极度友好]
// ---------------------------------------------------------
enum class ItemType : uint8_t { Unknown = 0, Character, Weapon };
enum class RankType : uint8_t { Unknown = 0, Rank3 = 3, Rank4 = 4, Rank5 = 5, Rank6 = 6 };
enum class GachaType : uint8_t { Unknown = 0, Beginner, Standard, Special, Constant };

ItemType ParseItemType(std::string_view sv) {
    std::string lower_sv; lower_sv.reserve(sv.size());
    for(char c : sv) lower_sv.push_back((char)std::tolower((unsigned char)c));
    
    if (lower_sv.find("character") != std::string::npos) return ItemType::Character;
    if (lower_sv.find("weapon") != std::string::npos) return ItemType::Weapon;
    return ItemType::Unknown;
}

RankType ParseRankType(std::string_view sv) {
    if (sv == "6") return RankType::Rank6;
    if (sv == "5") return RankType::Rank5;
    if (sv == "4") return RankType::Rank4;
    if (sv == "3") return RankType::Rank3;
    return RankType::Unknown;
}

GachaType ParseGachaType(std::string_view sv) {
    std::string lower_sv; lower_sv.reserve(sv.size());
    for(char c : sv) lower_sv.push_back((char)std::tolower((unsigned char)c));

    if (lower_sv.find("special") != std::string::npos) return GachaType::Special;
    if (lower_sv.find("beginner") != std::string::npos) return GachaType::Beginner;
    if (lower_sv.find("standard") != std::string::npos) return GachaType::Standard;
    if (lower_sv.find("constant") != std::string::npos) return GachaType::Constant;
    return GachaType::Unknown;
}

// ---------------------------------------------------------
// [极简 JSON 模块 & C++20 异构查找支持]
// ---------------------------------------------------------
size_t FindJsonKey(std::string_view source, std::string_view key, size_t startPos = 0) {
    while (true) {
        size_t pos = source.find(key, startPos);
        if (pos == std::string_view::npos) return std::string_view::npos;
        if (pos > 0 && source[pos - 1] == '"' && (pos + key.length() < source.length()) && source[pos + key.length()] == '"') return pos - 1; 
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
        pos++; auto endPos = pos;
        while (endPos < source.length() && source[endPos] != '"') { if (source[endPos] == '\\') endPos++; endPos++; }
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
    
    int depth = 0; size_t objStart = 0;
    for (size_t i = pos; i < source.length(); ++i) {
        char c = source[i];
        if (c == '"') { for (++i; i < source.length(); ++i) { if (source[i] == '\\') { ++i; continue; } if (source[i] == '"') break; } continue; }
        if (c == '{') { if (depth == 0) objStart = i; depth++; } 
        else if (c == '}') { depth--; if (depth == 0) cb(source.substr(objStart, i - objStart + 1)); } 
        else if (c == ']' && depth == 0) break;
    }
}

struct StringHash {
    using is_transparent = void; // 开启 C++20 异构查找 (string_view 查 string)
    size_t operator()(std::string_view sv) const { return std::hash<std::string_view>{}(sv); }
};

std::string WideToUtf8(std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), result.data(), size, nullptr, nullptr);
    return result;
}

// ---------------------------------------------------------
// [数据结构层：面向数据设计 (SoA) 与 栈数组聚合]
// ---------------------------------------------------------
struct PullDataSoA {
    std::pmr::vector<long long> ids;
    std::pmr::vector<ItemType> item_types;
    std::pmr::vector<GachaType> gacha_types;
    std::pmr::vector<RankType> rank_types;
    std::pmr::vector<std::string_view> names;
    std::pmr::vector<std::string_view> poolNames;

    explicit PullDataSoA(std::pmr::polymorphic_allocator<std::byte> alloc)
        : ids(alloc), item_types(alloc), gacha_types(alloc), rank_types(alloc), names(alloc), poolNames(alloc) {}

    void reserve(size_t cap) {
        ids.reserve(cap); item_types.reserve(cap); gacha_types.reserve(cap);
        rank_types.reserve(cap); names.reserve(cap); poolNames.reserve(cap);
    }
    void push_back(long long id, ItemType it, GachaType gt, RankType rt, std::string_view name, std::string_view pool) {
        ids.push_back(id); item_types.push_back(it); gacha_types.push_back(gt);
        rank_types.push_back(rt); names.push_back(name); poolNames.push_back(pool);
    }
};

// 纯栈上的热数据累加器 (大小仅数百字节，彻底干掉 unordered_map)
struct alignas(64) StatsAccumulator {
    std::array<int, 150> freq_all{}; 
    std::array<int, 150> freq_up{};  
    long long sum_all = 0, sum_sq_all = 0, sum_up = 0, sum_sq_up = 0, sum_win = 0;
    int count_all = 0, count_up = 0, count_win = 0;
    int max_pity_all = 0, max_pity_up = 0;
    int win_5050 = 0, lose_5050 = 0;
};

// 冷数据结果 (用于展示)
struct StatsResult {
    std::array<int, 150> freq_all{}; 
    std::array<int, 150> freq_up{};
    int count_all = 0, count_up = 0;
    double avg_all = 0.0, avg_up = 0.0, avg_win = -1.0; 
    double cv_all = 0.0, ci_all_err = 0.0, ci_up_err = 0.0;   
    int win_5050 = 0, lose_5050 = 0;            
    double win_rate_5050 = -1.0;                
    std::array<double, 150> hazard_all{}, hazard_up{};  
    double ks_d_all = 0.0;
    bool ks_is_normal = true; 
};

StatsResult statsChar, statsWep;
HWND hOutEdit, hCharEdit, hWepEdit, hPoolMapEdit;
static HBITMAP g_hChartBmp = NULL;  
int g_dpi = 96;
int DPIScale(int value) { return MulDiv(value, g_dpi, 96); }
float DPIScaleF(float value) { return value * (g_dpi / 96.0f); }

std::unordered_set<std::string, StringHash, std::equal_to<>> ParseCommaSeparatedUtf8(const std::wstring& text) {
    std::unordered_set<std::string, StringHash, std::equal_to<>> result; std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L'\uFF0C') {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n")); 
            if (!cur.empty()) cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
            if (!cur.empty()) result.insert(WideToUtf8(cur));
            cur.clear();
        } else cur += c;
    }
    cur.erase(0, cur.find_first_not_of(L" \t\r\n")); 
    if (!cur.empty()) cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
    if (!cur.empty()) result.insert(WideToUtf8(cur));
    return result;
}

std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> ParsePoolMapUtf8(const std::wstring& text) {
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> result; std::wstring cur_pool, cur_up, cur; bool reading_up = false;
    for (wchar_t c : text) {
        if ((c == L':' || c == L'\uFF1A') && !reading_up) {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1); cur_pool = cur; cur.clear(); reading_up = true;
        } else if (c == L',' || c == L'\uFF0C') {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1); cur_up = cur;
            if (!cur_pool.empty() && !cur_up.empty()) result[WideToUtf8(cur_pool)] = WideToUtf8(cur_up);
            cur.clear(); cur_pool.clear(); cur_up.clear(); reading_up = false;
        } else cur += c;
    }
    if (reading_up) {
        cur.erase(0, cur.find_first_not_of(L" \t\r\n")); cur.erase(cur.find_last_not_of(L" \t\r\n") + 1); cur_up = cur;
        if (!cur_pool.empty() && !cur_up.empty()) result[WideToUtf8(cur_pool)] = WideToUtf8(cur_up);
    }
    return result;
}

// -------------------------------------------------------
// CDF 表 & KS 计算 (针对 Stack Array 优化)
// -------------------------------------------------------
static double g_cdf_char[81] = {};  
static double g_cdf_wep[41]  = {};  

void InitCDFTables() {
    double surv = 1.0;
    for (int i = 1; i <= 80; ++i) {
        double p = (i <= 65) ? 0.008 : (i <= 79) ? 0.058 + (i - 66) * 0.05 : 1.0;
        g_cdf_char[i] = g_cdf_char[i-1] + surv * p; surv *= (1.0 - p);
    }
    surv = 1.0;
    for (int i = 1; i <= 40; ++i) {
        double p = (i >= 40) ? 1.0 : 0.04;
        g_cdf_wep[i] = g_cdf_wep[i-1] + surv * p; surv *= (1.0 - p);
    }
}

double ComputeKS(const std::array<int, 150>& freq, int max_pity, int n, const double* cdf_table, int cdf_len) {
    if (n == 0) return 0.0;
    double max_d = 0.0; int cum_count = 0;
    for (int x = 1; x <= max_pity; ++x) {
        double f_val = (x < cdf_len) ? cdf_table[x] : 1.0;
        double fn_before = (double)cum_count / n;
        cum_count += freq[x];
        double fn_after  = (double)cum_count / n;
        double d1 = std::abs(fn_before - f_val);
        double d2 = std::abs(fn_after  - f_val);
        if (d1 > max_d) max_d = d1;
        if (d2 > max_d) max_d = d2;
    }
    return max_d;
}

// -------------------------------------------------------
// 计算核心 (绝对零分配热路径)
// -------------------------------------------------------
StatsResult Calculate(const PullDataSoA& pulls, bool calcWeapon, 
                     const std::unordered_set<std::string, StringHash, std::equal_to<>>& standard_names, 
                     const std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>& pool_map) {
    StatsAccumulator acc;
    int current_pity = 0, pity_since_last_up = 0;
    bool had_non_up = false;
    
    size_t total = pulls.ids.size();
    for (size_t i = 0; i < total; ++i) {
        bool isSpecial = false;
        if (calcWeapon) {
            isSpecial = (pulls.item_types[i] == ItemType::Weapon && 
                         pulls.gacha_types[i] != GachaType::Constant && 
                         pulls.gacha_types[i] != GachaType::Standard && 
                         pulls.gacha_types[i] != GachaType::Beginner);
        } else {
            isSpecial = (pulls.item_types[i] == ItemType::Character && pulls.gacha_types[i] == GachaType::Special);
        }
        
        if (!isSpecial) continue;
        current_pity++; pity_since_last_up++;
        
        if (pulls.rank_types[i] == RankType::Rank6) {
            if (current_pity < 150) acc.freq_all[current_pity]++;
            if (current_pity > acc.max_pity_all) acc.max_pity_all = current_pity;
            acc.count_all++; acc.sum_all += current_pity; acc.sum_sq_all += (long long)current_pity * current_pity;
            
            bool isUP = false;
            // 异构查找 string_view 直接在 map 中搜索，极快
            auto it = pool_map.find(pulls.poolNames[i]);
            if (it != pool_map.end()) isUP = (pulls.names[i] == it->second);
            else isUP = !standard_names.contains(pulls.names[i]);
            
            if (isUP) {
                if (pity_since_last_up < 150) acc.freq_up[pity_since_last_up]++;
                if (pity_since_last_up > acc.max_pity_up) acc.max_pity_up = pity_since_last_up;
                acc.count_up++; acc.sum_up += pity_since_last_up; acc.sum_sq_up += (long long)pity_since_last_up * pity_since_last_up;
                
                if (!had_non_up) { acc.count_win++; acc.sum_win += current_pity; acc.win_5050++; }
                had_non_up = false; pity_since_last_up = 0;
            } else {
                if (!had_non_up) acc.lose_5050++; 
                had_non_up = true; 
            }
            current_pity = 0;
        }
    }
    
    // 冷数据组装
    StatsResult s;
    s.freq_all = acc.freq_all; s.freq_up = acc.freq_up;
    s.count_all = acc.count_all; s.count_up = acc.count_up;
    s.win_5050 = acc.win_5050; s.lose_5050 = acc.lose_5050;
    
    if (acc.count_all > 0) {
        s.avg_all = (double)acc.sum_all / acc.count_all;
        double var = (double)acc.sum_sq_all / acc.count_all - s.avg_all * s.avg_all;
        double std_all = std::sqrt(var > 0 ? var : 0);
        s.cv_all = (s.avg_all > 0) ? std_all / s.avg_all : 0;
        s.ci_all_err = 1.96 * std_all / std::sqrt((double)acc.count_all);
        
        int survivors = acc.count_all;
        for (int x = 1; x <= acc.max_pity_all; ++x) {
            if (survivors > 0) {
                s.hazard_all[x] = (double)acc.freq_all[x] / survivors;
                survivors -= acc.freq_all[x];
            }
        }
        const double* cdf_tbl = calcWeapon ? g_cdf_wep : g_cdf_char;
        int cdf_len = calcWeapon ? 41 : 81;
        s.ks_d_all = ComputeKS(acc.freq_all, acc.max_pity_all, acc.count_all, cdf_tbl, cdf_len);
        s.ks_is_normal = (s.ks_d_all <= (1.36 / std::sqrt((double)acc.count_all))); 
    }
    
    if (acc.count_up > 0) {
        s.avg_up = (double)acc.sum_up / acc.count_up;
        double var = (double)acc.sum_sq_up / acc.count_up - s.avg_up * s.avg_up;
        double std_up = std::sqrt(var > 0 ? var : 0);
        s.ci_up_err = 1.96 * std_up / std::sqrt((double)acc.count_up);
        
        int survivors = acc.count_up;
        for (int x = 1; x <= acc.max_pity_up; ++x) {
            if (survivors > 0) {
                s.hazard_up[x] = (double)acc.freq_up[x] / survivors;
                survivors -= acc.freq_up[x];
            }
        }
    }
    
    if (acc.count_win > 0) s.avg_win = (double)acc.sum_win / acc.count_win;
    if (acc.win_5050 + acc.lose_5050 > 0) s.win_rate_5050 = (double)acc.win_5050 / (acc.win_5050 + acc.lose_5050);
        
    return s;
}

void ProcessFile(const std::wstring& path) {
    wchar_t charBuf[1024]; GetWindowTextW(hCharEdit, charBuf, 1024);
    auto stdChars = ParseCommaSeparatedUtf8(charBuf);
    
    wchar_t poolBuf[4096]; GetWindowTextW(hPoolMapEdit, poolBuf, 4096);
    auto poolMap = ParsePoolMapUtf8(poolBuf);
    
    wchar_t wepBuf[4096]; GetWindowTextW(hWepEdit, wepBuf, 4096);
    auto stdWeps = ParseCommaSeparatedUtf8(wepBuf);

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0) { CloseHandle(hFile); return; }
    HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) { CloseHandle(hFile); return; }
    const char* mapData = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!mapData) { CloseHandle(hMap); CloseHandle(hFile); return; }

    std::string_view bufferView(mapData, fileSize);
    if (bufferView.size() >= 3 && (unsigned char)bufferView[0] == 0xEF && (unsigned char)bufferView[1] == 0xBB && (unsigned char)bufferView[2] == 0xBF) {
        bufferView.remove_prefix(3);
    }
    
    // PMR：在 Stack 上开辟 1MB 内存池 (配合 /STACK:4194304)
    std::array<std::byte, 1024 * 1024> stackBuffer;
    std::pmr::monotonic_buffer_resource pool(stackBuffer.data(), stackBuffer.size());
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);
    
    PullDataSoA pulls(alloc);
    pulls.reserve(5000); 

    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
        std::string_view name = ExtractJsonValue(itemStr, "name", true);
        ItemType it = ParseItemType(ExtractJsonValue(itemStr, "item_type", true));
        RankType rt = ParseRankType(ExtractJsonValue(itemStr, "rank_type", true));
        
        std::string_view poolName = ExtractJsonValue(itemStr, "poolName", true);
        if (poolName.empty()) poolName = ExtractJsonValue(itemStr, "gacha_name", true);
        if (poolName.empty()) poolName = ExtractJsonValue(itemStr, "poolname", true);
        
        GachaType gt = ParseGachaType(ExtractJsonValue(itemStr, "uigf_gacha_type", true));
        
        std::string_view idStr = ExtractJsonValue(itemStr, "id", true);
        if (idStr.empty()) idStr = ExtractJsonValue(itemStr, "id", false);
        long long parsed_id = 0;
        if (!idStr.empty()) std::from_chars(idStr.data(), idStr.data() + idStr.size(), parsed_id);
        
        pulls.push_back(parsed_id, it, gt, rt, name, poolName);
    });
    
    if (pulls.ids.empty()) { UnmapViewOfFile(mapData); CloseHandle(hMap); CloseHandle(hFile); SetWindowTextW(hOutEdit, L"JSON 解析失败或无数据。"); return; }
    
    // SoA 排序技巧：创建一个索引数组，排完序再重排 SoA (考虑到 ID 是负数避让，保持原逻辑)
    std::pmr::vector<size_t> indices(pulls.ids.size(), &pool);
    std::iota(indices.begin(), indices.end(), 0);
    auto abs_ll = [](long long v) { return v < 0 ? -v : v; };
    std::ranges::sort(indices, [&](size_t a, size_t b){ 
        return abs_ll(pulls.ids[a]) < abs_ll(pulls.ids[b]); 
    });

    PullDataSoA sortedPulls(alloc); sortedPulls.reserve(pulls.ids.size());
    for (size_t idx : indices) sortedPulls.push_back(pulls.ids[idx], pulls.item_types[idx], pulls.gacha_types[idx], pulls.rank_types[idx], pulls.names[idx], pulls.poolNames[idx]);

    // 计算双端数据
    statsChar = Calculate(sortedPulls, false, stdChars, poolMap);
    statsWep = Calculate(sortedPulls, true, stdWeps, {}); 
    
    UnmapViewOfFile(mapData); CloseHandle(hMap); CloseHandle(hFile);

    // 输出逻辑...
    wchar_t winCharStr[64] = L"[无数据]";
    if (statsChar.avg_win >= 0) swprintf(winCharStr, 64, L"%.2f 抽", statsChar.avg_win);
    wchar_t winWepStr[64] = L"[无数据]";
    if (statsWep.avg_win >= 0) swprintf(winWepStr, 64, L"%.2f 抽", statsWep.avg_win);

    wchar_t outMsg[2048];
    swprintf(outMsg, 2048, 
        L"【角色卡池 (特许寻访)】 总计六星: %d | 出当期 UP: %d\r\n"
        L" ▶ 综合六星 (含歪) 出货平均期望:     %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (%d胜%d负)\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\r\n\r\n"
        L"【武器卡池 (武库申领)】 总计六星: %d | 出当期 UP: %d\r\n"
        L" ▶ 综合六星 (含歪) 出货平均期望:     %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (%d胜%d负)\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls",
        statsChar.count_all, statsChar.count_up, 
        statsChar.avg_all, (std::max)(1.0, statsChar.avg_all - statsChar.ci_all_err), statsChar.avg_all + statsChar.ci_all_err, statsChar.cv_all * 100.0, statsChar.ks_d_all, (statsChar.count_all == 0 ? L"-" : (statsChar.ks_is_normal ? L"符合理论模型" : L"偏离过大")),
        statsChar.avg_up, (std::max)(1.0, statsChar.avg_up - statsChar.ci_up_err), statsChar.avg_up + statsChar.ci_up_err, statsChar.win_rate_5050 >= 0 ? statsChar.win_rate_5050 * 100.0 : 0.0, statsChar.win_5050, statsChar.lose_5050, winCharStr,
        statsWep.count_all, statsWep.count_up, 
        statsWep.avg_all, (std::max)(1.0, statsWep.avg_all - statsWep.ci_all_err), statsWep.avg_all + statsWep.ci_all_err, statsWep.cv_all * 100.0, statsWep.ks_d_all, (statsWep.count_all == 0 ? L"-" : (statsWep.ks_is_normal ? L"符合理论模型" : L"偏离过大")),
        statsWep.avg_up, (std::max)(1.0, statsWep.avg_up - statsWep.ci_up_err), statsWep.avg_up + statsWep.ci_up_err, statsWep.win_rate_5050 >= 0 ? statsWep.win_rate_5050 * 100.0 : 0.0, statsWep.win_5050, statsWep.lose_5050, winWepStr
    );
    SetWindowTextW(hOutEdit, outMsg);
}

// -------------------------------------------------------
// 图形渲染层 (已适配 Stack Array)
// -------------------------------------------------------
void DrawKDE(Gdiplus::Graphics& g, Gdiplus::Rect rect, const std::array<int, 150>& freq_all, const std::array<int, 150>& freq_up, const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255)); g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont, Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f), (float)rect.Y + DPIScaleF(12.0f)), &textBrush);
    
    int max_x = limit_base;
    bool hasData = false;
    for (int i = 1; i < 150; i++) {
        if (freq_all[i] > 0 || freq_up[i] > 0) { hasData = true; if (i > max_x) max_x = i; }
    }
    if (!hasData) {
        Gdiplus::Font emptyFont(&fontFamily, DPIScaleF(14.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush emptyBrush(Gdiplus::Color(255, 150, 150, 150));
        g.DrawString(L"暂无出金数据", -1, &emptyFont, Gdiplus::PointF((float)rect.X + (float)rect.Width/2.0f - DPIScaleF(50.0f), (float)rect.Y + (float)rect.Height/2.0f), &emptyBrush);
        return;
    }
    max_x = ((max_x / 10) + 1) * 10;
    
    auto calcKDE = [&](const std::array<int, 150>& freqs, std::array<double, 150>& out_curve) {
        out_curve.fill(0.0); int total = 0; 
        for (int i=1; i<=max_x; i++) total += freqs[i];
        if (total == 0) return;
        double bandwidth = 4.0; int spread = (int)(4.0 * bandwidth) + 1; 
        for (int v = 1; v <= max_x; ++v) {
            if (freqs[v] == 0) continue;
            int lo = (std::max)(1, v - spread), hi = (std::min)(max_x, v + spread);
            for (int x = lo; x <= hi; ++x) {
                double u = (x - v) / bandwidth;
                out_curve[x] += freqs[v] * std::exp(-0.5 * u * u);
            }
        }
        double inv_total = 1.0 / total;
        for (int x = 1; x <= max_x; ++x) out_curve[x] *= inv_total;
    };

    std::array<double, 150> kde_all{}, kde_up{};
    calcKDE(freq_all, kde_all); calcKDE(freq_up, kde_up);
    
    double max_y = 0.0001;
    for (int i=1; i<=max_x; i++) {
        if (kde_all[i] > max_y) max_y = kde_all[i];
        if (kde_up[i] > max_y) max_y = kde_up[i];
    }
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

    auto drawCurve = [&](const std::array<double, 150>& kde, Gdiplus::Color color) {
        std::vector<Gdiplus::PointF> pts; pts.reserve(max_x + 1); pts.push_back(getPt(0, 0));
        for (int x = 1; x <= max_x; x++) pts.push_back(getPt(x, kde[x]));
        Gdiplus::Pen pen(color, DPIScaleF(2.5f)); g.DrawCurve(&pen, pts.data(), (int)pts.size(), 0.3f);
    };

    drawCurve(kde_all, Gdiplus::Color(255, 65, 140, 240));
    drawCurve(kde_up, Gdiplus::Color(255, 240, 80, 80));
    
    Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 65, 140, 240)), redBrush(Gdiplus::Color(255, 240, 80, 80));
    float legendX = (float)rect.X + (float)rect.Width - DPIScaleF(190.0f);
    g.DrawString(L"━━ 综合六星出金分布 (含歪)", -1, &tickFont, Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(15.0f)), &blueBrush);
    g.DrawString(L"━━ 当期限定 UP 出金分布", -1, &tickFont, Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(35.0f)), &redBrush);
}

void DrawHazard(Gdiplus::Graphics& g, Gdiplus::Rect rect, const std::array<double, 150>& hazard_all, const std::array<double, 150>& hazard_up, const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255)); g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont, Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f), (float)rect.Y + DPIScaleF(12.0f)), &textBrush);
    
    int max_x = limit_base;
    double max_y = 0.1;
    bool hasData = false;
    for (int i=1; i<150; i++) {
        if (hazard_all[i] > 0 || hazard_up[i] > 0) {
            hasData = true;
            if (i > max_x) max_x = i;
            if (hazard_all[i] > max_y) max_y = hazard_all[i];
            if (hazard_up[i] > max_y) max_y = hazard_up[i];
        }
    }
    if (!hasData) return;
    max_x = ((max_x / 10) + 1) * 10;
    if (max_y > 0.8) max_y = 1.05; else max_y = (std::ceil(max_y * 10)) / 10.0 + 0.1;

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
    Gdiplus::SolidBrush brushAll(Gdiplus::Color(180, 65, 140, 240)); 
    for (int x = 1; x <= max_x; x++) {
        if (hazard_all[x] > 0) { Gdiplus::PointF p = getPt(x, hazard_all[x]); g.FillRectangle(&brushAll, p.X - barW, p.Y, barW, plotY + plotH - p.Y); }
    }
    Gdiplus::SolidBrush brushUp(Gdiplus::Color(180, 240, 80, 80));
    for (int x = 1; x <= max_x; x++) {
        if (hazard_up[x] > 0) { Gdiplus::PointF p = getPt(x, hazard_up[x]); g.FillRectangle(&brushUp, p.X, p.Y, barW, plotY + plotH - p.Y); }
    }

    Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 65, 140, 240)), redBrush(Gdiplus::Color(255, 240, 80, 80));
    float legendX = (float)rect.X + (float)rect.Width - DPIScaleF(160.0f);
    g.DrawString(L"■ 综合六星条件概率", -1, &tickFont, Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(15.0f)), &blueBrush);
    g.DrawString(L"■ 限定 UP 条件概率", -1, &tickFont, Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(35.0f)), &redBrush);
}

void RebuildChartCache(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom; if (w <= 0 || h <= 0) return;
    
    HDC hdcWnd = GetDC(hwnd); HDC hdcMem = CreateCompatibleDC(hdcWnd);
    if (g_hChartBmp) DeleteObject(g_hChartBmp);
    g_hChartBmp = CreateCompatibleBitmap(hdcWnd, w, h);
    
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hChartBmp);
    FillRect(hdcMem, &rc, (HBRUSH)(COLOR_WINDOW + 1));
    
    {
        Gdiplus::Graphics g(hdcMem); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        DrawKDE(g, Gdiplus::Rect(DPIScale(20), DPIScale(360), DPIScale(600), DPIScale(250)), statsChar.freq_all, statsChar.freq_up, L"角色期望核密度", 130);
        DrawHazard(g, Gdiplus::Rect(DPIScale(640), DPIScale(360), DPIScale(600), DPIScale(250)), statsChar.hazard_all, statsChar.hazard_up, L"角色经验风险函数", 130);
        DrawKDE(g, Gdiplus::Rect(DPIScale(20), DPIScale(615), DPIScale(600), DPIScale(250)), statsWep.freq_all, statsWep.freq_up, L"武器期望核密度", 80);
        DrawHazard(g, Gdiplus::Rect(DPIScale(640), DPIScale(615), DPIScale(600), DPIScale(250)), statsWep.hazard_all, statsWep.hazard_up, L"武器经验风险函数", 80);
    }
    SelectObject(hdcMem, hOld); DeleteDC(hdcMem); ReleaseDC(hwnd, hdcWnd);
}

static HFONT hFont = NULL;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE: {
            DragAcceptFiles(hwnd, TRUE);
            hFont = CreateFontW(-DPIScale(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
            HWND hL1 = CreateWindowW(L"STATIC", L"支持\x201C限定角色卡池:当期UP角色\x201D映射。未包含的限定角色卡池将仅排查常驻六星角色名单。", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(15), DPIScale(1000), DPIScale(20), hwnd, NULL, NULL, NULL);
            HWND hL_Char = CreateWindowW(L"STATIC", L"常驻六星角色:", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(45), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
            hCharEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W", L"骏卫,黎风,别礼,余烬,艾尔黛拉", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, DPIScale(120), DPIScale(40), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);
            HWND hL_PoolMap = CreateWindowW(L"STATIC", L"当期UP角色:", WS_CHILD | WS_VISIBLE, DPIScale(20), DPIScale(75), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
            hPoolMapEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W", L"熔火灼痕:莱万汀,轻飘飘的信使:洁尔佩塔,热烈色彩:伊冯,河流的女儿:汤汤,狼珀:洛茜,春雷动，万物生:庄方宜", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, DPIScale(120), DPIScale(70), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);
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
            RebuildChartCache(hwnd); 
            break;
        }
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam; wchar_t filePath[MAX_PATH];
            DragQueryFileW(hDrop, 0, filePath, MAX_PATH); DragFinish(hDrop);
            ProcessFile(filePath); RebuildChartCache(hwnd); InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            if (g_hChartBmp) {
                HDC hdcMem = CreateCompatibleDC(hdc); HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hChartBmp);
                BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, hdcMem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                SelectObject(hdcMem, hOld); DeleteDC(hdcMem);
            }
            EndPaint(hwnd, &ps);
            break;
        }
        case WM_ERASEBKGND: return 1; 
        case WM_DESTROY: {
            if (g_hChartBmp) { DeleteObject(g_hChartBmp); g_hChartBmp = NULL; }
            if (hFont) DeleteObject(hFont);
            PostQuitMessage(0); break;
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
    InitCDFTables(); 

    WNDCLASSW wc = {0}; wc.lpfnWndProc = WndProc; wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"EndfieldStatsClass"; RegisterClassW(&wc);

    DWORD dwStyle = (WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;
    RECT rect = { 0, 0, DPIScale(1280), DPIScale(900) };
    AdjustWindowRectEx(&rect, dwStyle, FALSE, 0);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"终末地抽卡记录分析与可视化", 
        dwStyle, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
