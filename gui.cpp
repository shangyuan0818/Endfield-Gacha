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

void InitCDFTables() {
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
}

// 修复:离散阶梯 CDF 的 K-S 统计量需严格对齐两条阶梯
// 在 x 处,两条阶梯的"底":F_n(cum before x),F_theory(x-1)
// 在 x 处,两条阶梯的"顶":F_n(cum after x),F_theory(x)
// 原版用 fn_before 减 cdf_table[x] —— 拿经验阶梯底对理论阶梯顶,
// 人为引入 h_x 的单点跳跃(软保底段可高达 5%+),造成巨大伪偏差
double ComputeKS(const std::array<int, 150>& freq, int max_pity, int n,
                 const double* cdf_table, int cdf_len) {
    if (n == 0) return 0.0;
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

            // win_5050 分子:
            //   - 角色池:只在 50/50 阶段(!had_non_up)的 UP 才计入,大保底必中 UP 不算
            //   - 武器池:每个 UP 都独立计入(无大保底概念,每个 6 星都是独立 25% 判定)
            // count_win/sum_win (avg_win "不歪出货期望") 仅对角色池有物理含义
            if (isWeapon) {
                acc.win_5050++;
            } else if (!had_non_up) {
                acc.win_5050++;
                acc.count_win++;
                acc.sum_win += current_pity;
            }
            had_non_up = false;
            pity_since_last_up = 0;
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
        current_pity = 0;
    }

    // 右删失:遍历结束时若仍有未结算的 pity,记录为删失样本
    // 这些抽数"存活"到了 current_pity 抽仍未出 6 星(或 UP)
    acc.censored_pity_all = current_pity;
    acc.censored_pity_up  = pity_since_last_up;

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

    // 角色池:"赢下小保底(不歪)的出货期望" — avg_win 记录的是 50/50 阶段直接出 UP 的抽数
    wchar_t winCharStr[64] = L"[无数据]";
    if (statsChar.avg_win >= 0) swprintf(winCharStr, 64, L"%.2f 抽", statsChar.avg_win);

    // 当前垫刀信息(右删失样本),附在输出末尾
    wchar_t pendCharStr[96] = L"";
    if (statsChar.censored_pity_all > 0 || statsChar.censored_pity_up > 0) {
        swprintf(pendCharStr, 96, L"  [当前垫刀: 距上次六星 %d 抽 / 距上次 UP %d 抽]",
                 statsChar.censored_pity_all, statsChar.censored_pity_up);
    }
    wchar_t pendWepStr[96] = L"";
    if (statsWep.censored_pity_all > 0 || statsWep.censored_pity_up > 0) {
        swprintf(pendWepStr, 96, L"  [当前垫刀: 距上次六星 %d 抽 / 距上次 UP %d 抽]",
                 statsWep.censored_pity_all, statsWep.censored_pity_up);
    }

    // KS 标签(角色池/武器池对称):样本为 0 时显示"-",否则判定"符合/偏离"
    auto ksLabel = [](const StatsResult& r) -> const wchar_t* {
        if (r.count_all == 0) return L"-";
        return r.ks_is_normal ? L"符合理论模型" : L"偏离过大";
    };
    const wchar_t* ksCharLabel = ksLabel(statsChar);
    const wchar_t* ksWepLabel  = ksLabel(statsWep);

    // 理论参考数值:
    //   角色池(ggpipi 模型):综合六星 ≈ 51.81 抽,首 UP ≈ 74.33 抽,不歪率 50%
    //   武器池(Reddit 模型):首 6 星 ≈ 19.17 抽,首 UP ≈ 81.66 抽,UP 条件率 25%
    wchar_t outMsg[2560];
    swprintf(outMsg, 2560,
        L"【角色卡池 (特许寻访)】 总计六星: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合六星 (含歪) 出货平均期望:     %5.2f 抽 (理论 ≈ 51.81)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 74.33)   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (理论 50%%) (%d胜%d负)\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\r\n\r\n"
        L"【武器卡池 (武库申领)】 总计六星: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合六星出货平均期望:             %5.2f 抽 (理论 ≈ 19.17)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 81.66)   [95%% CI: %5.1f ~ %5.1f]    |   6 星中 UP 率: %5.1f%% (理论 25%%) (%d UP / %d 非UP)",
        statsChar.count_all, statsChar.count_up, pendCharStr,
        statsChar.avg_all, (std::max)(1.0, statsChar.avg_all - statsChar.ci_all_err),
        statsChar.avg_all + statsChar.ci_all_err, statsChar.cv_all * 100.0, statsChar.ks_d_all,
        ksCharLabel,
        statsChar.avg_up, (std::max)(1.0, statsChar.avg_up - statsChar.ci_up_err),
        statsChar.avg_up + statsChar.ci_up_err,
        statsChar.win_rate_5050 >= 0 ? statsChar.win_rate_5050 * 100.0 : 0.0,
        statsChar.win_5050, statsChar.lose_5050, winCharStr,
        statsWep.count_all, statsWep.count_up, pendWepStr,
        statsWep.avg_all, (std::max)(1.0, statsWep.avg_all - statsWep.ci_all_err),
        statsWep.avg_all + statsWep.ci_all_err, statsWep.cv_all * 100.0, statsWep.ks_d_all,
        ksWepLabel,
        statsWep.avg_up, (std::max)(1.0, statsWep.avg_up - statsWep.ci_up_err),
        statsWep.avg_up + statsWep.ci_up_err,
        statsWep.win_rate_5050 >= 0 ? statsWep.win_rate_5050 * 100.0 : 0.0,
        statsWep.win_5050, statsWep.lose_5050
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
        long long sum = 0;
        long long sum_sq = 0;
        for (int i = 1; i <= max_x; i++) {
            total  += freqs[i];
            sum    += (long long)i * freqs[i];
            sum_sq += (long long)i * i * freqs[i];
        }
        if (total == 0) return;

        // Silverman 经验法则:h = 1.06 · σ · n^(-1/5)
        // σ 用无偏样本标准差(文献中的 Silverman 带宽公式基于样本 sd,非 MLE)
        // 回退条件仅防止数值退化(n<5 样本量不足以估计 σ,σ=0 会导致 h=0 除零)
        double var = SampleVariance(sum, sum_sq, total);
        double sd  = std::sqrt(var);
        double bandwidth = (total >= 5 && sd > 0.5)
                             ? 1.06 * sd * std::pow((double)total, -0.2)
                             : 4.0;  // 样本不足时的回退值,仅避免数值问题

        int spread = (int)(4.0 * bandwidth) + 1;

        // 第一步:标准高斯核累加(无任何边界假设)
        for (int v = 1; v <= max_x; ++v) {
            if (freqs[v] == 0) continue;
            int lo = (std::max)(1, v - spread);
            int hi = (std::min)(max_x, v + spread);
            for (int x = lo; x <= hi; ++x) {
                double u = (x - v) / bandwidth;
                out_curve[x] += freqs[v] * std::exp(-0.5 * u * u);
            }
        }

        // 第二步:Jones(1993) 边界修正 —— renormalization method
        //
        // 问题:真实分布支撑集 ⊂ [1, max_x](角色池硬保底 80、UP 硬保底 120,武器池类似)
        //       高斯核会向 [−∞, 0] 和 [max_x+1, +∞] 方向"泄漏"概率质量,
        //       导致 ∫_{[1, max_x]} f_hat(x)dx < 1,边界附近密度被系统性低估
        //
        // 反射法(Reflection)会假设"镜像对称",但抽卡分布在 x=30 尖峰、x=66..80
        //   软保底爬坡处**不对称**,反射会引入虚假结构
        //
        // 这里用重新归一化法:不假设对称,只补偿被截断的核质量
        //   C(x) = 1 / P(x − h·U ∈ [0.5, max_x + 0.5])
        //        = 1 / [Φ((max_x+0.5 − x)/h) − Φ((0.5 − x)/h)]
        // (正态 CDF Φ 用 erf 计算,离散 x 把整数 k 理解为 [k−0.5, k+0.5] 的格子中心)
        constexpr double SQRT_2PI = 2.5066282746310002;
        constexpr double INV_SQRT2 = 0.7071067811865475;
        double left_edge  = 0.5;
        double right_edge = (double)max_x + 0.5;
        for (int x = 1; x <= max_x; ++x) {
            // 标准正态 CDF: Φ(z) = 0.5 × (1 + erf(z/√2))
            double z_right = (right_edge - x) / bandwidth * INV_SQRT2;
            double z_left  = (left_edge  - x) / bandwidth * INV_SQRT2;
            double mass = 0.5 * (std::erf(z_right) - std::erf(z_left));
            if (mass > 1e-12) out_curve[x] /= mass;
        }

        // 第三步:完整归一化 1 / (n · h · √(2π))
        //   这一步让"每个数据点贡献"的核 f_v(x) = K((x−v)/h)/(h√(2π)) 满足 ∫ f_v = 1
        //   配合第二步的边界补偿,最终得到 Σ_{x=1..max_x} f_hat(x) = 1 (离散密度)
        double inv_norm = 1.0 / ((double)total * bandwidth * SQRT_2PI);
        for (int x = 1; x <= max_x; ++x) out_curve[x] *= inv_norm;
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
    // hazard_all 理论只分布在 [1, theory_all_cap](例如 80),超出部分不该有数据
    int cap_all = (theory_all_cap > 0) ? theory_all_cap : max_x;
    for (int x = 1; x <= (std::min)(max_x, cap_all); x++) {
        if (hazard_all[x] > 0) {
            Gdiplus::PointF p = getPt(x, hazard_all[x]);
            g.FillRectangle(&brushAll, p.X - barW, p.Y, barW, plotY + plotH - p.Y);
        }
    }
    Gdiplus::SolidBrush brushUp(Gdiplus::Color(180, 240, 80, 80));
    // hazard_up 理论分布到 [1, theory_up_cap](例如 120)
    int cap_up = (theory_up_cap > 0) ? theory_up_cap : max_x;
    for (int x = 1; x <= (std::min)(max_x, cap_up); x++) {
        if (hazard_up[x] > 0) {
            Gdiplus::PointF p = getPt(x, hazard_up[x]);
            g.FillRectangle(&brushUp, p.X, p.Y, barW, plotY + plotH - p.Y);
        }
    }

    // 理论上限分隔线:hazard_all 的硬保底位置(例如 80)
    // 超过这条线本不该再出现蓝色柱子 —— 如果出现,说明数据异常
    if (theory_all_cap > 0 && theory_all_cap < max_x) {
        Gdiplus::Pen capPen(Gdiplus::Color(200, 180, 180, 180), DPIScaleF(1.0f));
        Gdiplus::REAL dashPattern[] = { 4.0f, 3.0f };
        capPen.SetDashPattern(dashPattern, 2);
        float cx = plotX + (float)theory_all_cap / (float)max_x * plotW;
        g.DrawLine(&capPen, cx, plotY, cx, plotY + plotH);

        Gdiplus::Font capFont(&fontFamily, DPIScaleF(10.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush capBrush(Gdiplus::Color(255, 150, 150, 150));
        wchar_t capLabel[32];
        swprintf(capLabel, 32, L"理论上限 %d", theory_all_cap);
        g.DrawString(capLabel, -1, &capFont,
                     Gdiplus::PointF(cx + DPIScaleF(3.0f), plotY + DPIScaleF(2.0f)), &capBrush);
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
        // 角色 KDE:X 轴覆盖 UP 硬保底 120(UP 分布延伸到此)
        DrawKDE   (g, Gdiplus::Rect(DPIScale(20),  DPIScale(360), DPIScale(600), DPIScale(250)),
                   statsChar.freq_all, statsChar.freq_up, L"角色期望核密度", 120);
        // 角色风险函数:X=80 是综合 hazard_all 的硬保底,X=120 是 hazard_up 的硬保底
        DrawHazard(g, Gdiplus::Rect(DPIScale(640), DPIScale(360), DPIScale(600), DPIScale(250)),
                   statsChar.hazard_all, statsChar.hazard_up, L"角色经验风险函数", 120,
                   /*theory_all_cap=*/80, /*theory_up_cap=*/120);
        // 武器 KDE:X 轴覆盖 UP 硬保底 80
        DrawKDE   (g, Gdiplus::Rect(DPIScale(20),  DPIScale(615), DPIScale(600), DPIScale(250)),
                   statsWep.freq_all, statsWep.freq_up, L"武器期望核密度", 80);
        // 武器风险函数:X=40 是综合 hazard_all 的硬保底(十连粒度),
        //              X=80 是 hazard_up 的硬保底(UP 保底十连)
        DrawHazard(g, Gdiplus::Rect(DPIScale(640), DPIScale(615), DPIScale(600), DPIScale(250)),
                   statsWep.hazard_all, statsWep.hazard_up, L"武器经验风险函数", 80,
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
