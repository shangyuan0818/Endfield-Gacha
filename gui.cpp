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
    // gacha_type 字段的值实际就是上面几个枚举字符串,大小写不敏感匹配即可
    // (UIGF v4.2: 字段名为 gacha_type;v3.0 时叫 uigf_gacha_type)
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

// 注意:UP 映射文本中故意只识别 ASCII ',' 和 ':' 作为分隔符。
// 全角逗号 '，'(U+FF0C) 与全角冒号 '：'(U+FF1A) 不视为分隔符 —— 因为合法的
// 池名本身可能含有全角逗号(如 "春雷动，万物生")。把全角逗号当分隔符会导致
// 该池的 UP 映射被切碎,UP 识别全部失效(用户输入法切换的便利不值这个代价)。
std::unordered_set<std::string, StringHash, std::equal_to<>> ParseCommaSeparatedUtf8(const std::wstring& text) {
    std::unordered_set<std::string, StringHash, std::equal_to<>> result;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',') {
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
        if (c == L':' && !reading_up) {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n"));
            cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
            cur_pool = cur; cur.clear(); reading_up = true;
        } else if (c == L',') {
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

// 上面两个函数依赖 WideToUtf8 → 仅在主线程安全使用(WideCharToMultiByte 本身
// 是 thread-safe,但 GetWindowText 必须在主线程,所以是分两步:主线程提取 wstring
// 后转 utf8,然后这两个 FromUtf8 版本在 worker 上跑)。下面是 utf8 直进版本:
inline std::string TrimUtf8(std::string_view sv) {
    size_t b = sv.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    size_t e = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(b, e - b + 1));
}

std::unordered_set<std::string, StringHash, std::equal_to<>> ParseCommaSeparatedUtf8FromUtf8(std::string_view text) {
    // 与 wchar_t 版同步: 仅识别 ASCII ',' 作为分隔符,不识别全角逗号。
    std::unordered_set<std::string, StringHash, std::equal_to<>> result;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ',') {
            std::string trimmed = TrimUtf8(cur);
            if (!trimmed.empty()) result.insert(std::move(trimmed));
            cur.clear();
        } else {
            cur += text[i];
        }
    }
    std::string trimmed = TrimUtf8(cur);
    if (!trimmed.empty()) result.insert(std::move(trimmed));
    return result;
}

std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> ParsePoolMapUtf8FromUtf8(std::string_view text) {
    // 与 wchar_t 版同步: 仅识别 ASCII ',' 和 ':',不识别全角分隔符。
    // (合法池名可能含全角逗号如 "春雷动，万物生",识别全角会切碎该池映射)
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> result;
    std::string cur, cur_pool;
    bool reading_up = false;
    auto flush_entry = [&]() {
        if (!cur_pool.empty()) {
            std::string up = TrimUtf8(cur);
            if (!up.empty()) result[cur_pool] = std::move(up);
        }
        cur.clear(); cur_pool.clear(); reading_up = false;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ':' && !reading_up) {
            cur_pool = TrimUtf8(cur); cur.clear(); reading_up = true;
        } else if (text[i] == ',') {
            flush_entry();
        } else {
            cur += text[i];
        }
    }
    if (reading_up) flush_entry();
    return result;
}

// ---------------------------------------------------------
// [SoA 分桶 - 角色/武器 独立桶,Calculate 不再 filter 全量]
// 热路径只访问 4 个字段:rank_types / names / poolNames / is_free
// is_free: 标记"第30抽赠送十连"的成员,该机制不占用也不增加保底进度
// (依据《明日方舟终末地抽卡机制解析》2.1.1)
// ---------------------------------------------------------
struct PullBucket {
    std::pmr::vector<RankType>         rank_types;
    std::pmr::vector<std::string_view> names;
    std::pmr::vector<std::string_view> poolNames;
    std::pmr::vector<uint8_t>          is_free;

    explicit PullBucket(std::pmr::polymorphic_allocator<std::byte> alloc)
        : rank_types(alloc), names(alloc), poolNames(alloc), is_free(alloc) {}

    void reserve(size_t cap) {
        rank_types.reserve(cap); names.reserve(cap);
        poolNames.reserve(cap);  is_free.reserve(cap);
    }
    void push_back(RankType rt, std::string_view name, std::string_view pool, uint8_t free_flag) {
        rank_types.push_back(rt); names.push_back(name);
        poolNames.push_back(pool); is_free.push_back(free_flag);
    }
    size_t size() const { return rank_types.size(); }
};

// alignas(128) 而非 64: Apple Silicon 与 Intel Sandy Bridge+ 上 spatial prefetcher
// 会预取相邻 cacheline (128B), 用 128 对齐避免 false sharing 更稳妥
struct alignas(128) StatsAccumulator {
    std::array<int, 150> freq_all{};
    std::array<int, 150> freq_up{};
    long long sum_all = 0, sum_sq_all = 0, sum_up = 0, sum_sq_up = 0, sum_win = 0;
    int count_all = 0, count_up = 0, count_win = 0;
    int max_pity_all = 0, max_pity_up = 0;
    int win_5050 = 0, lose_5050 = 0;
    // 右删失:循环结束时仍在累积、尚未结算的当前保底计数
    // 生存分析里这些样本应参与分母(risk set),但不参与分子(event)
    int censored_pity_all = 0;
    int censored_pity_up  = 0;
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
    // 右删失(用于显示"当前已垫 N 抽")
    int censored_pity_all = 0;
    int censored_pity_up  = 0;
};

StatsResult statsChar, statsWep;
HWND hOutEdit, hCharEdit, hWepEdit, hPoolMapEdit;
static HBITMAP g_hChartBmp = NULL;
int g_dpi = 96;
int   DPIScale (int value)   { return MulDiv(value, g_dpi, 96); }
float DPIScaleF(float value) { return value * (g_dpi / 96.0f); }

// -------------------------------------------------------
// CDF 表 & KS 检验
// -------------------------------------------------------
// 角色池(ggpipi《明日方舟终末地抽卡机制解析》):
//   综合六星分布(hazard_all 的理论参照):
//     k=30:     h = 1 - (1-0.008)^11 ≈ 0.08462 (特殊十连 11 次独立判定)
//     1≤k≤65, k≠30:  h = 0.008
//     66≤k≤79:       h = 0.058 + (k-66) × 0.05
//     k=80:          h = 1 (硬保底)
//
// 武器池(Reddit "An Analysis of First Featured Weapon Acquisition", u/Useful_Plenty_2443):
//   以"十连"为最小抽取粒度。单抽角度:
//     基础 6 星率 = 4%(不分 UP/非 UP)
//     6 星中 UP 的条件率 = 25%  → 每抽拿到 UP 武器率 = 1%
//   十连角度:
//     s = 1 - 0.99^10 ≈ 9.5618%  该十连含至少 1 个 UP
//     u = 0.99^10 - 0.96^10 ≈ 23.9549%  该十连含 6 星但无 UP
//     v = 0.96^10 ≈ 66.4833%    该十连无任何 6 星
//   保底:
//     40 抽 6 星保底:连续 3 次十连(30 抽)无 6 星,第 4 次十连必含至少 1 个 6 星
//     80 抽 UP  保底:连续 7 次十连(70 抽)无 UP,第 8 次十连必含至少 1 个 UP
//   理论验证:
//     E[首 6 星] ≈ 19.17 抽(由下方解析 CDF 推出)
//     E[首 UP]  ≈ 81.66 抽(Reddit 四态递推)
//
// KS 理论 CDF 构造 — "距离上次 6 星的抽数 x"的分布:
//   角色池 g_cdf_char[x=0..80]:hazard 段函数积分
//   武器池 g_cdf_wep[x=0..40]:
//     x=1..30:   每抽独立 4%,P(x=k) = 0.96^(k-1) × 0.04
//     x=31..40:  保底十连条件分布,归一化常数 = 1 - 0.96^10
static double g_cdf_char[82] = {};  // x=0..80,角色池
static double g_cdf_wep[41]  = {};  // x=0..40,武器池
static bool   g_cdf_init     = false;

void InitCDFTables() {
    // 幂等保护: 多次调用只填充一次
    // 注意: 为了避免另一线程读到"半初始化"的表, init 标记必须在末尾才置 true
    if (g_cdf_init) return;
    // ---- 角色池 ----
    // 综合六星分布(含 k=30 特殊十连 11 次判定)
    double surv = 1.0;
    for (int i = 1; i <= 80; ++i) {
        double p;
        if (i == 30)       p = 1.0 - std::pow(1.0 - 0.008, 11);  // ≈ 0.08462
        else if (i <= 65)  p = 0.008;
        else if (i <= 79)  p = 0.058 + (i - 66) * 0.05;
        else               p = 1.0;
        if (p > 1.0) p = 1.0;
        g_cdf_char[i] = g_cdf_char[i - 1] + surv * p;
        surv *= (1.0 - p);
    }
    g_cdf_char[81] = 1.0;

    // ---- 武器池 ----
    // 我们要建的是"距离上次 6 星的抽数 x"的分布 CDF。
    // 物理模型:
    //   1) 前 3 个十连(x=1..30)内每抽 4% 独立,
    //      P(x=k | 1≤k≤30) = 0.96^(k-1) × 0.04
    //   2) 若前 30 抽都未出 6 星(概率 0.96^30),
    //      第 4 个十连保底:保证至少 1 个 6 星,但抽内分布按"条件伯努利"展开:
    //      在"十连含至少 1 个 6 星"的条件下,第 j 抽(j=1..10,对应 x=31..40)
    //      命中 6 星的概率 = 解析推导(下方计算)
    //
    // 正确的解析计算方法:
    //   在第 4 个十连,设 Y 是首次命中在本十连内的位置(1..10),Y 还可能为 ∞(本十连不命中)
    //   无保底模型:P(Y=j) = 0.96^(j-1) × 0.04,P(Y=∞) = 0.96^10
    //   保底规则:强制排除 Y=∞ 的情形 → 条件分布 P(Y=j | Y≤10) = 0.96^(j-1) × 0.04 / (1 - 0.96^10)
    //
    // 合起来:
    //   对 k=1..30:  P_pdf[k] = 0.96^(k-1) × 0.04
    //   对 k=31..40: P_pdf[k] = 0.96^30 × [0.96^(k-31) × 0.04 / (1 - 0.96^10)]
    //
    // 验证:∫PDF = (1 - 0.96^30) + 0.96^30 × 1 = 1 ✓
    {
        double base_hit = 0.04;
        double base_miss = 0.96;
        // 前 30 抽
        double surv_w = 1.0;
        for (int k = 1; k <= 30; ++k) {
            double pk = surv_w * base_hit;
            g_cdf_wep[k] = g_cdf_wep[k - 1] + pk;
            surv_w *= base_miss;
        }
        // 第 31~40 抽(保底十连,条件概率)
        // 到 k=30 时 surv_w = 0.96^30 ≈ 0.2939
        // 保底十连条件分布的归一化常数 = 1 - 0.96^10
        double norm = 1.0 - std::pow(base_miss, 10);  // ≈ 0.3351556
        double local_surv = 1.0;  // 在保底十连内部的存活概率
        for (int k = 31; k <= 40; ++k) {
            // 保底十连内第 (k-30) 抽命中概率 = 0.96^(k-31) × 0.04 / norm
            double local_hit = local_surv * base_hit / norm;
            double pk = surv_w * local_hit;
            g_cdf_wep[k] = g_cdf_wep[k - 1] + pk;
            local_surv *= base_miss;
        }
        // g_cdf_wep[40] 应该 ≈ 1.0
    }

    g_cdf_init = true;  // 末尾置位,确保所有读者看到完整表
}

// 修复:离散阶梯 CDF 的 K-S 统计量需严格对齐两条阶梯
// 在 x 处,两条阶梯的"底":F_n(cum before x),F_theory(x-1)
// 在 x 处,两条阶梯的"顶":F_n(cum after x),F_theory(x)
// 原版用 fn_before 减 cdf_table[x] —— 拿经验阶梯底对理论阶梯顶,
// 人为引入 h_x 的单点跳跃(软保底段可高达 5%+),造成巨大伪偏差
double ComputeKS(const std::array<int, 150>& freq, int max_pity, int n,
                 const double* cdf_table, int cdf_len) {
    if (n == 0) return 0.0;
    // 防御性 clamp: freq 数组容量 150,max_pity 必须 < 150 否则越界读
    if (max_pity > 149) max_pity = 149;
    double max_d = 0.0;
    int cum_count = 0;
    for (int x = 1; x <= max_pity; ++x) {
        double fn_before    = (double)cum_count / n;
        double cdf_before_x = (x - 1 < cdf_len) ? cdf_table[x - 1] : 1.0;

        cum_count += freq[x];

        double fn_after    = (double)cum_count / n;
        double cdf_after_x = (x < cdf_len) ? cdf_table[x] : 1.0;

        double d1 = std::abs(fn_before - cdf_before_x);
        double d2 = std::abs(fn_after  - cdf_after_x);
        if (d1 > max_d) max_d = d1;
        if (d2 > max_d) max_d = d2;
    }
    return max_d;
}

// -------------------------------------------------------
// 统计工具:t 分布 95% 双侧临界值 (α/2 = 0.025)
// -------------------------------------------------------
// 当样本量较小时(N < 30),标准正态 z=1.96 的 CI 会严重低估真实不确定性
// (因为 t 分布尾部更厚)。严格的样本 CI 应该用 t_{α/2, N-1}
//
// 实现策略:
//   df = 1, 2, 3, 4:查表(Hill 近似在低 df 误差较大,最高 0.75%)
//   df ≥ 5:用 Hill(1970) 四阶渐近展开(误差 < 0.02%)
//   df → ∞ 时收敛到 z = 1.959964
inline double TCritical95(int df) {
    // α=0.025 双侧 95% CI
    if (df <= 0) return 1.959963984540054;  // 保护
    // 低自由度查表(值来自 scipy.stats.t.ppf(0.975, df))
    static constexpr double kTable[] = {
        0.0,        // df=0 占位
        12.706205,  // df=1
        4.302653,   // df=2
        3.182446,   // df=3
        2.776445,   // df=4
    };
    if (df <= 4) return kTable[df];

    // Hill 1970 四阶展开
    constexpr double z = 1.959963984540054;
    constexpr double z2 = z * z;
    constexpr double z3 = z2 * z;
    constexpr double z5 = z3 * z2;
    constexpr double z7 = z5 * z2;
    constexpr double z9 = z7 * z2;
    constexpr double g1 = (z3 + z) / 4.0;
    constexpr double g2 = (5.0*z5 + 16.0*z3 + 3.0*z) / 96.0;
    constexpr double g3 = (3.0*z7 + 19.0*z5 + 17.0*z3 - 15.0*z) / 384.0;
    constexpr double g4 = (79.0*z9 + 776.0*z7 + 1482.0*z5 - 1920.0*z3 - 945.0*z) / 92160.0;
    double d = (double)df;
    double inv_d = 1.0 / d;
    return z + g1 * inv_d
             + g2 * inv_d * inv_d
             + g3 * inv_d * inv_d * inv_d
             + g4 * inv_d * inv_d * inv_d * inv_d;
}

// -------------------------------------------------------
// 无偏样本方差(贝塞尔校正):s² = [Σx² - (Σx)²/N] / (N-1)
// 注意 N=1 时样本方差未定义(除零),返回 0
// -------------------------------------------------------
inline double SampleVariance(long long sum, long long sum_sq, int n) {
    if (n <= 1) return 0.0;
    // 数值稳定式:避免先算 mean 再做 E[X²]-E[X]² 的灾难性消去
    double numerator = (double)sum_sq - (double)sum * sum / (double)n;
    if (numerator < 0.0) numerator = 0.0;  // 浮点误差保护
    return numerator / (double)(n - 1);
}

// -------------------------------------------------------
// 统计核心 - bucket 已只含目标池子,无需 filter
//
// 注意:武器池与角色池的"UP 判定"语义不同:
//   - 角色池:出 6 星后有 50/50,歪了则下一个六星保底 UP("大保底")
//   - 武器池:每个六星独立判定 UP(条件概率 25%),无"大保底"。
//             连续 7 次十连无 UP 触发"80 抽 UP 保底十连"强制 UP。
// 因此 win_5050/lose_5050/avg_win 这组变量:
//   - 角色池:对应"小保底不歪率",统计意义明确
//   - 武器池:复用变量但含义改为"6 星中 UP 条件率"(win_5050=UP六星数,lose_5050=非UP六星数)
//             avg_win 对武器池无定义,保持 -1
// -------------------------------------------------------
StatsResult Calculate(const PullBucket& bucket, bool isWeapon,
                     const std::unordered_set<std::string, StringHash, std::equal_to<>>& standard_names,
                     const std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>& pool_map) {
    StatsAccumulator acc;
    int current_pity = 0, pity_since_last_up = 0;
    bool had_non_up = false;  // 仅用于角色池的小保底追踪

    // 第30抽赠送十连处理 (依据《明日方舟终末地抽卡机制解析》2.1.1):
    //   - "该十连享有基础概率(0.008),但不占用也不增加保底进度"
    //   - 数据中用 isFree=true 标记 (10 条独立记录)
    //   - 不推进 current_pity / pity_since_last_up (本体保底通道独立)
    //   - 若赠送内出 6 星,归入 freq_all[30] (与理论 CDF 第30抽节点的合并 hazard
    //     `1-(1-0.008)^11` 对齐),sum_all 按 30 计入
    //   - 赠送出货不重置玩家本体 cur_pity (独立通道)
    //   - 仍计入 count_all / count_up / win_5050 / lose_5050 (这是真实出货)
    const size_t total = bucket.size();
    for (size_t i = 0; i < total; ++i) {
        const bool isFree = bucket.is_free[i];

        // 赠送十连不推进保底通道
        if (!isFree) {
            ++current_pity;
            ++pity_since_last_up;
        }

        // 非六星:likely 分支
        if (bucket.rank_types[i] != RankType::Rank6) [[likely]] {
            continue;
        }

        // 出 6 星. 决定计入 freq 的位置:
        //   - 赠送十连出货 -> 归入 freq[30]
        //   - 正常出货 -> 归入 freq[current_pity]
        const int slot_all = isFree ? 30 : current_pity;
        if (slot_all < 150) acc.freq_all[slot_all]++;
        if (slot_all > acc.max_pity_all) acc.max_pity_all = slot_all;
        acc.count_all++;
        acc.sum_all    += slot_all;
        acc.sum_sq_all += (long long)slot_all * slot_all;

        bool isUP = false;
        auto it = pool_map.find(bucket.poolNames[i]);
        if (it != pool_map.end()) isUP = (bucket.names[i] == it->second);
        else                      isUP = !standard_names.contains(bucket.names[i]);

        if (isUP) {
            const int slot_up = isFree ? 30 : pity_since_last_up;
            if (slot_up < 150) acc.freq_up[slot_up]++;
            if (slot_up > acc.max_pity_up) acc.max_pity_up = slot_up;
            acc.count_up++;
            acc.sum_up    += slot_up;
            acc.sum_sq_up += (long long)slot_up * slot_up;

            // win_5050 分子:
            //   - 角色池:只在 50/50 阶段(!had_non_up)的 UP 才计入,大保底必中 UP 不算
            //   - 武器池:每个 UP 都独立计入(无大保底概念,每个 6 星都是独立 25% 判定)
            // count_win/sum_win (avg_win "不歪出货期望") 仅对角色池有物理含义
            if (isWeapon) {
                acc.win_5050++;
            } else if (!had_non_up) {
                acc.win_5050++;
                acc.count_win++;
                acc.sum_win += slot_all;
            }
            had_non_up = false;
            // 赠送十连出 UP 不重置 pity_since_last_up (独立通道); 正常出 UP 重置
            if (!isFree) pity_since_last_up = 0;
        } else {
            // lose_5050 分母:
            //   - 角色池:上一次 6 星是 UP(had_non_up=false)→ 现在这个是"歪了",首次计入
            //             上一次 6 星是非 UP(had_non_up=true)→ 已在大保底阶段,跨期继承
            //             此时继续出非 UP 是异常数据(大保底规则本不允许),不重复计数
            //   - 武器池:每个非 UP 6 星都独立计入
            if (isWeapon) {
                acc.lose_5050++;
            } else if (!had_non_up) {
                acc.lose_5050++;
            }
            had_non_up = true;
        }
        // 赠送十连出货不重置 current_pity (独立通道); 正常出货重置
        if (!isFree) current_pity = 0;
    }

    // 右删失:遍历结束时若仍有未结算的 pity,记录为删失样本
    // 这些抽数"存活"到了 current_pity 抽仍未出 6 星(或 UP)
    acc.censored_pity_all = current_pity;
    acc.censored_pity_up  = pity_since_last_up;

    // 防御性 clamp:即使数据异常导致 max_pity / censored_pity > 149,
    // 后续 ComputeKS 与 hazard 循环的索引访问也必须安全
    if (acc.max_pity_all > 149) acc.max_pity_all = 149;
    if (acc.max_pity_up  > 149) acc.max_pity_up  = 149;
    if (acc.censored_pity_all > 149) acc.censored_pity_all = 149;
    if (acc.censored_pity_up  > 149) acc.censored_pity_up  = 149;

    StatsResult s;
    s.freq_all  = acc.freq_all;
    s.freq_up   = acc.freq_up;
    s.count_all = acc.count_all;
    s.count_up  = acc.count_up;
    s.win_5050  = acc.win_5050;
    s.lose_5050 = acc.lose_5050;
    s.censored_pity_all = acc.censored_pity_all;
    s.censored_pity_up  = acc.censored_pity_up;

    if (acc.count_all > 0) {
        s.avg_all = (double)acc.sum_all / acc.count_all;
        // 贝塞尔校正的无偏样本方差 s² = Σ(x-μ)² / (N-1)
        // N=1 时 s² 未定义,SampleVariance 返回 0(CI 也自然为 0)
        double var = SampleVariance(acc.sum_all, acc.sum_sq_all, acc.count_all);
        double std_all = std::sqrt(var);
        s.cv_all = (s.avg_all > 0) ? std_all / s.avg_all : 0;
        // CI 使用 t 分布临界值(自由度 N-1),小样本下比 z=1.96 更保守正确
        double t_crit = TCritical95(acc.count_all - 1);
        s.ci_all_err = t_crit * std_all / std::sqrt((double)acc.count_all);

        // K-S 检验:角色池用 g_cdf_char(ggpipi 模型),武器池用 g_cdf_wep(Reddit 模型)
        const double* cdf_tbl = isWeapon ? g_cdf_wep : g_cdf_char;
        int cdf_len = isWeapon ? 41 : 82;
        s.ks_d_all = ComputeKS(acc.freq_all, acc.max_pity_all, acc.count_all,
                               cdf_tbl, cdf_len);
        s.ks_is_normal = (s.ks_d_all <= (1.36 / std::sqrt((double)acc.count_all)));
    }

    // Kaplan-Meier 式经验风险函数 - 综合六星:
    //   risk set 初值 = 全部观测样本(已毕业 + 删失)
    //   到 x 抽时 hazard[x] = freq[x] / survivors
    //   survivors 每步先减去事件(freq[x]),再减去在 x 发生的删失
    // 即使 count_all=0 也要处理:用户可能从未出 6 星但已垫 N 抽(极少见但有效)
    if (acc.count_all > 0 || acc.censored_pity_all > 0) {
        int survivors = acc.count_all + (acc.censored_pity_all > 0 ? 1 : 0);
        int max_reach_all = (std::max)(acc.max_pity_all, acc.censored_pity_all);
        if (max_reach_all > 149) max_reach_all = 149;  // 防御性 clamp(已被上游保证,这里再防一道)
        for (int x = 1; x <= max_reach_all; ++x) {
            if (survivors > 0) {
                s.hazard_all[x] = (double)acc.freq_all[x] / survivors;
                survivors -= acc.freq_all[x];
                if (x == acc.censored_pity_all) survivors -= 1;
            }
        }
    }

    if (acc.count_up > 0) {
        s.avg_up = (double)acc.sum_up / acc.count_up;
        double var = SampleVariance(acc.sum_up, acc.sum_sq_up, acc.count_up);
        double std_up = std::sqrt(var);
        double t_crit = TCritical95(acc.count_up - 1);
        s.ci_up_err = t_crit * std_up / std::sqrt((double)acc.count_up);
    }

    // UP hazard 同理
    if (acc.count_up > 0 || acc.censored_pity_up > 0) {
        int survivors = acc.count_up + (acc.censored_pity_up > 0 ? 1 : 0);
        int max_reach_up = (std::max)(acc.max_pity_up, acc.censored_pity_up);
        if (max_reach_up > 149) max_reach_up = 149;  // 防御性 clamp
        for (int x = 1; x <= max_reach_up; ++x) {
            if (survivors > 0) {
                s.hazard_up[x] = (double)acc.freq_up[x] / survivors;
                survivors -= acc.freq_up[x];
                if (x == acc.censored_pity_up) survivors -= 1;
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
// 安全读取动态长度的 Edit 控件文本
// 原版固定 wchar_t[4096] 在用户粘贴超长 UP 映射文本时会被 GetWindowTextW 截断,
// 下游 ParsePoolMapUtf8 看到的是不完整数据 → 丢映射。
// 先 GetWindowTextLengthW 查长度再按需分配,彻底消除截断风险
inline std::wstring GetDynamicWindowText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    std::wstring buf((size_t)len, L'\0');
    GetWindowTextW(hwnd, buf.data(), len + 1);  // GetWindowTextW 需要 len+1 容纳末尾 '\0'
    return buf;
}

// ---------------------------------------------------------
// [文件处理 - 工作线程化]
//
// 原版 WM_DROPFILES 同步调 ProcessFile + RebuildChartCache,期间窗口消息
// 循环阻塞,用户无法移动窗口/输入/最小化。重构后:
//   1) 主线程做 I/O 准备(读 GUI 文本框 + 把文件内容拷到 std::string)
//   2) Worker 线程做纯 CPU 计算(JSON 解析 + Calculate),结果写入 heap 上
//      的 ProcessOutput 对象
//   3) Worker 用 PostMessage(WM_APP_PROCESS_DONE) 把结果指针回投到主线程
//   4) 主线程在该消息处理中更新 statsChar/statsWep + UI,然后 delete output
//
// 注意:
//   - GDI / SetWindowTextW 都不是 thread-safe,只能在主线程调
//   - statsChar/statsWep 是全局,WM_PAINT 通过 g_hChartBmp 间接读它们,
//     但 g_hChartBmp 由 RebuildChartCache 重建,所以只要 RebuildChartCache
//     和 statsChar 写入都在主线程串行,就不需要锁
//   - g_processing 标志防止 worker 跑时重复触发(双开 worker)
// ---------------------------------------------------------

#define WM_APP_PROCESS_DONE  (WM_APP + 1)

// 前向声明:RebuildChartCache 定义在 DrawECDF/DrawMRL 之后,但 ProcessFile_Consume
// 需要在文件中段调用它。
void RebuildChartCache(HWND hwnd);

// 跨线程载荷:主线程构造,worker 填充结果,主线程消费后 delete
struct ProcessOutput {
    HWND        hwnd_main = NULL;  // 主窗口,worker 用 PostMessage 回投到这里

    // === 主线程预填(由 ProcessFile_Submit 设置) ===
    // 文件 buffer 用 mmap 直读,零拷贝(与原版 ProcessFile + macOS Analyzer 对齐)。
    // 三个 handle 必须存活到 Consume 阶段才能 unmap/close,因为 ExportRecord
    // 内的 string_view 都指向 mmap 区域。
    HANDLE      hFile = INVALID_HANDLE_VALUE;
    HANDLE      hMap  = NULL;
    const void* viewPtr = nullptr;
    DWORD       fileSize = 0;

    std::string utf8_chars;     // 来自 hCharEdit (这个无法 mmap,GUI 控件文本必须主线程 GetWindowText)
    std::string utf8_poolMap;
    std::string utf8_weps;

    // === worker 填充 ===
    bool        ok = false;
    StatsResult statsChar;
    StatsResult statsWep;
    std::wstring outMsg;
    std::wstring errMsg;

    ~ProcessOutput() {
        // 主线程消费后调 delete 时统一清理 mmap 资源
        if (viewPtr) UnmapViewOfFile(viewPtr);
        if (hMap)    CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    }
};

// 用全局原子防双开;Win32 上 LONG volatile + InterlockedExchange 等价于 atomic_flag
static volatile LONG g_processing = 0;

// Worker 线程入口:纯 CPU 工作,不碰任何 GUI
DWORD WINAPI ProcessFile_Worker(LPVOID arg) {
    auto* out = (ProcessOutput*)arg;

    // 解析输入(WideToUtf8 已经在主线程做完,这里直接用 utf8 视图)
    auto stdChars = ParseCommaSeparatedUtf8FromUtf8(out->utf8_chars);
    auto poolMap  = ParsePoolMapUtf8FromUtf8       (out->utf8_poolMap);
    auto stdWeps  = ParseCommaSeparatedUtf8FromUtf8(out->utf8_weps);

    std::string_view bufferView((const char*)out->viewPtr, out->fileSize);
    if (bufferView.size() >= 3 &&
        (unsigned char)bufferView[0] == 0xEF &&
        (unsigned char)bufferView[1] == 0xBB &&
        (unsigned char)bufferView[2] == 0xBF) {
        bufferView.remove_prefix(3);
    }

    // PMR:栈上 2MB 内存池(对应主线程版本的设计)。
    // worker 线程在 ProcessFile_Submit 中以 4MB 栈创建,容得下这 2MB 缓冲区。
    // 栈池 vs 堆池的性能差异:
    //   - 分配/释放开销 0 (栈指针偏移 vs malloc 一次 2MB)
    //   - 与 worker 栈的局部变量物理相邻,L1/L2 热,TLB 不会 miss
    //   - 整个 PMR 工作集(temps + bucketChar + bucketWep) 都从此池分配,锁在热区
    std::array<std::byte, 2 * 1024 * 1024> stackBuffer;
    std::pmr::monotonic_buffer_resource pool(stackBuffer.data(), stackBuffer.size());
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    struct Temp {
        long long id;
        ItemType  it;
        GachaType gt;
        RankType  rt;
        std::string_view name;
        std::string_view poolName;
        uint8_t   isFree;
    };
    std::pmr::vector<Temp> temps(alloc);
    temps.reserve(6000);

    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
        // UIGF v4.2 字段读取:
        //   - gacha_type   (替代 v3.0 的 uigf_gacha_type)
        //   - item_name    (替代 v3.0 的 name)
        //   - pool_name    (自定义,snake_case;原 poolName)
        //   - is_free      (自定义,snake_case;原 isFree)
        //
        // ForEachJsonObject 找的是 "list" 这个 key。v4.2 文件里 "list" 只
        // 在 endfield[0] 内层出现一次(顶层 info 块没有 list),所以不需要
        // 先穿透 endfield 数组,直接找到的就是正确的记录数组。
        ItemType  it = ParseItemType (ExtractJsonValue(itemStr, "item_type",  true));
        RankType  rt = ParseRankType (ExtractJsonValue(itemStr, "rank_type",  true));
        GachaType gt = ParseGachaType(ExtractJsonValue(itemStr, "gacha_type", true));

        bool charPath = (it == ItemType::Character && gt == GachaType::Special);
        bool wepPath  = (it == ItemType::Weapon &&
                         gt != GachaType::Constant &&
                         gt != GachaType::Standard &&
                         gt != GachaType::Beginner);
        if (!charPath && !wepPath) return;

        std::string_view name = ExtractJsonValue(itemStr, "item_name", true);
        std::string_view poolName = ExtractJsonValue(itemStr, "pool_name", true);

        std::string_view idStr = ExtractJsonValue(itemStr, "id", true);
        if (idStr.empty()) idStr = ExtractJsonValue(itemStr, "id", false);
        long long parsed_id = 0;
        if (!idStr.empty()) {
            std::from_chars(idStr.data(), idStr.data() + idStr.size(), parsed_id);
        }

        // is_free: bool 字面量,不带引号
        std::string_view isFreeStr = ExtractJsonValue(itemStr, "is_free", false);
        uint8_t isFree = (isFreeStr == "true") ? 1 : 0;

        temps.push_back(Temp{parsed_id, it, gt, rt, name, poolName, isFree});
    });

    if (temps.empty()) {
        out->ok = false;
        out->errMsg = L"JSON 解析失败或无数据。";
        PostMessageW(out->hwnd_main, WM_APP_PROCESS_DONE, 0, (LPARAM)out);
        return 0;
    }

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

    PullBucket bucketChar(alloc); bucketChar.reserve(4000);
    PullBucket bucketWep (alloc); bucketWep.reserve(2000);
    for (const auto& t : temps) {
        if (t.it == ItemType::Character && t.gt == GachaType::Special) {
            bucketChar.push_back(t.rt, t.name, t.poolName, t.isFree);
        } else {
            bucketWep.push_back(t.rt, t.name, t.poolName, t.isFree);
        }
    }

    out->statsChar = Calculate(bucketChar, /*isWeapon=*/false, stdChars, poolMap);
    out->statsWep  = Calculate(bucketWep,  /*isWeapon=*/true,  stdWeps, {});

    // 在 worker 渲染输出文本(swprintf 是 thread-safe;只有 SetWindowTextW 不是)
    wchar_t winCharStr[64] = L"[无数据]";
    if (out->statsChar.avg_win >= 0)
        swprintf(winCharStr, 64, L"%.2f 抽", out->statsChar.avg_win);

    wchar_t pendCharStr[96] = L"";
    if (out->statsChar.censored_pity_all > 0 || out->statsChar.censored_pity_up > 0) {
        swprintf(pendCharStr, 96, L"  [当前垫刀: 距上次六星 %d 抽 / 距上次 UP %d 抽]",
                 out->statsChar.censored_pity_all, out->statsChar.censored_pity_up);
    }
    wchar_t pendWepStr[96] = L"";
    if (out->statsWep.censored_pity_all > 0 || out->statsWep.censored_pity_up > 0) {
        swprintf(pendWepStr, 96, L"  [当前垫刀: 距上次六星 %d 抽 / 距上次 UP %d 抽]",
                 out->statsWep.censored_pity_all, out->statsWep.censored_pity_up);
    }

    auto ksLabel = [](const StatsResult& r) -> const wchar_t* {
        if (r.count_all == 0) return L"-";
        return r.ks_is_normal ? L"符合理论模型" : L"偏离过大";
    };
    const wchar_t* ksCharLabel = ksLabel(out->statsChar);
    const wchar_t* ksWepLabel  = ksLabel(out->statsWep);

    wchar_t outMsg[2560];
    swprintf(outMsg, 2560,
        L"【角色卡池 (特许寻访)】 总计六星: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合六星 (含歪) 出货平均期望:     %5.2f 抽 (理论 ≈ 51.81)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 74.33)   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (理论 50%%) (%d胜%d负)\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\r\n\r\n"
        L"【武器卡池 (武库申领)】 总计六星: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合六星出货平均期望:             %5.2f 抽 (理论 ≈ 19.17)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 81.66)   [95%% CI: %5.1f ~ %5.1f]    |   6 星中 UP 率: %5.1f%% (理论 25%%) (%d UP / %d 非UP)",
        out->statsChar.count_all, out->statsChar.count_up, pendCharStr,
        out->statsChar.avg_all, (std::max)(1.0, out->statsChar.avg_all - out->statsChar.ci_all_err),
        out->statsChar.avg_all + out->statsChar.ci_all_err, out->statsChar.cv_all * 100.0,
        out->statsChar.ks_d_all, ksCharLabel,
        out->statsChar.avg_up, (std::max)(1.0, out->statsChar.avg_up - out->statsChar.ci_up_err),
        out->statsChar.avg_up + out->statsChar.ci_up_err,
        out->statsChar.win_rate_5050 >= 0 ? out->statsChar.win_rate_5050 * 100.0 : 0.0,
        out->statsChar.win_5050, out->statsChar.lose_5050, winCharStr,
        out->statsWep.count_all, out->statsWep.count_up, pendWepStr,
        out->statsWep.avg_all, (std::max)(1.0, out->statsWep.avg_all - out->statsWep.ci_all_err),
        out->statsWep.avg_all + out->statsWep.ci_all_err, out->statsWep.cv_all * 100.0,
        out->statsWep.ks_d_all, ksWepLabel,
        out->statsWep.avg_up, (std::max)(1.0, out->statsWep.avg_up - out->statsWep.ci_up_err),
        out->statsWep.avg_up + out->statsWep.ci_up_err,
        out->statsWep.win_rate_5050 >= 0 ? out->statsWep.win_rate_5050 * 100.0 : 0.0,
        out->statsWep.win_5050, out->statsWep.lose_5050
    );
    out->outMsg = outMsg;
    out->ok = true;

    PostMessageW(out->hwnd_main, WM_APP_PROCESS_DONE, 0, (LPARAM)out);
    return 0;
}

// 主线程入口:做 I/O 准备 + 启动 worker。
// 返回 false 表示提交失败(应立即清理),true 表示 worker 已启动(WM_APP_PROCESS_DONE
// 会在完成时投递)。
bool ProcessFile_Submit(HWND hwnd, const std::wstring& path) {
    // 双开保护:用 InterlockedCompareExchange 原子地把 0->1
    if (InterlockedCompareExchange(&g_processing, 1, 0) != 0) {
        return false;  // 已有 worker 在跑,忽略本次拖入
    }

    auto out = std::make_unique<ProcessOutput>();
    out->hwnd_main = hwnd;

    // 主线程读 GUI 控件文本(子控件的 GetWindowTextW 不允许从 worker 调)
    out->utf8_chars   = WideToUtf8(GetDynamicWindowText(hCharEdit));
    out->utf8_poolMap = WideToUtf8(GetDynamicWindowText(hPoolMapEdit));
    out->utf8_weps    = WideToUtf8(GetDynamicWindowText(hWepEdit));

    // 主线程做文件 mmap,所有权直接交给 ProcessOutput(零拷贝)。
    // mmap view 在 worker 持有期间一直有效,Consume 阶段 ProcessOutput 析构统一 unmap。
    // 失败路径下也由 unique_ptr<ProcessOutput> 析构正确清理(已分配的资源)。
    out->hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out->hFile == INVALID_HANDLE_VALUE) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    out->fileSize = GetFileSize(out->hFile, NULL);
    if (out->fileSize == 0 || out->fileSize == INVALID_FILE_SIZE) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    out->hMap = CreateFileMappingW(out->hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!out->hMap) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    out->viewPtr = MapViewOfFile(out->hMap, FILE_MAP_READ, 0, 0, 0);
    if (!out->viewPtr) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }

    // 启动 worker. 显式指定 4MB 栈(与主线程 /STACK:4194304 对齐;
    // CreateThread 默认栈只 1MB,容纳不下 worker 内部的 2MB PMR 栈池)。
    // 注意:dwStackSize 是预留+commit,Windows 实际只 commit 必要页,常驻内存仅约 1 页。
    HANDLE hThread = CreateThread(NULL, 4 * 1024 * 1024,
                                  ProcessFile_Worker, out.get(), 0, NULL);
    if (!hThread) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    CloseHandle(hThread);  // 我们用 PostMessage 同步,不需要 join
    out.release();         // worker 接管所有权,完成时主线程在 WM_APP_PROCESS_DONE 里 delete
    return true;
}

// 主线程消费 worker 结果. 必须在 WM_APP_PROCESS_DONE 里调用
void ProcessFile_Consume(HWND hwnd, ProcessOutput* out) {
    if (out->ok) {
        // 把结果搬到全局 statsChar/statsWep (主线程独占,不需要锁)
        statsChar = out->statsChar;
        statsWep  = out->statsWep;
        SetWindowTextW(hOutEdit, out->outMsg.c_str());
        RebuildChartCache(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
    } else {
        SetWindowTextW(hOutEdit,
            out->errMsg.empty() ? L"处理失败,请检查文件格式" : out->errMsg.c_str());
    }
    delete out;
    InterlockedExchange(&g_processing, 0);  // 释放双开锁
}

// -------------------------------------------------------
// 图形渲染 —— drawCurve 用栈数组,无堆分配
// -------------------------------------------------------
// ---------------------------------------------------------
// [ECDF (经验累积分布函数) 图]
//
// 设计:
//   - 离散阶梯线: ECDF(x) = (Σ_{k<=x} freq[k]) / total
//   - 同时画综合(蓝)和 UP(红)两条经验 ECDF + 两条理论 CDF(虚线)
//   - 标记 KS 偏离最大处的 D 值竖线(与 ks_d_all 一致,用户可视化检验)
//   - 右删失处理: ECDF 终点不到 1.0(因为 censored_pity 表示当前未出货)
//
// 为什么从 KDE 切换到 ECDF:
//   抽卡数据是离散整数 pity,样本量极小(n ~10),KDE 的高斯核平滑会引入虚假
//   连续性,带宽选择对结果影响巨大,在 x=1 等边界处会产生人造凸起。
//   ECDF 是离散数据的标准非参数显示,无任何参数选择,与 KS 检验直接对应。
// ---------------------------------------------------------
void DrawECDF(Gdiplus::Graphics& g, Gdiplus::Rect rect,
              const std::array<int, 150>& freq_all, const std::array<int, 150>& freq_up,
              int count_all, int count_up,
              [[maybe_unused]] int censored_all, [[maybe_unused]] int censored_up,
              const double* theory_cdf_all, int theory_cdf_all_len,
              const double* theory_cdf_up,  int theory_cdf_up_len,
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
    bool hasData = (count_all > 0) || (count_up > 0);
    for (int i = 1; i < 150; i++) {
        if (freq_all[i] > 0 || freq_up[i] > 0) {
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

    // 网格 + 坐标轴
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 230, 230, 230), DPIScaleF(1.0f));
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 80, 80, 80),  DPIScaleF(1.0f));
    float plotX = (float)rect.X + DPIScaleF(50.0f);
    float plotY = (float)rect.Y + DPIScaleF(40.0f);
    float plotW = (float)rect.Width  - DPIScaleF(70.0f);
    float plotH = (float)rect.Height - DPIScaleF(60.0f);
    if (plotW <= 0 || plotH <= 0) return;

    g.DrawLine(&axisPen, plotX, plotY,         plotX, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);

    auto getPt = [&](int x, double y) -> Gdiplus::PointF {
        if (y < 0) y = 0; if (y > 1) y = 1;
        return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW,
                               plotY + plotH - (float)y * plotH);
    };

    Gdiplus::Font tickFont(&fontFamily, DPIScaleF(11.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));

    // Y 轴 0/25/50/75/100% 网格
    for (int i = 0; i <= 4; ++i) {
        double y_val = (double)i / 4.0;
        float py = plotY + plotH - (float)y_val * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[16]; swprintf(y_label, 16, L"%d%%", i * 25);
        float labelW = (float)wcslen(y_label) * DPIScaleF(5.5f) + DPIScaleF(8.0f);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - labelW, py - DPIScaleF(6.0f)), &tickBrush);
    }
    // X 轴刻度
    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        float xoff = (x < 10 ? 4.0f : x < 100 ? 8.0f : 12.0f) * DPIScaleF(1.0f);
        g.DrawString(x_label, -1, &tickFont,
                     Gdiplus::PointF(px - xoff, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    // 画理论 CDF (虚线): 实线步进 (k-1, F(k-1)) -> (k, F(k-1)) -> (k, F(k))
    // 用 GraphicsPath 攒整条阶梯路径再一次性 stroke,
    // dash pattern 沿连续路径走,避免逐段 DrawLine 时 dash 在每段重启
    // (那样会让短段虚线视觉上变成密集小段甚至看起来像"淡色波浪线")。
    auto drawTheoryCDF = [&](const double* cdf, int cdf_len, Gdiplus::Color color) {
        if (!cdf || cdf_len < 2) return;
        Gdiplus::Pen pen(color, DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        pen.SetDashPattern(dash, 2);
        int upper = (cdf_len - 1 < max_x) ? cdf_len - 1 : max_x;
        Gdiplus::GraphicsPath path;
        // 起点 (0, F(0))
        auto p0 = getPt(0, cdf[0]);
        Gdiplus::PointF prev = p0;
        for (int k = 1; k <= upper; ++k) {
            // 水平段终点 (k, F(k-1)) -> 垂直段终点 (k, F(k))
            auto pH = getPt(k, cdf[k-1]);
            auto pV = getPt(k, cdf[k]);
            path.AddLine(prev, pH);
            path.AddLine(pH, pV);
            prev = pV;
        }
        g.DrawPath(&pen, &path);
    };

    // 画经验 ECDF (实阶梯线).
    // 注: 删失观测(用户当前还在垫的 cur_pity)不画在 ECDF 上 ——
    // 因为它还没事件化, 强行画一个标记反而误导(会落在 ECDF 终点 y=100% 处)。
    // MRL 图已经精确显示"已垫 X 抽 / 预期还需 Y 抽", 这里不重复。
    auto drawEmpiricalECDF = [&](const std::array<int, 150>& freq, int total,
                                  Gdiplus::Color color) {
        if (total == 0) return;
        Gdiplus::Pen pen(color, DPIScaleF(2.5f));
        double cum = 0.0;
        auto prev_pt = getPt(0, 0);
        for (int k = 1; k <= max_x; ++k) {
            if (freq[k] == 0) continue;
            auto h_end = getPt(k, cum);
            g.DrawLine(&pen, prev_pt.X, prev_pt.Y, h_end.X, h_end.Y);
            cum += (double)freq[k] / (double)total;
            auto v_end = getPt(k, cum);
            g.DrawLine(&pen, h_end.X, h_end.Y, v_end.X, v_end.Y);
            prev_pt = v_end;
        }
        auto end_pt = getPt(max_x, cum);
        g.DrawLine(&pen, prev_pt.X, prev_pt.Y, end_pt.X, end_pt.Y);
    };

    drawTheoryCDF(theory_cdf_all, theory_cdf_all_len, Gdiplus::Color(180, 65, 140, 240));
    drawTheoryCDF(theory_cdf_up,  theory_cdf_up_len,  Gdiplus::Color(180, 240, 80, 80));
    drawEmpiricalECDF(freq_all, count_all, Gdiplus::Color(255, 65, 140, 240));
    drawEmpiricalECDF(freq_up,  count_up,  Gdiplus::Color(255, 240, 80, 80));

    // KS 标记: 在偏离最大处画短竖线(仅综合 ECDF)
    if (count_all > 0 && theory_cdf_all && theory_cdf_all_len >= 2) {
        double max_d = 0; int max_d_x = 0;
        double cum = 0;
        for (int k = 1; k <= max_x && k < theory_cdf_all_len; ++k) {
            cum += (double)freq_all[k] / (double)count_all;
            double d = std::fabs(cum - theory_cdf_all[k]);
            if (d > max_d) { max_d = d; max_d_x = k; }
        }
        if (max_d > 0.01 && max_d_x > 0) {
            double emp_y  = 0;
            for (int k = 1; k <= max_d_x; ++k) emp_y += (double)freq_all[k] / (double)count_all;
            double th_y = theory_cdf_all[max_d_x];
            auto p_emp = getPt(max_d_x, emp_y);
            auto p_th  = getPt(max_d_x, th_y);
            Gdiplus::Pen ksPen(Gdiplus::Color(255, 120, 120, 120), DPIScaleF(1.5f));
            Gdiplus::REAL dash[2] = { DPIScaleF(2.0f), DPIScaleF(2.0f) };
            ksPen.SetDashPattern(dash, 2);
            g.DrawLine(&ksPen, p_emp.X, p_emp.Y, p_th.X, p_th.Y);
            wchar_t lbl[32];
            swprintf(lbl, 32, L"KS D=%.3f", max_d);
            float midY = (p_emp.Y + p_th.Y) * 0.5f;
            g.DrawString(lbl, -1, &tickFont,
                         Gdiplus::PointF(p_emp.X + DPIScaleF(4.0f), midY - DPIScaleF(7.0f)),
                         &tickBrush);
        }
    }

    // 图例 (3 项水平排列: 综合实线 / UP 实线 / 理论 CDF 虚线)
    // 与 macOS / iOS 端布局对齐 —— 标题旁同一行,从右向左排,
    // 这样图例完全位于标题区(rect.Y+12 行),不会下沉到绘图区(rect.Y+40 起)。
    // 旧版图例垂直堆叠 3 行,最下面一项会落进绘图区与曲线重叠。
    Gdiplus::Font legendFont(&fontFamily, DPIScaleF(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush blueBr(Gdiplus::Color(255, 65, 140, 240));
    Gdiplus::SolidBrush redBr (Gdiplus::Color(255, 240, 80, 80));

    // 三个图例项的文字(色块在前,文字在后)
    const wchar_t* legAll  = L"综合六星 ECDF";
    const wchar_t* legUp   = L"当期限定 UP ECDF";
    const wchar_t* legThy  = L"理论 CDF (综合)";

    // 测量文字宽度,精确从右排 —— 不能用固定常量,因为不同字体/DPI 下宽度不同
    auto measureW = [&](const wchar_t* s) -> float {
        Gdiplus::RectF box;
        g.MeasureString(s, -1, &legendFont, Gdiplus::PointF(0, 0), &box);
        return box.Width;
    };
    const float swatchW    = DPIScaleF(14.0f);  // 实线/虚线色块宽度
    const float swatchGap  = DPIScaleF(6.0f);   // 色块到文字间距
    const float entryGap   = DPIScaleF(16.0f);  // 项与项之间间距
    const float legendY    = (float)rect.Y + DPIScaleF(12.0f);
    const float swatchYOff = DPIScaleF(8.0f);   // 色块在图例行内的垂直居中偏移

    // 从右往左:Theory(虚线) → UP → All
    float wAll  = measureW(legAll);
    float wUp   = measureW(legUp);
    float wThy  = measureW(legThy);
    float xRight = (float)rect.X + (float)rect.Width - DPIScaleF(12.0f);

    // 第 3 项: 理论 CDF (虚线,最右)
    float xThyText = xRight - wThy;
    float xThySw   = xThyText - swatchGap - swatchW;
    {
        Gdiplus::Pen dashPen(Gdiplus::Color(255, 130, 130, 130), DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(2.5f), DPIScaleF(2.0f) };
        dashPen.SetDashPattern(dash, 2);
        g.DrawLine(&dashPen, xThySw, legendY + swatchYOff,
                   xThySw + swatchW, legendY + swatchYOff);
    }
    g.DrawString(legThy, -1, &legendFont,
                 Gdiplus::PointF(xThyText, legendY), &textBrush);

    // 第 2 项: UP ECDF
    float xUpText = xThySw - entryGap - wUp;
    float xUpSw   = xUpText - swatchGap - swatchW;
    g.FillRectangle(&redBr, xUpSw, legendY + swatchYOff - DPIScaleF(1.5f),
                    swatchW, DPIScaleF(3.0f));
    g.DrawString(legUp, -1, &legendFont,
                 Gdiplus::PointF(xUpText, legendY), &textBrush);

    // 第 1 项: 综合 ECDF
    float xAllText = xUpSw - entryGap - wAll;
    float xAllSw   = xAllText - swatchGap - swatchW;
    g.FillRectangle(&blueBr, xAllSw, legendY + swatchYOff - DPIScaleF(1.5f),
                    swatchW, DPIScaleF(3.0f));
    g.DrawString(legAll, -1, &legendFont,
                 Gdiplus::PointF(xAllText, legendY), &textBrush);
}

// ---------------------------------------------------------
// [MRL (Mean Residual Life) 图]
//
// MRL(t) = E[X - t | X > t] —— "已经垫了 t 抽,还要再垫多少抽的期望"
//
// 经验 MRL 计算 (从 freq 直方图):
//   MRL_emp(t) = Σ_{k>t} (k-t)·freq[k] / Σ_{k>t} freq[k]
//   分母 = 0 (即 t >= max_observed) 时 MRL 未定义
//
// 显示策略:
//   - 实线: t 处至少有 2 个观测在分子里 (Σ_{k>t} freq[k] >= 2),数值可靠
//   - 半透明虚线: 仅 1 个观测,高方差区
//   - 不画: 0 观测 (无意义)
//   - 同时画理论 MRL (虚线): 基于理论 CDF 数值积分
//   - 当前 censored_pity 位置画竖线 + "你在这里"标注 (用户决策视角的关键)
// ---------------------------------------------------------
void DrawMRL(Gdiplus::Graphics& g, Gdiplus::Rect rect,
             const std::array<int, 150>& freq_all,
             const std::array<int, 150>& freq_up,
             int count_all, int count_up,
             int censored_all, int censored_up,
             const double* theory_cdf_all, int theory_cdf_all_len,
             const double* theory_cdf_up,  int theory_cdf_up_len,
             const std::wstring& title, int limit_base,
             int theory_all_cap = 0, int theory_up_cap = 0) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255));
    g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont,
                 Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f), (float)rect.Y + DPIScaleF(12.0f)),
                 &textBrush);

    int max_x = limit_base;
    bool hasData = (count_all > 0) || (count_up > 0);
    for (int i = 1; i < 150; i++) {
        if (freq_all[i] > 0 || freq_up[i] > 0) if (i > max_x) max_x = i;
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

    // ---- 计算经验 MRL 序列 (并记录每个 t 处的 surviving 计数) ----
    auto computeEmpiricalMRL = [&](const std::array<int, 150>& freq, int total)
        -> std::pair<std::array<double, 150>, std::array<int, 150>> {
        std::array<double, 150> mrl{}; mrl.fill(-1.0);  // -1 = undefined
        std::array<int, 150> surv{}; surv.fill(0);
        if (total == 0) return {mrl, surv};
        // 后缀和: 从最大 max_x 往回累加
        long long suf_count = 0, suf_weighted = 0;
        for (int t = max_x; t >= 0; --t) {
            // 在循环开始时 suf_count = Σ_{k>t} freq[k], 注意 k 从 t+1 开始
            // 我们要在每一步先用当前累积值算 MRL(t),再把 freq[t] 累加进去给下一轮 t-1 用
            surv[t] = (int)suf_count;
            if (suf_count >= 1) {
                mrl[t] = (double)(suf_weighted - (long long)t * suf_count) / (double)suf_count;
            }
            if (t >= 1) {
                suf_count    += freq[t];
                suf_weighted += (long long)t * freq[t];
            }
        }
        return {mrl, surv};
    };

    auto mrl_all = computeEmpiricalMRL(freq_all, count_all);
    auto mrl_up  = computeEmpiricalMRL(freq_up,  count_up);

    // ---- 计算理论 MRL (基于理论 CDF) ----
    auto computeTheoryMRL = [&](const double* cdf, int cdf_len) {
        std::array<double, 150> tmrl{}; tmrl.fill(-1.0);
        if (!cdf || cdf_len < 2) return tmrl;
        int upper = cdf_len - 1;  // CDF 最大有效索引
        // 从 PDF: pdf[k] = cdf[k] - cdf[k-1], for k=1..upper
        // MRL(t) = Σ_{k>t} (k-t) · pdf[k] / (1 - cdf[t])
        for (int t = 0; t <= upper - 1 && t <= max_x; ++t) {
            double surv_t = 1.0 - cdf[t];
            if (surv_t < 1e-9) break;
            double num = 0.0;
            for (int k = t + 1; k <= upper; ++k) {
                double pdf_k = cdf[k] - cdf[k-1];
                num += (double)(k - t) * pdf_k;
            }
            tmrl[t] = num / surv_t;
        }
        return tmrl;
    };
    auto theory_mrl_all = computeTheoryMRL(theory_cdf_all, theory_cdf_all_len);
    auto theory_mrl_up  = computeTheoryMRL(theory_cdf_up,  theory_cdf_up_len);

    // ---- Y 轴范围: 取所有 MRL 值的最大值 ----
    double max_y = 1.0;
    for (int t = 0; t <= max_x; ++t) {
        if (mrl_all.first[t] > max_y) max_y = mrl_all.first[t];
        if (mrl_up.first[t]  > max_y) max_y = mrl_up.first[t];
        if (theory_mrl_all[t] > max_y) max_y = theory_mrl_all[t];
        if (theory_mrl_up[t]  > max_y) max_y = theory_mrl_up[t];
    }
    // 取整到 10 的倍数,留 10% 顶部空间
    max_y = std::ceil(max_y * 1.1 / 10.0) * 10.0;
    if (max_y < 10) max_y = 10;

    // ---- 网格 + 坐标轴 ----
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 230, 230, 230), DPIScaleF(1.0f));
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 80, 80, 80),  DPIScaleF(1.0f));
    float plotX = (float)rect.X + DPIScaleF(50.0f);
    float plotY = (float)rect.Y + DPIScaleF(40.0f);
    float plotW = (float)rect.Width  - DPIScaleF(70.0f);
    float plotH = (float)rect.Height - DPIScaleF(60.0f);
    if (plotW <= 0 || plotH <= 0) return;

    g.DrawLine(&axisPen, plotX, plotY,         plotX, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);

    auto getPt = [&](int x, double y) -> Gdiplus::PointF {
        if (y < 0) y = 0; if (y > max_y) y = max_y;
        return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW,
                               plotY + plotH - (float)(y / max_y) * plotH);
    };

    Gdiplus::Font tickFont(&fontFamily, DPIScaleF(11.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));

    // Y 轴刻度 (单位: 抽)
    for (int i = 0; i <= 4; ++i) {
        double y_val = max_y * (double)i / 4.0;
        float py = plotY + plotH - (float)i / 4.0f * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[16]; swprintf(y_label, 16, L"%.0f", y_val);
        float labelW = (float)wcslen(y_label) * DPIScaleF(5.5f) + DPIScaleF(8.0f);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - labelW, py - DPIScaleF(6.0f)), &tickBrush);
    }
    // X 轴刻度
    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        float xoff = (x < 10 ? 4.0f : x < 100 ? 8.0f : 12.0f) * DPIScaleF(1.0f);
        g.DrawString(x_label, -1, &tickFont,
                     Gdiplus::PointF(px - xoff, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    // ---- 画理论 MRL (虚线) ----
    // 用 GraphicsPath 一次性 stroke, dash pattern 沿连续路径走。
    // 注: 实际 theoryMRL 在 [0, cap] 区间是连续单调的(不会出现 -1 中段断开),
    // 所以单一 Path 一次构建即可,无需处理多段。
    auto drawTheoryMRL = [&](const std::array<double, 150>& tmrl, int cap, Gdiplus::Color color) {
        Gdiplus::Pen pen(color, DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        pen.SetDashPattern(dash, 2);
        int upper = (cap > 0 && cap <= max_x) ? cap : max_x;
        Gdiplus::GraphicsPath path;
        Gdiplus::PointF prev;
        bool has_prev = false;
        for (int t = 0; t <= upper; ++t) {
            if (tmrl[t] < 0) continue;
            auto p = getPt(t, tmrl[t]);
            if (has_prev) path.AddLine(prev, p);
            prev = p; has_prev = true;
        }
        if (has_prev) g.DrawPath(&pen, &path);
    };
    drawTheoryMRL(theory_mrl_all, theory_all_cap, Gdiplus::Color(180, 65, 140, 240));
    drawTheoryMRL(theory_mrl_up,  theory_up_cap,  Gdiplus::Color(180, 240, 80, 80));

    // ---- 画经验 MRL (实线: surv>=2; 虚线: surv==1) ----
    // 接受 RGB 三个分量(避免依赖 Gdiplus::Color::GetR/G/B 的 SDK 兼容问题)
    //
    // 关键: 实线段和虚线段分别攒到两个 GraphicsPath, 各自一次性 stroke。
    // 旧版逐段 DrawLine 会让 dash pattern 在每个 1px 短段重启,虚线视觉上糊成
    // 几乎实线的细线 —— 这正是用户截图里"红色虚线最后变实线"的根因。
    // dash 用 4/3 与理论 MRL 统一。
    auto drawEmpiricalMRL = [&](const std::pair<std::array<double, 150>, std::array<int, 150>>& mrl_data,
                                 BYTE r, BYTE gC, BYTE b) {
        const auto& mrl = mrl_data.first;
        const auto& surv = mrl_data.second;
        Gdiplus::Pen solidPen(Gdiplus::Color(255, r, gC, b), DPIScaleF(2.5f));
        Gdiplus::Pen dashPen(Gdiplus::Color(180, r, gC, b),  DPIScaleF(1.8f));
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        dashPen.SetDashPattern(dash, 2);

        // 累积:实线段进 solidPath,虚线段进 dashPath (每段都 StartFigure 隔开)
        Gdiplus::GraphicsPath solidPath, dashPath;
        Gdiplus::PointF prev; bool has_prev = false; bool prev_solid = true;
        for (int t = 0; t <= max_x; ++t) {
            if (mrl[t] < 0 || surv[t] == 0) {
                if (has_prev) {
                    solidPath.StartFigure();
                    dashPath.StartFigure();
                }
                has_prev = false; continue;
            }
            auto p = getPt(t, mrl[t]);
            bool solid = (surv[t] >= 2);
            if (has_prev) {
                if (solid && prev_solid) solidPath.AddLine(prev, p);
                else                     dashPath.AddLine(prev, p);
            }
            prev = p; has_prev = true; prev_solid = solid;
        }
        g.DrawPath(&solidPen, &solidPath);
        g.DrawPath(&dashPen,  &dashPath);
    };
    drawEmpiricalMRL(mrl_all, 65, 140, 240);
    drawEmpiricalMRL(mrl_up,  240, 80, 80);

    // ---- "你在这里" 竖线 (当前 censored_pity 位置) ----
    // 关键设计:
    //   - 综合 (蓝): 优先用理论 MRL, 否则降级到经验 MRL
    //   - UP (红):   传 useTheory=false, 只用经验 MRL (没有 UP 理论 CDF)
    //   - 虚线在 X 位置画出, 但标签固定在 plot 区域右上角竖排堆叠。
    //     避免: 标签贴虚线时碰到 X=1 这种边界情况会被裁切, 也避免红蓝标签互相重叠
    //     (例如两个 censored 数值接近时旧逻辑会把两段文本叠在一起)。
    //     视觉对应: 标签自带颜色, 用户能看出"蓝色标签对应蓝色虚线"。

    // (1) 先画虚线, 同时收集要展示的 (text, color) 条目
    struct CensoredLabel {
        std::wstring text;
        Gdiplus::Color color;
    };
    std::vector<CensoredLabel> censoredLabels;
    censoredLabels.reserve(2);

    auto resolveAndDrawLine = [&](int censored,
                                   const std::pair<std::array<double, 150>, std::array<int, 150>>& mrl_data,
                                   const std::array<double, 150>& tmrl,
                                   bool useTheory, int theory_cap,
                                   BYTE r, BYTE gC, BYTE b) {
        if (censored <= 0 || censored > max_x) return;
        double y_value = -1.0;
        if (useTheory && censored < (int)tmrl.size() && tmrl[censored] > 0
            && (theory_cap == 0 || censored <= theory_cap)) {
            y_value = tmrl[censored];
        } else if (mrl_data.first[censored] > 0) {
            y_value = mrl_data.first[censored];
        }
        if (y_value <= 0) return;
        Gdiplus::Color color(255, r, gC, b);
        Gdiplus::Pen markPen(color, DPIScaleF(1.5f));
        // dash pattern 与理论 CDF/MRL 保持一致 (4/3),让所有虚线视觉风格统一
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        markPen.SetDashPattern(dash, 2);
        auto top = getPt(censored, y_value);
        g.DrawLine(&markPen, top.X, top.Y, top.X, plotY + plotH);

        // 收集标签 (新格式: 单行, 用中点分隔; 右上角空间足够)
        wchar_t lbl[64];
        swprintf(lbl, 64, L"已垫 %d 抽 · 预期还需 %.1f", censored, y_value);
        censoredLabels.push_back({ std::wstring(lbl), color });
    };
    resolveAndDrawLine(censored_all, mrl_all, theory_mrl_all, true,  theory_all_cap, 65, 140, 240);
    resolveAndDrawLine(censored_up,  mrl_up,  theory_mrl_up,  false, theory_up_cap,  240, 80, 80);

    // (2) 在 plot 区域右上角内侧固定位置堆叠标签
    //     锚点右对齐, 行高约 14pt
    //
    //     图例改为水平横排后只占 rect.Y+12 那一行 (与 macOS/iOS 一致),
    //     不再下沉到绘图区,所以标签可以从 plotY+6 紧贴绘图区顶部起步。
    if (!censoredLabels.empty()) {
        Gdiplus::StringFormat fmtRight;
        fmtRight.SetAlignment(Gdiplus::StringAlignmentFar);     // 水平右对齐
        fmtRight.SetLineAlignment(Gdiplus::StringAlignmentNear); // 顶部对齐

        Gdiplus::SolidBrush whiteBr(Gdiplus::Color(255, 252, 253, 255));

        const float anchorX = plotX + plotW - DPIScaleF(6.0f);
        const float anchorY = plotY + DPIScaleF(6.0f);
        const float lineHeight = DPIScaleF(16.0f);

        for (size_t i = 0; i < censoredLabels.size(); ++i) {
            const auto& entry = censoredLabels[i];
            float ly = anchorY + (float)i * lineHeight;
            // 用一个点+右对齐 StringFormat 直接定位文字右上角
            Gdiplus::PointF pt(anchorX, ly);
            // 白色描边: 4 个对角偏移画白底字提升可读性
            for (int dx = -1; dx <= 1; dx += 2) {
                for (int dy = -1; dy <= 1; dy += 2) {
                    g.DrawString(entry.text.c_str(), -1, &tickFont,
                                 Gdiplus::PointF(pt.X + (float)dx, pt.Y + (float)dy),
                                 &fmtRight, &whiteBr);
                }
            }
            // 主文本
            Gdiplus::SolidBrush lblBrush(entry.color);
            g.DrawString(entry.text.c_str(), -1, &tickFont, pt, &fmtRight, &lblBrush);
        }
    }

    // ---- 图例 (3 项水平排列: 综合实线 / UP 实线 / 理论值虚线) ----
    // 与 macOS / iOS 端布局对齐 —— 标题旁同一行,从右向左排,
    // 这样图例完全位于标题区(rect.Y+12 行),不会下沉到绘图区(rect.Y+40 起)。
    Gdiplus::Font legendFont(&fontFamily, DPIScaleF(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush blueBr(Gdiplus::Color(255, 65, 140, 240));
    Gdiplus::SolidBrush redBr (Gdiplus::Color(255, 240, 80, 80));

    const wchar_t* legAll  = L"综合六星 剩余期望";
    const wchar_t* legUp   = L"当期限定 UP 剩余期望";
    const wchar_t* legThy  = L"理论值 (综合)";

    auto measureW = [&](const wchar_t* s) -> float {
        Gdiplus::RectF box;
        g.MeasureString(s, -1, &legendFont, Gdiplus::PointF(0, 0), &box);
        return box.Width;
    };
    const float swatchW    = DPIScaleF(14.0f);
    const float swatchGap  = DPIScaleF(6.0f);
    const float entryGap   = DPIScaleF(16.0f);
    const float legendY    = (float)rect.Y + DPIScaleF(12.0f);
    const float swatchYOff = DPIScaleF(8.0f);

    float wAll  = measureW(legAll);
    float wUp   = measureW(legUp);
    float wThy  = measureW(legThy);
    float xRight = (float)rect.X + (float)rect.Width - DPIScaleF(12.0f);

    // 第 3 项: 理论值 (虚线,最右)
    float xThyText = xRight - wThy;
    float xThySw   = xThyText - swatchGap - swatchW;
    {
        Gdiplus::Pen dashPen(Gdiplus::Color(255, 130, 130, 130), DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(2.5f), DPIScaleF(2.0f) };
        dashPen.SetDashPattern(dash, 2);
        g.DrawLine(&dashPen, xThySw, legendY + swatchYOff,
                   xThySw + swatchW, legendY + swatchYOff);
    }
    g.DrawString(legThy, -1, &legendFont,
                 Gdiplus::PointF(xThyText, legendY), &textBrush);

    // 第 2 项: UP 剩余期望
    float xUpText = xThySw - entryGap - wUp;
    float xUpSw   = xUpText - swatchGap - swatchW;
    g.FillRectangle(&redBr, xUpSw, legendY + swatchYOff - DPIScaleF(1.5f),
                    swatchW, DPIScaleF(3.0f));
    g.DrawString(legUp, -1, &legendFont,
                 Gdiplus::PointF(xUpText, legendY), &textBrush);

    // 第 1 项: 综合剩余期望
    float xAllText = xUpSw - entryGap - wAll;
    float xAllSw   = xAllText - swatchGap - swatchW;
    g.FillRectangle(&blueBr, xAllSw, legendY + swatchYOff - DPIScaleF(1.5f),
                    swatchW, DPIScaleF(3.0f));
    g.DrawString(legAll, -1, &legendFont,
                 Gdiplus::PointF(xAllText, legendY), &textBrush);
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
        // 角色 ECDF: X 轴覆盖 UP 硬保底 120 (UP 分布延伸到此)
        // 理论 CDF 仅画综合(g_cdf_char), UP 理论分布需要更复杂的卷积建模, 暂不画
        DrawECDF  (g, Gdiplus::Rect(DPIScale(20),  DPIScale(360), DPIScale(600), DPIScale(250)),
                   statsChar.freq_all, statsChar.freq_up,
                   statsChar.count_all, statsChar.count_up,
                   statsChar.censored_pity_all, statsChar.censored_pity_up,
                   g_cdf_char, 82, nullptr, 0,
                   L"角色累积分布 (ECDF)", 120);
        // 角色 MRL: X=80 是综合 6 星硬保底 (理论 MRL 上限), X=120 是 UP 硬保底
        DrawMRL   (g, Gdiplus::Rect(DPIScale(640), DPIScale(360), DPIScale(600), DPIScale(250)),
                   statsChar.freq_all, statsChar.freq_up,
                   statsChar.count_all, statsChar.count_up,
                   statsChar.censored_pity_all, statsChar.censored_pity_up,
                   g_cdf_char, 82, nullptr, 0,
                   L"角色剩余抽数期望 (MRL)", 120,
                   /*theory_all_cap=*/80, /*theory_up_cap=*/120);
        // 武器 ECDF: X 轴覆盖 UP 硬保底 80
        DrawECDF  (g, Gdiplus::Rect(DPIScale(20),  DPIScale(615), DPIScale(600), DPIScale(250)),
                   statsWep.freq_all, statsWep.freq_up,
                   statsWep.count_all, statsWep.count_up,
                   statsWep.censored_pity_all, statsWep.censored_pity_up,
                   g_cdf_wep, 41, nullptr, 0,
                   L"武器累积分布 (ECDF)", 80);
        // 武器 MRL: X=40 综合硬保底, X=80 UP 硬保底
        DrawMRL   (g, Gdiplus::Rect(DPIScale(640), DPIScale(615), DPIScale(600), DPIScale(250)),
                   statsWep.freq_all, statsWep.freq_up,
                   statsWep.count_all, statsWep.count_up,
                   statsWep.censored_pity_all, statsWep.censored_pity_up,
                   g_cdf_wep, 41, nullptr, 0,
                   L"武器剩余抽数期望 (MRL)", 80,
                   /*theory_all_cap=*/40, /*theory_up_cap=*/80);
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

        DWORD tabStops[] = {50};
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
        // 异步提交;Submit 内部做双开保护(g_processing CAS 锁)
        // 失败(已有 worker 在跑或 I/O 失败)时静默忽略,UI 上保留之前的状态
        ProcessFile_Submit(hwnd, filePath);
        break;
    }
    case WM_APP_PROCESS_DONE: {
        // worker 完成,主线程消费结果(更新全局 stats、刷新 UI)
        auto* out = (ProcessOutput*)lParam;
        ProcessFile_Consume(hwnd, out);
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
