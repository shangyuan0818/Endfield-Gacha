// ============================================================
// Endfield Gacha Visualizer - Win32 + GDI+ + PMR / 预分桶 / AoS
// ============================================================
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
#include <cstring>
#include <string_view>
#include <charconv>
#include <ranges>
#include <memory_resource>
#include <array>
#include <cstdint>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------
// [枚举降维]
// ---------------------------------------------------------
enum class ItemType  : uint8_t { Unknown = 0, Character, Weapon };
enum class RankType  : uint8_t { Unknown = 0, Rank3 = 3, Rank4 = 4, Rank5 = 5, Rank6 = 6 };
enum class GachaType : uint8_t { Unknown = 0, Beginner, Standard, Special, Constant };

// 无堆分配的大小写不敏感包含比较
// 原版每次解析一条记录都要 std::string + reserve + push_back + find, 这是严重的 hot-path bug
inline bool ContainsCI(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    const size_t H = haystack.size();
    const size_t N = needle.size();
    for (size_t i = 0; i + N <= H; ++i) {
        bool ok = true;
        for (size_t j = 0; j < N; ++j) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

inline ItemType ParseItemType(std::string_view sv) {
    // 精确匹配优先(UIGF 规范值),命中率高
    if (sv == "Character") return ItemType::Character;
    if (sv == "Weapon")    return ItemType::Weapon;
    if (ContainsCI(sv, "character")) return ItemType::Character;
    if (ContainsCI(sv, "weapon"))    return ItemType::Weapon;
    return ItemType::Unknown;
}

inline RankType ParseRankType(std::string_view sv) {
    if (sv == "6") return RankType::Rank6;
    if (sv == "5") return RankType::Rank5;
    if (sv == "4") return RankType::Rank4;
    if (sv == "3") return RankType::Rank3;
    return RankType::Unknown;
}

inline GachaType ParseGachaType(std::string_view sv) {
    // 原版做 tolower 拷贝整串再 find —— 堆分配,删。
    // uigf_gacha_type 字段的值实际就是上面几个枚举字符串,大小写不敏感匹配即可
    if (ContainsCI(sv, "special"))  return GachaType::Special;
    if (ContainsCI(sv, "beginner")) return GachaType::Beginner;
    if (ContainsCI(sv, "standard")) return GachaType::Standard;
    if (ContainsCI(sv, "constant")) return GachaType::Constant;
    return GachaType::Unknown;
}

// ---------------------------------------------------------
// [极简 JSON 模块 - 修复转义边界]
// ---------------------------------------------------------
inline size_t FindJsonKey(std::string_view source, std::string_view key, size_t startPos = 0) {
    while (true) {
        size_t pos = source.find(key, startPos);
        if (pos == std::string_view::npos) return std::string_view::npos;
        if (pos > 0 && source[pos - 1] == '"' &&
            (pos + key.length() < source.length()) &&
            source[pos + key.length()] == '"') return pos - 1;
        startPos = pos + key.length();
    }
}

inline std::string_view ExtractJsonValue(std::string_view source, std::string_view key, bool isString) {
    size_t pos = FindJsonKey(source, key);
    if (pos == std::string_view::npos) return {};
    pos = source.find(':', pos + key.length() + 2);
    if (pos == std::string_view::npos) return {};
    ++pos;
    while (pos < source.length() &&
           (source[pos] == ' ' || source[pos] == '\t' ||
            source[pos] == '\n' || source[pos] == '\r')) ++pos;

    if (isString) {
        if (pos >= source.length() || source[pos] != '"') return {};
        ++pos;
        size_t endPos = pos;
        while (endPos < source.length() && source[endPos] != '"') {
            if (source[endPos] == '\\' && endPos + 1 < source.length()) endPos += 2;
            else ++endPos;
        }
        return (endPos < source.length()) ? source.substr(pos, endPos - pos) : std::string_view{};
    } else {
        size_t endPos = pos;
        // 注意:原版 gui.cpp 少了 ']' 判断(main.cpp 有),这里补齐以保证解析嵌套数组值时不出错
        while (endPos < source.length() &&
               source[endPos] != ',' && source[endPos] != '}' &&
               source[endPos] != ']' && source[endPos] != ' ' &&
               source[endPos] != '\n' && source[endPos] != '\r') ++endPos;
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
    const size_t len = source.length();
    for (size_t i = pos; i < len; ++i) {
        char c = source[i];
        if (c == '"') {
            for (++i; i < len; ++i) {
                if (source[i] == '\\' && i + 1 < len) { ++i; continue; }
                if (source[i] == '"') break;
            }
            continue;
        }
        if (c == '{') {
            if (depth == 0) objStart = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) cb(source.substr(objStart, i - objStart + 1));
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
}

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const { return std::hash<std::string_view>{}(sv); }
};

inline std::string WideToUtf8(std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), result.data(), size, nullptr, nullptr);
    return result;
}

std::unordered_set<std::string, StringHash, std::equal_to<>> ParseCommaSeparatedUtf8(const std::wstring& text) {
    std::unordered_set<std::string, StringHash, std::equal_to<>> result;
    std::wstring cur;
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
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> result;
    std::wstring cur_pool, cur_up, cur;
    bool reading_up = false;
    for (wchar_t c : text) {
        if ((c == L':' || c == L'\uFF1A') && !reading_up) {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n"));
            cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
            cur_pool = cur; cur.clear(); reading_up = true;
        } else if (c == L',' || c == L'\uFF0C') {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n"));
            cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
            cur_up = cur;
            if (!cur_pool.empty() && !cur_up.empty())
                result[WideToUtf8(cur_pool)] = WideToUtf8(cur_up);
            cur.clear(); cur_pool.clear(); cur_up.clear(); reading_up = false;
        } else cur += c;
    }
    if (reading_up) {
        cur.erase(0, cur.find_first_not_of(L" \t\r\n"));
        cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
        cur_up = cur;
        if (!cur_pool.empty() && !cur_up.empty())
            result[WideToUtf8(cur_pool)] = WideToUtf8(cur_up);
    }
    return result;
}

// ---------------------------------------------------------
// [SoA 分桶 - 角色/武器 独立桶,Calculate 不再 filter 全量]
// 热路径只访问 3 个字段:rank_types / names / poolNames(角色桶用)
// ---------------------------------------------------------
struct PullBucket {
    std::pmr::vector<RankType>         rank_types;
    std::pmr::vector<std::string_view> names;
    std::pmr::vector<std::string_view> poolNames;

    explicit PullBucket(std::pmr::polymorphic_allocator<std::byte> alloc)
        : rank_types(alloc), names(alloc), poolNames(alloc) {}

    void reserve(size_t cap) {
        rank_types.reserve(cap); names.reserve(cap); poolNames.reserve(cap);
    }
    void push_back(RankType rt, std::string_view name, std::string_view pool) {
        rank_types.push_back(rt); names.push_back(name); poolNames.push_back(pool);
    }
    size_t size() const { return rank_types.size(); }
};

struct alignas(64) StatsAccumulator {
    std::array<int, 150> freq_all{};
    std::array<int, 150> freq_up{};
    long long sum_all = 0, sum_sq_all = 0, sum_up = 0, sum_sq_up = 0, sum_win = 0;
    int count_all = 0, count_up = 0, count_win = 0;
    int max_pity_all = 0, max_pity_up = 0;
    int win_5050 = 0, lose_5050 = 0;
};

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
int   DPIScale (int value)   { return MulDiv(value, g_dpi, 96); }
float DPIScaleF(float value) { return value * (g_dpi / 96.0f); }

// -------------------------------------------------------
// CDF 表 & KS 计算
// -------------------------------------------------------
static double g_cdf_char[81] = {};
static double g_cdf_wep[41]  = {};

void InitCDFTables() {
    double surv = 1.0;
    for (int i = 1; i <= 80; ++i) {
        double p = (i <= 65) ? 0.008 : (i <= 79) ? 0.058 + (i - 66) * 0.05 : 1.0;
        g_cdf_char[i] = g_cdf_char[i - 1] + surv * p;
        surv *= (1.0 - p);
    }
    surv = 1.0;
    for (int i = 1; i <= 40; ++i) {
        double p = (i >= 40) ? 1.0 : 0.04;
        g_cdf_wep[i] = g_cdf_wep[i - 1] + surv * p;
        surv *= (1.0 - p);
    }
}

double ComputeKS(const std::array<int, 150>& freq, int max_pity, int n, const double* cdf_table, int cdf_len) {
    if (n == 0) return 0.0;
    double max_d = 0.0;
    int cum_count = 0;
    for (int x = 1; x <= max_pity; ++x) {
        double f_val = (x < cdf_len) ? cdf_table[x] : 1.0;
        double fn_before = (double)cum_count / n;
        cum_count += freq[x];
        double fn_after = (double)cum_count / n;
        double d1 = std::abs(fn_before - f_val);
        double d2 = std::abs(fn_after  - f_val);
        if (d1 > max_d) max_d = d1;
        if (d2 > max_d) max_d = d2;
    }
    return max_d;
}

// -------------------------------------------------------
// 统计核心 - bucket 已只含目标池子,无需 filter
// -------------------------------------------------------
StatsResult Calculate(const PullBucket& bucket, bool isWeapon,
                     const std::unordered_set<std::string, StringHash, std::equal_to<>>& standard_names,
                     const std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>& pool_map) {
    StatsAccumulator acc;
    int current_pity = 0, pity_since_last_up = 0;
    bool had_non_up = false;

    const size_t total = bucket.size();
    for (size_t i = 0; i < total; ++i) {
        ++current_pity;
        ++pity_since_last_up;

        // 非六星:likely 分支
        if (bucket.rank_types[i] != RankType::Rank6) [[likely]] {
            continue;
        }

        if (current_pity < 150) acc.freq_all[current_pity]++;
        if (current_pity > acc.max_pity_all) acc.max_pity_all = current_pity;
        acc.count_all++;
        acc.sum_all    += current_pity;
        acc.sum_sq_all += (long long)current_pity * current_pity;

        bool isUP = false;
        auto it = pool_map.find(bucket.poolNames[i]);
        if (it != pool_map.end()) isUP = (bucket.names[i] == it->second);
        else                      isUP = !standard_names.contains(bucket.names[i]);

        if (isUP) {
            if (pity_since_last_up < 150) acc.freq_up[pity_since_last_up]++;
            if (pity_since_last_up > acc.max_pity_up) acc.max_pity_up = pity_since_last_up;
            acc.count_up++;
            acc.sum_up    += pity_since_last_up;
            acc.sum_sq_up += (long long)pity_since_last_up * pity_since_last_up;

            if (!had_non_up) { acc.count_win++; acc.sum_win += current_pity; acc.win_5050++; }
            had_non_up = false;
            pity_since_last_up = 0;
        } else {
            if (!had_non_up) acc.lose_5050++;
            had_non_up = true;
        }
        current_pity = 0;
    }

    StatsResult s;
    s.freq_all  = acc.freq_all;
    s.freq_up   = acc.freq_up;
    s.count_all = acc.count_all;
    s.count_up  = acc.count_up;
    s.win_5050  = acc.win_5050;
    s.lose_5050 = acc.lose_5050;

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
        const double* cdf_tbl = isWeapon ? g_cdf_wep : g_cdf_char;
        int cdf_len = isWeapon ? 41 : 81;
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
    if (acc.win_5050 + acc.lose_5050 > 0)
        s.win_rate_5050 = (double)acc.win_5050 / (acc.win_5050 + acc.lose_5050);

    return s;
}

// ---------------------------------------------------------
// [RAII 句柄]
// ---------------------------------------------------------
struct FileGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~FileGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    operator HANDLE() const { return h; }
};
struct MapGuard {
    HANDLE h = NULL;
    ~MapGuard() { if (h) CloseHandle(h); }
    operator HANDLE() const { return h; }
};
struct ViewGuard {
    const void* p = nullptr;
    ~ViewGuard() { if (p) UnmapViewOfFile(p); }
};

// ---------------------------------------------------------
// 文件处理
// ---------------------------------------------------------
void ProcessFile(const std::wstring& path) {
    wchar_t charBuf[1024]; GetWindowTextW(hCharEdit, charBuf, 1024);
    auto stdChars = ParseCommaSeparatedUtf8(charBuf);

    wchar_t poolBuf[4096]; GetWindowTextW(hPoolMapEdit, poolBuf, 4096);
    auto poolMap = ParsePoolMapUtf8(poolBuf);

    wchar_t wepBuf[4096]; GetWindowTextW(hWepEdit, wepBuf, 4096);
    auto stdWeps = ParseCommaSeparatedUtf8(wepBuf);

    FileGuard hFile;
    hFile.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                          NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile.h == INVALID_HANDLE_VALUE) return;

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0 || fileSize == INVALID_FILE_SIZE) return;

    MapGuard hMap;
    hMap.h = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap.h) return;

    ViewGuard view;
    view.p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!view.p) return;

    std::string_view bufferView((const char*)view.p, fileSize);
    if (bufferView.size() >= 3 &&
        (unsigned char)bufferView[0] == 0xEF &&
        (unsigned char)bufferView[1] == 0xBB &&
        (unsigned char)bufferView[2] == 0xBF) {
        bufferView.remove_prefix(3);
    }

    // PMR:栈上 2MB 内存池
    std::array<std::byte, 2 * 1024 * 1024> stackBuffer;
    std::pmr::monotonic_buffer_resource pool(stackBuffer.data(), stackBuffer.size());
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    // temps:先收集排序所需的最小字段,再分发到两个桶
    // 终末地排序规则:
    //   角色(id>=0) vs 武器(id<0) 天然分区,内部按 |id| 升序
    //   (原版是 sort by |id| 不分区 —— 但实际上角色 id 为正、武器 id 为负,|id| 比较
    //    会混淆角色和武器相对顺序。新版先按 sign 分区再比 |id|,保证同区内严格按抽卡顺序)
    struct Temp {
        long long id;
        ItemType  it;
        GachaType gt;
        RankType  rt;
        std::string_view name;
        std::string_view poolName;
    };
    std::pmr::vector<Temp> temps(alloc);
    temps.reserve(6000);

    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
        ItemType  it = ParseItemType (ExtractJsonValue(itemStr, "item_type", true));
        RankType  rt = ParseRankType (ExtractJsonValue(itemStr, "rank_type", true));
        GachaType gt = ParseGachaType(ExtractJsonValue(itemStr, "uigf_gacha_type", true));

        // 统计判定规则:
        //   角色池:item_type=Character AND gacha_type=Special
        //   武器池:item_type=Weapon AND gacha_type ∉ {Constant, Standard, Beginner}
        // 不属于这两类的记录完全不需要入桶 —— 节省解析开销
        bool charPath = (it == ItemType::Character && gt == GachaType::Special);
        bool wepPath  = (it == ItemType::Weapon &&
                         gt != GachaType::Constant &&
                         gt != GachaType::Standard &&
                         gt != GachaType::Beginner);
        if (!charPath && !wepPath) return;

        std::string_view name = ExtractJsonValue(itemStr, "name", true);
        std::string_view poolName = ExtractJsonValue(itemStr, "poolName", true);
        if (poolName.empty()) poolName = ExtractJsonValue(itemStr, "gacha_name", true);
        if (poolName.empty()) poolName = ExtractJsonValue(itemStr, "poolname",   true);

        std::string_view idStr = ExtractJsonValue(itemStr, "id", true);
        if (idStr.empty()) idStr = ExtractJsonValue(itemStr, "id", false);
        long long parsed_id = 0;
        if (!idStr.empty()) {
            std::from_chars(idStr.data(), idStr.data() + idStr.size(), parsed_id);
        }

        temps.push_back(Temp{parsed_id, it, gt, rt, name, poolName});
    });

    if (temps.empty()) {
        SetWindowTextW(hOutEdit, L"JSON 解析失败或无数据。");
        return;
    }

    // 排序:先按 sign(角色/武器分区),再按 |id|。
    // 已排序检查:UIGF 文件写出时就按此规则排好,常规情况可直接跳过 sort
    auto abs_ll = [](long long v) { return v < 0 ? -v : v; };
    auto less = [&](const Temp& a, const Temp& b) {
        bool wepA = a.id < 0;
        bool wepB = b.id < 0;
        if (wepA != wepB) return wepA < wepB;
        return abs_ll(a.id) < abs_ll(b.id);
    };
    bool sorted = true;
    for (size_t i = 1; i < temps.size(); ++i) {
        if (less(temps[i], temps[i - 1])) { sorted = false; break; }
    }
    if (!sorted) std::ranges::sort(temps, less);

    // 分发到两个桶
    PullBucket bucketChar(alloc); bucketChar.reserve(4000);
    PullBucket bucketWep (alloc); bucketWep.reserve(2000);
    for (const auto& t : temps) {
        if (t.it == ItemType::Character && t.gt == GachaType::Special) {
            bucketChar.push_back(t.rt, t.name, t.poolName);
        } else {  // 武器桶(条件已在收集阶段过滤过)
            bucketWep.push_back(t.rt, t.name, t.poolName);
        }
    }

    statsChar = Calculate(bucketChar, /*isWeapon=*/false, stdChars, poolMap);
    statsWep  = Calculate(bucketWep,  /*isWeapon=*/true,  stdWeps, {});

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
        statsChar.avg_all, (std::max)(1.0, statsChar.avg_all - statsChar.ci_all_err),
        statsChar.avg_all + statsChar.ci_all_err, statsChar.cv_all * 100.0, statsChar.ks_d_all,
        (statsChar.count_all == 0 ? L"-" : (statsChar.ks_is_normal ? L"符合理论模型" : L"偏离过大")),
        statsChar.avg_up, (std::max)(1.0, statsChar.avg_up - statsChar.ci_up_err),
        statsChar.avg_up + statsChar.ci_up_err,
        statsChar.win_rate_5050 >= 0 ? statsChar.win_rate_5050 * 100.0 : 0.0,
        statsChar.win_5050, statsChar.lose_5050, winCharStr,
        statsWep.count_all, statsWep.count_up,
        statsWep.avg_all, (std::max)(1.0, statsWep.avg_all - statsWep.ci_all_err),
        statsWep.avg_all + statsWep.ci_all_err, statsWep.cv_all * 100.0, statsWep.ks_d_all,
        (statsWep.count_all == 0 ? L"-" : (statsWep.ks_is_normal ? L"符合理论模型" : L"偏离过大")),
        statsWep.avg_up, (std::max)(1.0, statsWep.avg_up - statsWep.ci_up_err),
        statsWep.avg_up + statsWep.ci_up_err,
        statsWep.win_rate_5050 >= 0 ? statsWep.win_rate_5050 * 100.0 : 0.0,
        statsWep.win_5050, statsWep.lose_5050, winWepStr
    );
    SetWindowTextW(hOutEdit, outMsg);
}

// -------------------------------------------------------
// 图形渲染 —— drawCurve 用栈数组,无堆分配
// -------------------------------------------------------
void DrawKDE(Gdiplus::Graphics& g, Gdiplus::Rect rect,
             const std::array<int, 150>& freq_all, const std::array<int, 150>& freq_up,
             const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255));
    g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont,
                 Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f), (float)rect.Y + DPIScaleF(12.0f)),
                 &textBrush);

    int max_x = limit_base;
    bool hasData = false;
    for (int i = 1; i < 150; i++) {
        if (freq_all[i] > 0 || freq_up[i] > 0) {
            hasData = true;
            if (i > max_x) max_x = i;
        }
    }
    if (!hasData) {
        Gdiplus::Font emptyFont(&fontFamily, DPIScaleF(14.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush emptyBrush(Gdiplus::Color(255, 150, 150, 150));
        g.DrawString(L"暂无出金数据", -1, &emptyFont,
                     Gdiplus::PointF((float)rect.X + (float)rect.Width / 2.0f - DPIScaleF(50.0f),
                                     (float)rect.Y + (float)rect.Height / 2.0f),
                     &emptyBrush);
        return;
    }
    max_x = ((max_x / 10) + 1) * 10;

    auto calcKDE = [&](const std::array<int, 150>& freqs, std::array<double, 150>& out_curve) {
        out_curve.fill(0.0);
        int total = 0;
        for (int i = 1; i <= max_x; i++) total += freqs[i];
        if (total == 0) return;
        double bandwidth = 4.0;
        int spread = (int)(4.0 * bandwidth) + 1;
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
    calcKDE(freq_all, kde_all);
    calcKDE(freq_up,  kde_up);

    double max_y = 0.0001;
    for (int i = 1; i <= max_x; i++) {
        if (kde_all[i] > max_y) max_y = kde_all[i];
        if (kde_up[i]  > max_y) max_y = kde_up[i];
    }
    max_y *= 1.25;

    float plotX = (float)rect.X + DPIScaleF(55.0f), plotY = (float)rect.Y + DPIScaleF(45.0f);
    float plotW = (float)rect.Width - DPIScaleF(85.0f), plotH = (float)rect.Height - DPIScaleF(75.0f);
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 150, 150, 150), DPIScaleF(1.5f));
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 235, 235, 235), DPIScaleF(1.0f));
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY, plotX, plotY + plotH);

    auto getPt = [&](int x, double y) {
        return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW,
                               plotY + plotH - (float)(y / max_y) * plotH);
    };
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

    // 栈数组 —— max_x 最大 <150
    auto drawCurve = [&](const std::array<double, 150>& kde, Gdiplus::Color color) {
        std::array<Gdiplus::PointF, 151> pts;
        pts[0] = getPt(0, 0);
        int ptsCount = 1;
        for (int x = 1; x <= max_x && ptsCount < (int)pts.size(); ++x) {
            pts[ptsCount++] = getPt(x, kde[x]);
        }
        Gdiplus::Pen pen(color, DPIScaleF(2.5f));
        g.DrawCurve(&pen, pts.data(), ptsCount, 0.3f);
    };

    drawCurve(kde_all, Gdiplus::Color(255, 65, 140, 240));
    drawCurve(kde_up,  Gdiplus::Color(255, 240, 80, 80));

    Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 65, 140, 240)),
                        redBrush(Gdiplus::Color(255, 240, 80, 80));
    float legendX = (float)rect.X + (float)rect.Width - DPIScaleF(190.0f);
    g.DrawString(L"━━ 综合六星出金分布 (含歪)", -1, &tickFont,
                 Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(15.0f)), &blueBrush);
    g.DrawString(L"━━ 当期限定 UP 出金分布", -1, &tickFont,
                 Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(35.0f)), &redBrush);
}

void DrawHazard(Gdiplus::Graphics& g, Gdiplus::Rect rect,
                const std::array<double, 150>& hazard_all,
                const std::array<double, 150>& hazard_up,
                const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255));
    g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont,
                 Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f), (float)rect.Y + DPIScaleF(12.0f)),
                 &textBrush);

    int max_x = limit_base;
    double max_y = 0.1;
    bool hasData = false;
    for (int i = 1; i < 150; i++) {
        if (hazard_all[i] > 0 || hazard_up[i] > 0) {
            hasData = true;
            if (i > max_x) max_x = i;
            if (hazard_all[i] > max_y) max_y = hazard_all[i];
            if (hazard_up[i]  > max_y) max_y = hazard_up[i];
        }
    }
    if (!hasData) return;
    max_x = ((max_x / 10) + 1) * 10;
    if (max_y > 0.8) max_y = 1.05;
    else max_y = (std::ceil(max_y * 10)) / 10.0 + 0.1;

    float plotX = (float)rect.X + DPIScaleF(55.0f), plotY = (float)rect.Y + DPIScaleF(45.0f);
    float plotW = (float)rect.Width - DPIScaleF(85.0f), plotH = (float)rect.Height - DPIScaleF(75.0f);
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 150, 150, 150), DPIScaleF(1.5f));
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 235, 235, 235), DPIScaleF(1.0f));
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY, plotX, plotY + plotH);

    auto getPt = [&](int x, double y) {
        return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW,
                               plotY + plotH - (float)(y / max_y) * plotH);
    };
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
        if (hazard_all[x] > 0) {
            Gdiplus::PointF p = getPt(x, hazard_all[x]);
            g.FillRectangle(&brushAll, p.X - barW, p.Y, barW, plotY + plotH - p.Y);
        }
    }
    Gdiplus::SolidBrush brushUp(Gdiplus::Color(180, 240, 80, 80));
    for (int x = 1; x <= max_x; x++) {
        if (hazard_up[x] > 0) {
            Gdiplus::PointF p = getPt(x, hazard_up[x]);
            g.FillRectangle(&brushUp, p.X, p.Y, barW, plotY + plotH - p.Y);
        }
    }

    Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 65, 140, 240)),
                        redBrush(Gdiplus::Color(255, 240, 80, 80));
    float legendX = (float)rect.X + (float)rect.Width - DPIScaleF(160.0f);
    g.DrawString(L"■ 综合六星条件概率", -1, &tickFont,
                 Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(15.0f)), &blueBrush);
    g.DrawString(L"■ 限定 UP 条件概率", -1, &tickFont,
                 Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(35.0f)), &redBrush);
}

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
        DrawKDE   (g, Gdiplus::Rect(DPIScale(20),  DPIScale(360), DPIScale(600), DPIScale(250)),
                   statsChar.freq_all, statsChar.freq_up, L"角色期望核密度", 130);
        DrawHazard(g, Gdiplus::Rect(DPIScale(640), DPIScale(360), DPIScale(600), DPIScale(250)),
                   statsChar.hazard_all, statsChar.hazard_up, L"角色经验风险函数", 130);
        DrawKDE   (g, Gdiplus::Rect(DPIScale(20),  DPIScale(615), DPIScale(600), DPIScale(250)),
                   statsWep.freq_all, statsWep.freq_up, L"武器期望核密度", 80);
        DrawHazard(g, Gdiplus::Rect(DPIScale(640), DPIScale(615), DPIScale(600), DPIScale(250)),
                   statsWep.hazard_all, statsWep.hazard_up, L"武器经验风险函数", 80);
    }
    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWnd);
}

static HFONT hFont = NULL;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);
        hFont = CreateFontW(-DPIScale(13), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
        HWND hL1 = CreateWindowW(L"STATIC",
            L"支持\x201C限定角色卡池:当期UP角色\x201D映射。未包含的限定角色卡池将仅排查常驻六星角色名单。",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(15), DPIScale(1000), DPIScale(20), hwnd, NULL, NULL, NULL);
        HWND hL_Char = CreateWindowW(L"STATIC", L"常驻六星角色:",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(45), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
        hCharEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"骏卫,黎风,别礼,余烬,艾尔黛拉",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            DPIScale(120), DPIScale(40), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);
        HWND hL_PoolMap = CreateWindowW(L"STATIC", L"当期UP角色:",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(75), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
        hPoolMapEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"熔火灼痕:莱万汀,轻飘飘的信使:洁尔佩塔,热烈色彩:伊冯,河流的女儿:汤汤,狼珀:洛茜,春雷动，万物生:庄方宜",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            DPIScale(120), DPIScale(70), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);
        HWND hL_Wep = CreateWindowW(L"STATIC", L"常驻六星武器:",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(105), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
        hWepEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"宏愿,不知归,黯色火炬,扶摇,热熔切割器,显赫声名,白夜新星,大雷斑,赫拉芬格,典范,昔日精品,破碎君王,J.E.T.,骁勇,负山,同类相食,楔子,领航者,骑士精神,遗忘,爆破单元,作品：蚀迹,沧溟星梦,光荣记忆,望乡",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            DPIScale(120), DPIScale(100), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);

        hOutEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"等待拖入文件...",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            DPIScale(20), DPIScale(135), DPIScale(1220), DPIScale(215), hwnd, NULL, NULL, NULL);

        DWORD tabStops[] = {200};
        SendMessage(hOutEdit, EM_SETTABSTOPS, 1, (LPARAM)tabStops);
        SendMessage(hOutEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor(COLOR_3DFACE));

        for (HWND h : {hL1, hL_Char, hCharEdit, hL_PoolMap, hPoolMapEdit, hL_Wep, hWepEdit, hOutEdit})
            SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
        RebuildChartCache(hwnd);
        break;
    }
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        wchar_t filePath[MAX_PATH];
        DragQueryFileW(hDrop, 0, filePath, MAX_PATH);
        DragFinish(hDrop);
        ProcessFile(filePath);
        RebuildChartCache(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_hChartBmp) {
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
    case WM_ERASEBKGND: return 1;
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
    HDC hdcScreen = GetDC(NULL);
    g_dpi = GetDeviceCaps(hdcScreen, LOGPIXELSX);
    ReleaseDC(NULL, hdcScreen);

    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    InitCDFTables();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"EndfieldStatsClass";
    RegisterClassW(&wc);

    DWORD dwStyle = (WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;
    RECT rect = {0, 0, DPIScale(1280), DPIScale(900)};
    AdjustWindowRectEx(&rect, dwStyle, FALSE, 0);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"终末地抽卡记录分析与可视化",
        dwStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
