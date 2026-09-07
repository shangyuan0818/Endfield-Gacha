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
#include <span>           // v0.1.3.3: 理论 CDF 表改用 std::span 传参
#include <cstdint>
#include <memory>      // std::make_unique_for_overwrite (C++20) —— worker 的 2MB PMR arena 用它在堆上不清零分配
#include <process.h>   // _beginthreadex / _endthreadex(调用 CRT 的线程应走这个而非裸 CreateThread)

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------
// [枚举降维]
// ---------------------------------------------------------
enum class ItemType  : uint8_t { Unknown = 0, Character, Weapon };
enum class RankType  : uint8_t { Unknown = 0, Rank3 = 3, Rank4 = 4, Rank5 = 5, Rank6 = 6 };
enum class GachaType : uint8_t { Unknown = 0, Beginner, Standard, Special, Constant, Joint, Refactor };

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
    //
    // Joint 池 (辉光庆典) 的 pool_type = E_CharacterGachaPoolType_Joint,
    // poolId 形如 "joint_1_2_2"。两条匹配路径任一命中均可:
    //   - Special 池: poolId 形如 "special_*",pool_type 含 "Special"
    //   - Joint   池: poolId 形如 "joint_*",  pool_type 含 "Joint"
    // 注意先匹配 Joint 再匹配 Standard, 避免 "Joint"/"Constant" 等子串误判
    // (这里实际没有冲突,纯粹是命中率优化:UIGF v4.2 里 gacha_type 值的精确字符串
    // 是 "E_CharacterGachaPoolType_Joint" 等, ContainsCI 检查 "joint" 即可)
    //
    // Refactor 池 (重构寻访, 1.5「雪凇幽梦」新增, 客户端 GachaCharPoolTypeTable type=4):
    //   poolId 形如 "rerun_chr_yvonne" (角色) / "rerun_wpn_yvonne" (武器),
    //   /api/record/char 的 pool_type 枚举为 E_CharacterGachaPoolType_Rerun
    //   —— 该枚举已向官方接口实测确认 (见 main.cpp 中 pools 表的说明), 不是猜测。
    //   本导出器把 poolId 写进 UIGF 的 gacha_type, 故这里匹配 "rerun";
    //   同时兼容其它工具可能写入的 "refactor" 拼法。
    if (ContainsCI(sv, "rerun"))    return GachaType::Refactor;
    if (ContainsCI(sv, "refactor")) return GachaType::Refactor;
    if (ContainsCI(sv, "joint"))    return GachaType::Joint;
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
// 统计热路径 (非六星的 [[likely]] 分支) 只访问紧凑的标量数组:
//   rank_types / is_free / starts_new_banner —— 全是枚举/单字节, 顺序扫描, 缓存友好。
// names / poolNames 仅在少量六星记录做 UP 判定/映射查找时才访问 (会触达 mmap 字节)。
// is_free: 标记"第30抽赠送十连"的成员,该机制不占用也不增加保底进度
//   (依据《明日方舟终末地抽卡机制解析》2.1.1)
// starts_new_banner (v0.1.3.2): 分桶阶段预计算的"本条是否为新一期卡池起点"字节标记。
//   取代 Calculate 里每条记录对 poolNames[i] vs poolNames[i-1] 的 string_view 比较 ——
//   那个比较即便对非六星也要跑, 且 operator!= 在等长时要 memcmp mmap 里的池名字节,
//   削弱 SoA 收益。预计算后热路径只读一个字节; 池名仅留给六星 UP 查找。
// ---------------------------------------------------------
struct PullBucket {
    std::pmr::vector<RankType>         rank_types;
    std::pmr::vector<std::string_view> names;
    std::pmr::vector<std::string_view> poolNames;
    std::pmr::vector<uint8_t>          is_free;
    std::pmr::vector<uint8_t>          starts_new_banner;  // v0.1.3.2: 1 = 本条 poolName 与上一条不同

    explicit PullBucket(std::pmr::polymorphic_allocator<std::byte> alloc)
        : rank_types(alloc), names(alloc), poolNames(alloc),
          is_free(alloc), starts_new_banner(alloc) {}

    void reserve(size_t cap) {
        rank_types.reserve(cap); names.reserve(cap);
        poolNames.reserve(cap);  is_free.reserve(cap);
        starts_new_banner.reserve(cap);
    }
    void push_back(RankType rt, std::string_view name, std::string_view pool, uint8_t free_flag) {
        // 与上一条 (同桶内) 比较池名, 把"是否新卡池起点"在分桶时算好。首条 (桶为空) 记 0 ——
        // 等价于 Calculate 原来的 i>0 守卫。此处比较的是刚解析、仍在缓存里的 mmap 字节,
        // 比挪到统计热路径里逐条比更划算 (统计循环每条都要跑、且会污染 SoA 的顺序访问)。
        uint8_t starts_new = (!poolNames.empty() && poolNames.back() != pool) ? 1 : 0;
        rank_types.push_back(rt); names.push_back(name);
        poolNames.push_back(pool); is_free.push_back(free_flag);
        starts_new_banner.push_back(starts_new);
    }
    size_t size() const { return rank_types.size(); }
};

// StatsAccumulator: Calculate() 内的单线程累加器 (局部变量 acc), 不存在多核并发写,
// 不需要 cache-line 对齐 —— 故不加 alignas (旧版的 alignas(128) 在单线程下是无操作,
// 留着只会误导维护者以为这里有并发)。将来若改成多线程分片归约、每线程持有相邻
// accumulator, 再按实际 cache-line 布局补 padding 防 false sharing 即可。
struct StatsAccumulator {
    std::array<int, 260> freq_all{};
    std::array<int, 260> freq_up{};
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
    std::array<int, 260> freq_all{};
    std::array<int, 260> freq_up{};
    int count_all = 0, count_up = 0;
    double avg_all = 0.0, avg_up = 0.0, avg_win = -1.0;
    double cv_all = 0.0, ci_all_err = 0.0, ci_up_err = 0.0;
    int win_5050 = 0, lose_5050 = 0;
    double win_rate_5050 = -1.0;
    std::array<double, 260> hazard_all{}, hazard_up{};
    double ks_d_all = 0.0, ks_d_up = 0.0;
    bool ks_is_normal = true, ks_is_normal_up = true;
    // v0.1.4.0: UP 侧样本是否为"两种分布的混合", 混合时不输出拟合判定 (见 Calculate)
    bool ks_up_mixed = false;
    // 右删失(用于显示"当前已垫 N 抽")
    int censored_pity_all = 0;
    int censored_pity_up  = 0;
};

StatsResult statsChar, statsWep, statsJoint, statsRefactor;
HWND hOutEdit, hCharEdit, hWepEdit, hPoolMapEdit;
// v0.1.4.0: 三个说明标签也提成全局 —— 窗口可缩放后每次 WM_SIZE / 滚动都要重新摆位
HWND hHintLabel = NULL, hCharLabel = NULL, hPoolMapLabel = NULL, hWepLabel = NULL;
static int g_scrollY  = 0;   // 当前垂直滚动量 (物理像素)
static int g_contentH = 0;   // 最近一次布局算出的内容总高 (物理像素)
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
//     E[首 UP]  ≈ 54.74 抽(Reddit 四态递推 + 80 抽硬保底截断, 即 g_cdf_wep_up 模型
//                  均值 54.737。Reddit 原文 81.66 是"忽略 80 保底"的无截断值 ——
//                  经验均值必然 <=80, 应与含保底的 54.74 对照, 不应再拿 81.66 当参考。
//                  另: 经验 pity_up 记申领内单抽落点, 实测常比按申领末记账的 54.74
//                  再低 3~7 抽 (保底/强制出货的拨内落点游戏未公开)。)
//
// KS 理论 CDF 构造 — "距离上次 6 星的抽数 x"的分布:
//   角色池 g_cdf_char[x=0..80]:hazard 段函数积分
//   武器池 g_cdf_wep[x=0..40]:
//     x=1..30:   每抽独立 4%,P(x=k) = 0.96^(k-1) × 0.04
//     x=31..40:  保底十连条件分布,归一化常数 = 1 - 0.96^10
//
// UP 理论 CDF (新增, v0.1.1):
//   角色 UP g_cdf_char_up[x=0..120]: 双状态前向迭代算法 (docs §2.1.2)
//     综合 6 星出货后 50% 毕业 / 50% 重置水位; 第 120 抽硬保底强制毕业
//   武器 UP g_cdf_wep_up[x=0..80]:  4×8 状态机 (Reddit Step 4)
//     ns ∈ [0,3]: 已连续多少 10-pull 没出 6 星 (40 抽 6 星保底 → ns==3 必出)
//     nf ∈ [0,7]: 已连续多少 10-pull 没出 featured (80 抽 featured 保底 → nf==7 必出 featured)
//     展开成单抽索引时只在 10 倍数边界跳变 (机制本身决定, 拨内 CDF 平坦)
//
// 辉光庆典 UP (v0.1.2.4):
//   辉光庆典与 Special 池机制差异关键点:
//     (1) 池中 4 个 6 星均匀分布: 2 限定 + 2 常驻. P(限定|六星) = 2/4 = 50%
//     (2) 没有"大保底"——歪了下一次六星不保证是限定 (规则只列出 80 抽六星硬保底)
//     (3) 没有 120 抽 UP 硬保底——120/240 抽是赠送选择券(用户主动选, 与抽卡概率独立)
//   等价模型: 重复独立的"出 6 星"周期, 每周期 50% 概率出限定 (即停止). 周期内"距上次六星
//   x 抽"的分布 = g_cdf_char 描述的同一分布 (含 30 抽赠送十连和 80 抽硬保底).
//   完整理论期望: E[首限定] ≈ 104.68 抽 (由下方前向迭代真实算出)。
//   注意: 几何停止捷径 E[首6星]/0.5 = 51.81/0.5 = 103.62 并【不】成立 —— 30 抽赠送
//   十连绑定在"绝对第 30 抽"且每期一次, 不随六星周期重复; 歪后重开的周期均值是
//   53.90 (无赠送), 真值介于 103.62 与 2×53.90=107.80 之间, 迭代给出 104.68。
//
//   实现策略 (CDF 截断 + 解析长尾延伸):
//     - g_cdf_joint_up[242] 仅覆盖 X=0..240 (与图表 X 轴一致, CDF 在此处 ≈ 0.93).
//     - 数组截断的 ~7% 长尾质量通过 g_joint_tail_mean_excess 单点近似补回 MRL.
//     - 验证: cdf[240] + 长尾点质量 与 simulate 到 n=2000 精确 MRL 全程误差 < 1e-9 抽.
//     - 历史: v0.1.2.3 用过 g_cdf_joint_up[1002] 物理扩到 1000 抽让 CDF 自然收敛到
//             0.999993, 数学等价但浪费 ~8KB 静态内存, v0.1.2.4 改用解析延伸.
// 角色寻访的基础六星概率 (客户端 GachaCharPoolTypeTable: star6BaseRate = 8000 → 0.8%)。
// 赠送十连(加急招募)恒按【基础概率】判定, 不吃 66 抽起的软保底加成 —— 见下方各
// 免费十连展开循环。
constexpr double kBaseRate6 = 0.008;

static double g_cdf_char[82]   = {};  // x=0..80,角色池综合
static double g_cdf_wep[41]    = {};  // x=0..40,武器池综合
static double g_cdf_char_up[122] = {};  // x=0..120, 角色池 UP
static double g_cdf_wep_up[81]   = {};  // x=0..80,  武器池 UP
static double g_cdf_joint_up[242] = {}; // x=0..240, 辉光庆典 限定 (首个非常驻六星)
static double g_cdf_refactor[82]    = {};  // x=0..80,  重构寻访综合 (赠送十连节点在 30/60)
static double g_cdf_refactor_up[122] = {}; // x=0..120, 重构寻访 UP (系列首次, 含 120 硬保底)
// 辉光池 CDF 在 X=240 处仅 ~93%, 长尾用解析点质量延伸 (见 InitCDFTables 末尾):
//   g_joint_tail_mean_excess = E[首限定抽数 | 首限定 > 240] - 240
// 工程含义: 在 computeTheoryMRL 中, 对辉光池追加一项
//   (240 + g_joint_tail_mean_excess - t) × (1 - cdf[240])
// 让 MRL[0] 能从无延伸时的 ~82 修正回完整真值 ~104.68.
// 注: 这个量是机制参数 (hazard 函数, 50% 限定率, 30抽免费十连) 的函数, 不是写死常量;
//     InitCDFTables 末尾会临时 simulate 到 n=2000 算出, 保证机制改动后自动跟上.
static double g_joint_tail_mean_excess = 0.0;
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

    // ---- 角色池 UP 理论 CDF (g_cdf_char_up[0..120]) ----
    // 真实模型 (v0.1.3.0, 修正 v0.1.2.2 的“歪→下次必中”大保底错误):
    //   终末地特许寻访【没有原神/米池式大保底】: 小保底歪了之后, 下一次出六星仍是
    //   独立 50/50, 可以连续歪多次。唯一的 UP 兜底是【120 抽硬保底】(本期累计 120 抽
    //   必出 UP), 且 120 计数每期独立、不继承 (官方机制说明 + Wanuxi: “系统只在 120 抽
    //   触发保底, 这与一些同类游戏不同 —— 那些游戏小保底失败后下次必中”)。
    //   => 状态退化为单维 D[s] (与辉光池 g_cdf_joint_up 同构), 唯一差别是本池在
    //      n=120 强制所有“尚未出 UP”的存活者毕业 (辉光池无此硬保底)。
    //   D[s]: 水位 s ∈ [0,79] = 距上次出 6 星的抽数, 概率质量 = “尚未出 UP” 的人群。
    //   每抽:
    //     - 不出货: 概率 D[s]×(1-ph) → newD[s+1]
    //     - 出货 (独立 50/50): 50% 毕业(出 UP, 计入 cum);
    //                          50% 歪(水位归 0, 仍未出 UP → newD[0])
    //   n=30 特殊: 展开 11 次独立判定 (本体抽推进水位; 免费十连水位停, isFree 出货
    //              不重置水位; 与辉光池/经验代码一致)。
    //   n=120 硬保底: 所有存活者 (任意水位) 强制出 UP。
    //
    // 历史: v0.1.2.2 用二维 D[s][h] (h=大保底标志) 实现“歪→下次必中 UP”, 是把终末地
    //        误当成原神/米池模型。经联网核实, 终末地特许寻访【不存在】该大保底。
    //        该错误后果: E[首 UP] 偏低 (~74.16 vs 真值 ~79.29 原始抽), 120 硬保底
    //        触发率被低估 (~20.8% vs ~32.8%), 长尾 (80~119 段) 理论 CDF 系统性偏高
    //        约 0.12 (K-S 量级)。另注: 旧标注“理论≈74.33”其实是【净成本】(扣前 5 抽
    //        免费), 原始抽数真值 = 74.33+5 ≈ 79.29, 正好与本修正模型一致 ——
    //        当年错误模型给的 74.16 数值上贴近 74.33, 掩盖了 bug (拿原始抽对净成本)。
    {
        constexpr int hard_cap = 120;
        constexpr int max_soft = 80;
        auto h_char = [](int k) -> double {
            if (k <= 65)      return 0.008;
            else if (k <= 79) return 0.058 + (k - 66) * 0.05;
            else              return 1.0;
        };
        // 单维状态: D[s] = 水位 s 且“尚未出 UP”的概率 (无大保底标志, 每次出货独立 50/50)
        std::array<double, max_soft> D{}; D[0] = 1.0;
        double cum = 0.0;
        for (int n = 1; n <= hard_cap; ++n) {
            if (n == hard_cap) {
                // 120 硬保底: 所有“尚未出 UP”的存活者 (任意水位) 强制出 UP
                double alive = 0.0;
                for (int s = 0; s < max_soft; ++s) alive += D[s];
                cum += alive;
                g_cdf_char_up[n] = (std::min)(1.0, cum);
                for (int k = n + 1; k <= hard_cap + 1; ++k) g_cdf_char_up[k] = 1.0;
                break;
            }

            std::array<double, max_soft> newD{};
            double p_hit_grad = 0.0;

            if (n == 30) {
                // ===== n=30: 11 次独立判定 (本体抽 1 次 + 免费十连 10 次) =====
                std::array<double, max_soft> stateA{};
                for (int s = 0; s < max_soft; ++s) {
                    if (D[s] == 0) continue;
                    double ph = h_char(s + 1);
                    if (s + 1 < max_soft) stateA[s + 1] += D[s] * (1.0 - ph);
                    p_hit_grad += D[s] * ph * 0.5;   // 毕业 (出 UP)
                    stateA[0]  += D[s] * ph * 0.5;   // 歪, 水位归 0 (本体抽), 仍未出 UP
                }
                for (int free_step = 0; free_step < 10; ++free_step) {
                    std::array<double, max_soft> newStateA{};
                    for (int s = 0; s < max_soft; ++s) {
                        if (stateA[s] == 0) continue;
                        const double ph = kBaseRate6;   // 赠送十连走基础概率, 不吃软保底加成
                        newStateA[s] += stateA[s] * (1.0 - ph);   // 不出货, 水位停
                        p_hit_grad   += stateA[s] * ph * 0.5;     // 毕业 (出 UP)
                        newStateA[s] += stateA[s] * ph * 0.5;     // 歪, 水位停 (isFree)
                    }
                    stateA = newStateA;
                }
                newD = stateA;
            } else {
                // ===== 普通抽 =====
                for (int s = 0; s < max_soft; ++s) {
                    if (D[s] == 0) continue;
                    double ph = h_char(s + 1);
                    if (s + 1 < max_soft) newD[s + 1] += D[s] * (1.0 - ph);
                    p_hit_grad += D[s] * ph * 0.5;   // 毕业 (出 UP)
                    newD[0]    += D[s] * ph * 0.5;   // 歪, 水位归 0, 仍未出 UP
                }
            }

            cum += p_hit_grad;
            g_cdf_char_up[n] = (std::min)(1.0, cum);
            D = newD;
        }
    }

    // ---- 重构寻访 综合六星 CDF (g_cdf_refactor[0..80]) ----
    // 「重构寻访」(RE-Factor Headhunting) 是 1.5「雪凇幽梦」新增的第五种角色寻访类型,
    // 首期「绚丽异彩」重构寻访#1 于 2026/09/24 12:00 开启 (UP = 伊冯, 旧限定复刻)。
    //
    // 数值来源 (客户端 GachaCharPoolTypeTable type=4, 与特许寻访 type=0 逐字段比对):
    //   star6BaseRate            = 8000    → 0.8%      (与特许寻访相同)
    //   star6RatePromotePullCount= [66]              ┐ 第 66 抽起每抽 +5%
    //   star6RatePromoteValue    = [50000] → +5%     ┘ (与特许寻访相同)
    //   softGuarantee            = 80      → 80 抽硬保底出六星  (与特许寻访相同)
    //   hardGuarantee            = 120     → 120 抽必出 UP      (与特许寻访相同)
    //   shareSoftGuarantee       = true
    //   freeTenPullRewardPullCount = [30, 60, 90]  ← 【唯一的数值差异】
    //     特许寻访是 [30, 0, 0] (只在累计 30 抽送 1 次免费十连),
    //     重构寻访在累计 30 / 60 / 90 抽【各】送 1 次免费十连。
    //   testimonialPullCount     = 0       → 重构寻访没有特许寻访的 60 抽「寻访情报书」
    // 官方规则原文:《「雪凇幽梦」版本研发通讯》 https://endfield.hypergryph.com/news/4776
    //   (英文版 https://endfield.gryphline.com/en-us/news/4481)
    //
    // 本表与 g_cdf_char 的唯一差别: 赠送十连的合并 hazard 节点从「只有 30」变成「30 和 60」。
    // 为什么没有 90: 本表按【距上次六星的抽数 x】索引, 而 80 抽硬保底保证 x <= 80,
    //   所以累计第 90 抽的那次赠送十连在本表的坐标系里不可达 (它只能发生在某次六星之后,
    //   此时水位已经归零)。第 3 次赠送十连的贡献在经验侧被并入节点 60 (见 Calculate 中
    //   free_node_all 的说明) —— 这是与既有 g_cdf_char 同一类的、已知且刻意的近似:
    //   赠送十连绑定的是【本期累计抽数】而不是【水位】, 只有在该里程碑之前没出过六星时
    //   两者才重合。
    {
        double surv_rf = 1.0;
        for (int i = 1; i <= 80; ++i) {
            double p;
            if (i == 30 || i == 60) p = 1.0 - std::pow(1.0 - kBaseRate6, 11);  // 本体 1 抽 + 免费十连 10 抽
            else if (i <= 65)       p = 0.008;
            else if (i <= 79)       p = 0.058 + (i - 66) * 0.05;
            else                    p = 1.0;
            if (p > 1.0) p = 1.0;
            g_cdf_refactor[i] = g_cdf_refactor[i - 1] + surv_rf * p;
            surv_rf *= (1.0 - p);
        }
        g_cdf_refactor[81] = 1.0;
    }

    // ---- 重构寻访 UP 理论 CDF (g_cdf_refactor_up[0..120]) ----
    // 与 g_cdf_char_up 同构 (单维水位状态 + 每次出货独立 50/50 + n=120 硬保底强制毕业),
    // 唯一差别: 赠送十连展开点从 {30} 变成 {30, 60, 90}。
    // 注意本表按【累计抽数 n】索引 (不是水位), 所以 30/60/90 三个里程碑都能【精确】表达,
    // 不存在 g_cdf_refactor 那里的坐标系近似。
    //
    // 【重要假设 — 官方未公布】P(UP | 出六星) = 50%。
    //   官方对重构寻访只说「6星干员【伊冯】获取概率大幅提升」, 没有给出 UP 占比数字;
    //   客户端 GachaCharPoolContentTable 的角色条目也【没有】randomWeight 字段
    //   (武器池才有, 武器侧因此能精确算出 20/(20+6*10) = 25%)。
    //   这里沿用特许寻访的 50%, 依据是两池其余全部参数逐字段相同。
    //   ★ 待 2026/09/24 开池后, 用游戏内【干员寻访】的概率公示页核对; 若不是 50%,
    //     本表与下面 win_5050 的「理论 50%」标注都要跟着改。
    //
    // 【与特许寻访的机制差异 (来自 news/4776 官方原文), 对本表的影响】
    //   - 80 抽小保底: 「所有『重构寻访』共享此项保底机制…该保底计数将继承到其他
    //     『重构寻访』中」→ 跨期不清零 (Calculate 里 track_banner 对重构池取 false)。
    //   - 120 抽 UP 保底: 「前120次寻访必定能获取概率提升的6星干员, 该规则在同名重构寻访中
    //     【仅生效1次】。该计数将继承到后续的同名重构寻访中」→ 与特许寻访「每期独立重置」
    //     不同, 它是【每个同名系列一生只触发一次】。
    //     ⇒ 本表描述的是【该系列尚未用掉 120 兜底】时的分布 (即首次抽该系列)。
    //       系列兜底一旦用掉, 后续复刻期的理论分布退化为「无 120 硬保底」的长尾形态
    //       (形状接近 g_cdf_joint_up 而非本表)。截至目前「绚丽异彩」#1 是史上第一期
    //       重构寻访, 任何真实数据都只可能处于「兜底未用掉」状态, 故只建首次曲线;
    //       等 #2 复刻真正出现后再补第二条曲线。
    {
        constexpr int hard_cap = 120;
        constexpr int max_soft = 80;
        auto h_rf = [](int k) -> double {
            if (k <= 65)      return 0.008;
            else if (k <= 79) return 0.058 + (k - 66) * 0.05;
            else              return 1.0;
        };
        std::array<double, max_soft> D{}; D[0] = 1.0;
        double cum = 0.0;
        for (int n = 1; n <= hard_cap; ++n) {
            if (n == hard_cap) {
                double alive = 0.0;
                for (int s = 0; s < max_soft; ++s) alive += D[s];
                cum += alive;
                g_cdf_refactor_up[n] = (std::min)(1.0, cum);
                for (int k = n + 1; k <= hard_cap + 1; ++k) g_cdf_refactor_up[k] = 1.0;
                break;
            }

            std::array<double, max_soft> newD{};
            double p_hit_grad = 0.0;

            if (n == 30 || n == 60 || n == 90) {
                // ===== 赠送十连里程碑: 11 次独立判定 (本体抽 1 次 + 免费十连 10 次) =====
                // 免费十连不推进也不重置水位 (官方: 其结果不计入保底计数), 与 g_cdf_char_up
                // 在 n=30 的处理完全一致, 这里只是把同一段逻辑用在三个里程碑上。
                std::array<double, max_soft> stateA{};
                for (int s = 0; s < max_soft; ++s) {
                    if (D[s] == 0) continue;
                    double ph = h_rf(s + 1);
                    if (s + 1 < max_soft) stateA[s + 1] += D[s] * (1.0 - ph);
                    p_hit_grad += D[s] * ph * 0.5;   // 毕业 (出 UP)
                    stateA[0]  += D[s] * ph * 0.5;   // 歪, 水位归 0 (本体抽), 仍未出 UP
                }
                for (int free_step = 0; free_step < 10; ++free_step) {
                    std::array<double, max_soft> newStateA{};
                    for (int s = 0; s < max_soft; ++s) {
                        if (stateA[s] == 0) continue;
                        // 赠送十连走【基础概率】, 不吃软保底加成 —— 官方对加急招募的原文是
                        // 「加急招募的干员获取概率与本次寻访的基础概率一致」, 且其结果不计入
                        // 保底计数。故这里必须用 kBaseRate6 而不是 h_rf(s+1)。
                        //
                        // 为什么特许/辉光池没暴露这个问题: 它们只有 n=30 一个赠送节点, 那时
                        // 水位 s <= 30 < 66, h() 本来就等于基础概率, 两种写法数值相同。
                        // 重构池的第 3 个节点在 n=90, 存活水位可以到 66..79 的软保底段 ——
                        // 若沿用 h_rf, 免费单抽会被算成最高 30.8% 的出货率 (基础是 0.8%)。
                        const double ph = kBaseRate6;
                        newStateA[s] += stateA[s] * (1.0 - ph);   // 不出货, 水位停
                        p_hit_grad   += stateA[s] * ph * 0.5;     // 毕业 (出 UP)
                        newStateA[s] += stateA[s] * ph * 0.5;     // 歪, 水位停 (isFree)
                    }
                    stateA = newStateA;
                }
                newD = stateA;
            } else {
                for (int s = 0; s < max_soft; ++s) {
                    if (D[s] == 0) continue;
                    double ph = h_rf(s + 1);
                    if (s + 1 < max_soft) newD[s + 1] += D[s] * (1.0 - ph);
                    p_hit_grad += D[s] * ph * 0.5;
                    newD[0]    += D[s] * ph * 0.5;
                }
            }

            cum += p_hit_grad;
            g_cdf_refactor_up[n] = (std::min)(1.0, cum);
            D = newD;
        }
    }

    // ---- 武器池 UP 理论 CDF (g_cdf_wep_up[0..80]) ----
    // Reddit "First Featured Weapon Acquisition" Step 4 的 4×8 状态机:
    //   s = 1 - 0.99^10 ≈ 0.0956   (含至少 1 个 featured 的概率)
    //   u = 0.99^10 - 0.96^10 ≈ 0.2395  (无 featured 但有非 featured 6 星)
    //   v = 0.96^10 ≈ 0.6648       (无 6 星)
    //   s_pity = 1 - 0.75 × 0.99^9 ≈ 0.3149  (6 星 pity 拨中 featured 的条件概率)
    // CDF 展开成单抽索引: 只在 10 倍数边界跳变, 其它点平坦 (拨内不出货)
    {
        const double s = 1.0 - std::pow(0.99, 10);
        const double u = std::pow(0.99, 10) - std::pow(0.96, 10);
        const double v = std::pow(0.96, 10);
        const double s_pity = 1.0 - 0.75 * std::pow(0.99, 9);

        // state[ns][nf]
        double state[4][8] = {{0}};
        state[0][0] = 1.0;
        std::array<double, 8> finish_per_10pull{};

        for (int k = 0; k < 8; ++k) {
            double newState[4][8] = {{0}};
            double p_feat = 0.0;
            for (int ns = 0; ns < 4; ++ns) {
                for (int nf = 0; nf < 8; ++nf) {
                    double prob = state[ns][nf];
                    if (prob == 0) continue;
                    if (nf == 7) {
                        // featured pity: 必出 featured, 毕业
                        p_feat += prob;
                        continue;
                    }
                    if (ns == 3) {
                        // 6 星 pity 拨: featured 概率 s_pity, 非 featured 6 星 (1 - s_pity)
                        p_feat += prob * s_pity;
                        // 出非 featured 6 星: ns 重置, nf+1
                        newState[0][nf + 1] += prob * (1.0 - s_pity);
                    } else {
                        // 普通 10-pull
                        p_feat += prob * s;
                        newState[0][nf + 1]      += prob * u;
                        newState[ns + 1][nf + 1] += prob * v;
                    }
                }
            }
            finish_per_10pull[k] = p_feat;
            std::memcpy(state, newState, sizeof(state));
        }

        // 展开成单抽索引 (长度 81, 索引 0..80)
        double cum = 0.0;
        for (int k = 0; k < 8; ++k) {
            cum += finish_per_10pull[k];
            int pull_end = (k + 1) * 10;  // 第 k+1 拨结束 = 第 (k+1)*10 抽
            g_cdf_wep_up[pull_end] = (std::min)(1.0, cum);
        }
        // 非 10 倍数位置填上一个 10 倍数的值 (机制决定: 拨内不出货)
        for (int i = 1; i <= 80; ++i) {
            if (i % 10 != 0) g_cdf_wep_up[i] = g_cdf_wep_up[(i / 10) * 10];
        }
    }

    // ---- 辉光庆典 限定 (首个非常驻六星) 理论 CDF g_cdf_joint_up[0..240] ----
    //
    // 真实模型 (v0.1.2.2, 替换原 v0.1.2.1 的滚动卷积近似):
    //   Joint 与 Special 差别:
    //     - 每次出 6 星独立 50% 出限定 (4 个 6 星均匀: 2 限定 / 2 常驻)
    //     - 无大保底 (歪了下次不保证), 故状态只需 D[s] 单维
    //     - 无 UP 硬保底 (120 / 240 抽是赠送选择券, 不计入)
    //     - 6 星硬保底 80 抽 + 30 抽免费十连赠送 (与 Special 一致)
    //   状态 D[s]: 水位 s ∈ [0, 79]
    //   每抽:
    //     - 不出货: 概率 D[s] × (1-ph), 转 newD[s+1]
    //     - 出货: 50% 毕业 (限定) + 50% 水位归 0 (常驻)
    //   n=30: 展开 11 次判定 (本体抽水位推进, 免费十连水位停)
    //     免费十连出非限定时水位也停 (isFree 不重置)
    //   X 轴上限 240 (CDF 在此约 93%, 长尾延续, 实际不可能更长)
    //
    // 历史: v0.1.2.1 用滚动卷积法 (E[per cycle] × geom(0.5)), 准确但抽象;
    //        n=30 处也只有 g_cdf_char 自带的 hazard 跳跃, 经验 ECDF 在 X=30
    //        slot_up=30 跳跃时理论曲线对不上. 新模型把 11 次免费十连判定真实
    //        展开, 理论 CDF 在 X=30 处有可见跳跃 (~0.038), 与经验对齐.
    {
        constexpr int max_soft = 80;
        // v0.1.2.4: max_n 缩回 240 (与图表 X 轴 limit_base 一致, 数组 g_cdf_joint_up[242]).
        // CDF 在此处 ≈ 0.9308 (长尾 6.92% 未饱和), 但 computeTheoryMRL 通过本函数末尾
        // 计算的 g_joint_tail_mean_excess 长尾延伸常量补回完整 MRL 贡献.
        // 验证: 用 cdf[240] + 长尾点质量 (位置 240+84.37, 质量 0.0692) 算 MRL,
        //       与 simulate 到 n=2000 的精确值在 t=0..239 全程误差 < 1e-9 抽.
        constexpr int max_n    = 240;
        auto h_char = [](int k) -> double {
            if (k <= 65)      return 0.008;
            else if (k <= 79) return 0.058 + (k - 66) * 0.05;
            else              return 1.0;
        };
        std::array<double, max_soft> D{}; D[0] = 1.0;
        double cum = 0.0;
        for (int n = 1; n <= max_n; ++n) {
            std::array<double, max_soft> newD{};
            double p_hit_grad = 0.0;

            if (n == 30) {
                // 本体抽 (1 次, 推进水位)
                std::array<double, max_soft> stateA{};
                for (int s = 0; s < max_soft; ++s) {
                    if (D[s] == 0) continue;
                    double ph = h_char(s + 1);
                    if (s + 1 < max_soft) {
                        stateA[s + 1] += D[s] * (1.0 - ph);
                    }
                    p_hit_grad += D[s] * ph * 0.5;   // 毕业
                    stateA[0]  += D[s] * ph * 0.5;   // 非限定, 水位归 0 (本体抽)
                }
                // 免费十连 10 次 (水位停)
                for (int free_step = 0; free_step < 10; ++free_step) {
                    std::array<double, max_soft> newStateA{};
                    for (int s = 0; s < max_soft; ++s) {
                        if (stateA[s] == 0) continue;
                        const double ph = kBaseRate6;   // 赠送十连走基础概率, 不吃软保底加成
                        newStateA[s] += stateA[s] * (1.0 - ph);          // 不出货, 水位停
                        p_hit_grad   += stateA[s] * ph * 0.5;            // 毕业
                        newStateA[s] += stateA[s] * ph * 0.5;            // 非限定, 水位停 (isFree)
                    }
                    stateA = newStateA;
                }
                newD = stateA;
            } else {
                for (int s = 0; s < max_soft; ++s) {
                    if (D[s] == 0) continue;
                    double ph = h_char(s + 1);
                    if (s + 1 < max_soft) {
                        newD[s + 1] += D[s] * (1.0 - ph);
                    }
                    p_hit_grad += D[s] * ph * 0.5;   // 毕业
                    newD[0]    += D[s] * ph * 0.5;   // 非限定, 水位归 0
                }
            }

            cum += p_hit_grad;
            g_cdf_joint_up[n] = (std::min)(1.0, cum);
            D = newD;
        }
        // 注: v0.1.2.2 起不再设 cdf[max_n+1]=1.0 哨兵 (drawTheoryCDF 直接画到数组末端
        // 真实值 ~0.93). ComputeKS 内部对 x >= cdf_len 用 cdf[last_valid] fallback,
        // 不会因长尾未饱和而高估 K-S 偏差.

        // ---- 长尾解析延伸常量 g_joint_tail_mean_excess ----
        // 数学推导:
        //   设 F(n) = P(首限定 <= n) = g_cdf_joint_up[n], 截到 n = max_n = 240.
        //   1 - F(240) ≈ 0.0692 (长尾质量) 仍有 ~7% 概率分布在 n > 240.
        //   不补长尾的话: MRL[0] = Σ k·pdf[k] for k=1..240 ≈ 82.22 抽 (严重低估,
        //   真实 E[首限定] ≈ 104.68 抽). 误差源于截断丢失的 22.46 抽长尾贡献.
        //
        //   解决方案: 把"未截到的长尾"用一个点质量近似:
        //     - 质量 = 1 - F(240) (即 tail_mass)
        //     - 位置 = E[首限定抽数 | 首限定 > 240] (即 E_tail, 条件均值)
        //   则 computeTheoryMRL 公式追加项:
        //     num += (E_tail - t) × tail_mass   (绝对位置坐标)
        //   等价表示:
        //     num += (max_n + tail_mean_excess - t) × tail_mass
        //   其中 tail_mean_excess = E_tail - max_n 是相对 max_n 的条件超额量.
        //
        // 求 tail_mean_excess: 临时把 simulate 跑到 n = 2000 (此时 CDF 几乎饱和到
        // 1 - 1e-7), 在 n > max_n (=240) 的尾段上累加 Σ k·pdf[k] 与 Σ pdf[k] = tail_mass,
        // 再除以 tail_mass 得 E_tail (条件均值). 跑 2000 步 × 80 状态 = 160k 简单浮点
        // 操作, < 1ms.
        //
        // 注: 这里没有写死常量 (~84.37), 是为了与 g_cdf_joint_up 表保持机制一致:
        //     如果未来 hazard 函数 / 50% 限定率 / 30抽免费十连规则变了, 上面的 CDF
        //     表会自动跟着重算, 这里的 tail_mean_excess 也会同步重算. 避免"改了表
        //     忘了改常量"的悬空 bug.
        {
            constexpr int tail_sim_n = 2000;
            std::array<double, max_soft> D2{}; D2[0] = 1.0;
            double cdf_val = 0.0;
            double tail_sum_k_pdf = 0.0;   // Σ k·pdf[k] for k > max_n
            double tail_mass      = 0.0;   // Σ pdf[k]     for k > max_n
            for (int n = 1; n <= tail_sim_n; ++n) {
                std::array<double, max_soft> newD2{};
                double p_hit_grad = 0.0;

                if (n == 30) {
                    std::array<double, max_soft> stateA{};
                    for (int s = 0; s < max_soft; ++s) {
                        if (D2[s] == 0) continue;
                        double ph = h_char(s + 1);
                        if (s + 1 < max_soft) stateA[s + 1] += D2[s] * (1.0 - ph);
                        p_hit_grad  += D2[s] * ph * 0.5;
                        stateA[0]   += D2[s] * ph * 0.5;
                    }
                    for (int free_step = 0; free_step < 10; ++free_step) {
                        std::array<double, max_soft> newStateA{};
                        for (int s = 0; s < max_soft; ++s) {
                            if (stateA[s] == 0) continue;
                            const double ph = kBaseRate6;   // 同上: 赠送十连走基础概率
                            newStateA[s] += stateA[s] * (1.0 - ph);
                            p_hit_grad   += stateA[s] * ph * 0.5;
                            newStateA[s] += stateA[s] * ph * 0.5;
                        }
                        stateA = newStateA;
                    }
                    newD2 = stateA;
                } else {
                    for (int s = 0; s < max_soft; ++s) {
                        if (D2[s] == 0) continue;
                        double ph = h_char(s + 1);
                        if (s + 1 < max_soft) newD2[s + 1] += D2[s] * (1.0 - ph);
                        p_hit_grad += D2[s] * ph * 0.5;
                        newD2[0]   += D2[s] * ph * 0.5;
                    }
                }

                cdf_val += p_hit_grad;
                double pdf_n = p_hit_grad;
                if (n > max_n) {
                    tail_sum_k_pdf += (double)n * pdf_n;
                    tail_mass      += pdf_n;
                }
                D2 = newD2;
            }
            // 防御: 极端情况下 tail_mass=0 (例如机制改成 CDF 在 240 就饱和), 不延伸
            if (tail_mass > 1e-12) {
                double E_tail = tail_sum_k_pdf / tail_mass;
                g_joint_tail_mean_excess = E_tail - (double)max_n;
            } else {
                g_joint_tail_mean_excess = 0.0;
            }
            // 预期值: g_joint_tail_mean_excess ≈ 84.37 抽
            // 数值上 tail_mass ≈ 1 - g_cdf_joint_up[240] ≈ 0.0692 (作为 cross-check
            // 应该与 (1 - cdf_val 计算到 n=240 时的值) 相等. 由于浮点累加路径不同,
            // 误差在 1e-15 量级, 不显式校验)
        }
    }

    g_cdf_init = true;  // 末尾置位,确保所有读者看到完整表
}

// 修复:离散阶梯 CDF 的 K-S 统计量需严格对齐两条阶梯
// 在 x 处,两条阶梯的"底":F_n(cum before x),F_theory(x-1)
// 在 x 处,两条阶梯的"顶":F_n(cum after x),F_theory(x)
// 原版用 fn_before 减 cdf_table[x] —— 拿经验阶梯底对理论阶梯顶,
// 人为引入 h_x 的单点跳跃(软保底段可高达 5%+),造成巨大伪偏差
double ComputeKS(const std::array<int, 260>& freq, int max_pity, int n,
                 std::span<const double> cdf_table) {
    // v0.1.3.3: "裸指针 + 长度"两个散参 → std::span (C++20)。长度随表走,
    // 调用方不可能再把表和长度传错配对; 函数体保留局部 cdf_len, 下方逻辑零改动。
    const int cdf_len = (int)cdf_table.size();
    if (n == 0) return 0.0;
    // 防御性 clamp: freq 数组容量 260,max_pity 必须 < 260 否则越界读
    if (max_pity > 259) max_pity = 259;
    // v0.1.2.2: 找到 CDF 表的"有效末端" last_valid (饱和到 1 或单调性破坏前的最后一格).
    // 越过 last_valid 后, 用 cdf[last_valid] 而非 1.0 作 fallback —— 这对辉光池
    // (cdf 在 X=240 处 ≈ 0.93, X>240 时 CDF 仍未达 1) 很关键; 旧代码用 1.0 fallback
    // 会让长尾区域的 K-S 偏离凭空变大.
    constexpr double EPS_SAT = 1e-6;
    int last_valid = cdf_len - 1;
    for (int k = 1; k < cdf_len; ++k) {
        if (cdf_table[k] >= 1.0 - EPS_SAT) { last_valid = k; break; }
        if (k > 0 && cdf_table[k] + EPS_SAT < cdf_table[k - 1]) { last_valid = k - 1; break; }
    }
    auto lookup_cdf = [&](int idx) -> double {
        if (idx < 0) return 0.0;
        if (idx >= cdf_len) return cdf_table[last_valid];
        return cdf_table[idx];
    };
    double max_d = 0.0;
    int cum_count = 0;
    for (int x = 1; x <= max_pity; ++x) {
        double fn_before    = (double)cum_count / n;
        double cdf_before_x = lookup_cdf(x - 1);

        cum_count += freq[x];

        double fn_after    = (double)cum_count / n;
        double cdf_after_x = lookup_cdf(x);

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
//   - 角色池 (Special):每个 6 星独立 50/50, 无大保底(歪了下次【不】保证 UP); 唯一兜底是
//             120 抽硬保底(每期独立、不继承): 本期 119 抽未出 UP 则第 120 抽强制 UP。
//   - 武器池:每个六星独立判定 UP(条件概率 25%),无“歪→下次必中”。
//             唯一兜底是 80 抽(8 申领)限定硬保底:连续 7 次十连(70 抽)无 UP,第 8 次
//             十连(第 71~80 抽)强制出当期限定。40 小保底 + 80 硬保底每期独立重算、均不继承。
//   - 角色池 (Joint, 辉光庆典): 4 个 6 星都是限定角色, 其中 2 个同时也在常驻
//             名单 (本期: 艾尔黛拉/骏卫)。"UP" 定义 = 不在常驻名单中的 = 真·限定
//             (本期: 莱万汀/洁尔佩塔)。物理上 4 个均匀分布 → P(UP|6星) = 2/4 = 50%
//             与 Special 池的 50/50 歪率数值上重合,所以理论 CDF 直接复用
//             g_cdf_char; 限定(UP) CDF 用专建 g_cdf_joint_up (辉光池无 120 硬保底, 不复用
//             已加硬保底的 g_cdf_char_up; 机制与 Special 一致: 0.8% 基础,
//             k=66 起软保底, k=80 硬保底, 第 30 抽赠送十连)
// 因此 win_5050/lose_5050/avg_win 这组变量:
//   - Special / Joint:对应"小保底不歪率",统计意义明确
//   - 武器池:复用变量但含义改为"6 星中 UP 条件率"(win_5050=UP六星数,lose_5050=非UP六星数)
//             avg_win 对武器池无定义,保持 -1
//
// isJoint 参数 (v0.1.2.0 新增):
//   - true:  跳过 pool_map 查找, 直接走 standard_names 排除法判 UP
//            (辉光庆典池没有"当期 UP"概念, 所有非常驻 6 星都算 UP)
//   - false: 走原 pool_map 优先 → standard_names 兜底的路径
//   - 武器池 (isWeapon=true) 忽略 isJoint
//
// isRefactor 参数 (v0.1.4.0 新增, 重构寻访 RE-Factor):
//   - 池中六星 = 当期 UP + 5 名常驻 (无往期限定滞留), 所以 pool_map 与常驻排除法
//     两条路径都能正确判 UP; 与 Special 一样走 pool_map 优先。
//   - 80 抽小保底【所有重构寻访之间共享继承】→ 不按期重置 (track_banner = false)
//   - 120 抽 UP 保底【同名系列一生仅生效 1 次】且计数跨期继承 → got_up_banner 不重置,
//     天然等价于"整个系列只触发一次"
//   - 赠送十连有 3 处 (累计 30/60/90 抽), 而非特许寻访的 1 处
//   官方规则原文: https://endfield.hypergryph.com/news/4776
// -------------------------------------------------------
StatsResult Calculate(const PullBucket& bucket, bool isWeapon,
                     const std::unordered_set<std::string, StringHash, std::equal_to<>>& standard_names,
                     const std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>& pool_map,
                     bool isJoint = false, bool isRefactor = false) {
    StatsAccumulator acc;
    int current_pity = 0, pity_since_last_up = 0;
    // 卡池边界重置策略 (终末地三池各不同 —— 联网核实 + uigf 数据验证):
    //   - 特许池(Special): 仅 120 硬保底每期重置 (pity_since_last_up); 80 小保底【继承】(current_pity 不重置)
    //   - 武器池(Weapon):  40 小保底 + 80 硬保底【都】每期重置 (current_pity 与 pity_since_last_up 都重置, 均不继承)
    //   - 辉光庆典(Joint): 无硬保底, 连续累加, 不按期重置
    //   got_up_banner: 本期是否已出过 UP/限定 (硬保底每期仅生效一次), 每期重置
    //   hardpity_n:    硬保底强制阈值 —— 角色 120 抽; 武器 8 申领(= 第 71..80 抽强制出限定)
    // 三池均无“歪→下次必中”那种保底; 边界用 poolName 变化探测 (数据里每期 pool_name 唯一;
    //   武器 id 为负, 桶内按 |id| 升序 = 时间序, 每期连续).
    bool got_up_banner = false;
    // 重构寻访: 80 小保底跨所有重构池共享继承, 120 UP 保底按【同名系列】一生一次,
    //   两者都不按期重置 → 与 Joint 一样 track_banner = false;
    //   但它【有】120 硬保底 (Joint 没有), 所以 forced_by_hardpity 要单独放行, 见下。
    const bool track_special = (!isWeapon && !isJoint && !isRefactor);
    const bool track_weapon  = isWeapon;
    const bool track_banner  = (track_special || track_weapon);   // Joint / Refactor 不按期重置
    const int  hardpity_n    = isWeapon ? 71 : 120;

    // 赠送十连块计数 (v0.1.4.0): 重构寻访在累计 30/60/90 抽各送 1 次免费十连,
    //   需要把每个 isFree 块映射到对应的里程碑节点, 否则三个块会全部挤在节点 30,
    //   与理论 CDF 对不上。特许/辉光只有 1 处赠送十连, 恒为节点 30, 不受影响。
    //
    //   计法: 直接数【本桶内累计的 isFree 记录条数】, 第 n 条属于第 (n/10) 块 (0-based)。
    //   不能靠"非 isFree → isFree 的跳变"来分块 —— 官方允许把未使用的加急招募留到后面
    //   (「未使用的加急招募, 将保留到后续同名重构寻访中」), 玩家完全可能攒够 90 抽后
    //   连着开三次十连, 记录里就是连续 30 条 is_free=true, 跳变法只会数出 1 块。
    //
    //   已知局限: 抽卡记录只保留最近 90 天, 历史被截断时第一块可能只剩半截, 会让后续
    //   块序号整体偏移。无法从记录本身分辨, 故不做补偿 —— 影响仅限赠送出货落在哪个
    //   理论节点, 不影响出货计数与胜负统计。
    int free_pull_count = 0;

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

        // 本条若是赠送十连, 先算出它属于第几块 (1-based), 再累加计数
        int free_block_idx = 0;
        if (isFree) free_block_idx = (free_pull_count++ / 10) + 1;

        // 卡池边界探测: 读分桶阶段预计算的字节标记 (v0.1.3.2), 不再在热路径 memcmp 池名。
        //   starts_new_banner[i] = (本条 poolName 与上一条不同); 首条恒为 0 → 已含原 i>0 守卫。
        //   特许池: 120 硬保底不继承 → pity_since_last_up + got_up_banner 清零; 80 小保底继承 (current_pity 不动)
        //   武器池: 40 + 80 都不继承 → current_pity + pity_since_last_up + got_up_banner 全清零
        // 重构寻访 (v0.1.4.0): 官方给的两个作用域【不同】——
        //   80 抽六星保底: 「所有『重构寻访』共享此项保底机制…继承到其他『重构寻访』中」
        //     ⇒ current_pity 跨所有重构池连续累加, 永不按期清零。
        //   120 抽首个 UP 保底 / 累计奖励: 「该规则在【同名】重构寻访中仅生效 1 次,
        //     该计数将继承到后续的【同名】重构寻访中」
        //     ⇒ 只在【换到另一个系列】时才重置, 同名系列的 #1/#2/#3 之间继承。
        //   数据里同名系列的各期共用同一个 pool_name (与特许池"每期一个新池名"不同),
        //   所以 starts_new_banner (= pool_name 变化) 恰好就是"换系列"的判据。
        //   已知局限: 若将来两个重构系列【同时开放】, 记录按时间交错, pool_name 会来回
        //   跳变而产生误重置 —— 与武器池多期并行是同一类结构性问题, 待真实数据出现后
        //   改成按系列分桶再解决。
        if ((track_banner || isRefactor) && bucket.starts_new_banner[i]) {
            pity_since_last_up = 0;
            got_up_banner      = false;
            if (track_weapon) current_pity = 0;   // 武器 40 小保底也每期重算 (角色 80 小保底继承, 不清)
        }

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
        //   - 赠送十连出货 -> 归入对应里程碑节点 (特许/辉光恒为 30; 重构为 30/60/90)
        //   - 正常出货     -> 归入 freq[current_pity]
        //
        // 重构寻访的两套坐标系 (v0.1.4.0):
        //   free_node_up  用于 freq_up —— g_cdf_refactor_up 按【累计抽数】索引, 30/60/90
        //                  三个里程碑都能精确表达, 直接按块序号映射。
        //   free_node_all 用于 freq_all —— g_cdf_refactor 按【距上次六星的水位】索引,
        //                  而 80 抽硬保底保证水位 <= 80, 累计第 90 抽的赠送十连在该坐标系
        //                  里不可达, 故第 3 块及以后并入节点 60。这是与既有 g_cdf_char
        //                  同一类的已知近似 (赠送十连绑定累计抽数而非水位), 见 InitCDFTables。
        int free_node_all = 30, free_node_up = 30;
        if (isRefactor && free_block_idx >= 2) {
            free_node_all = 60;
            free_node_up  = (free_block_idx == 2) ? 60 : 90;
        }
        const int slot_all = isFree ? free_node_all : current_pity;
        if (slot_all < 260) acc.freq_all[slot_all]++;
        if (slot_all > acc.max_pity_all) acc.max_pity_all = slot_all;
        acc.count_all++;
        acc.sum_all    += slot_all;
        acc.sum_sq_all += (long long)slot_all * slot_all;

        bool isUP = false;
        if (isJoint) {
            // 辉光庆典: 不查 pool_map, 直接用常驻名单排除法
            // 4 个池中 6 星里, 在常驻名单中的视为"非 UP"(等价于 Special 池的"歪")
            isUP = !standard_names.contains(bucket.names[i]);
        } else {
            auto it = pool_map.find(bucket.poolNames[i]);
            if (it != pool_map.end()) isUP = (bucket.names[i] == it->second);
            else                      isUP = !standard_names.contains(bucket.names[i]);
        }

        if (isUP) {
            const int slot_up = isFree ? free_node_up : pity_since_last_up;
            if (slot_up < 260) acc.freq_up[slot_up]++;
            if (slot_up > acc.max_pity_up) acc.max_pity_up = slot_up;
            acc.count_up++;
            acc.sum_up    += slot_up;
            acc.sum_sq_up += (long long)slot_up * slot_up;

            // 胜负统计 (修正: 终末地无“歪→下次必中”, 每个六星/六星武器都是独立判定):
            //   - 角色池 50/50, 武器池 25% 条件率 —— 每个 UP/限定都计入“胜”, 唯一例外:
            //     由【硬保底强制】出的那个 (本期首个 UP/限定, 且当期累计抽数已打满硬保底阈值)
            //     不是掷硬币结果, 必须剔除, 否则把真实条件率系统性拉高 (角色>50%, 武器>25%)。
            //     角色 120 抽硬保底; 武器 8 申领(第 71..80 抽)硬保底, 见 hardpity_n。
            //   - 辉光庆典: 无硬保底, 每个限定直接计入。
            //   - avg_win (count_win/sum_win) 仅特许池有物理含义; 武器/Joint 不累计 (avg_win 保持 -1)。
            // 重构寻访虽然 track_banner=false (不按期重置), 但它【有】120 抽硬保底,
            // 只是作用域是"同名系列一生一次" —— got_up_banner 在重构池永不重置,
            // 恰好等价于该语义, 故这里把 isRefactor 一并放行。
            const bool forced_by_hardpity =
                (track_banner || isRefactor) && !got_up_banner && !isFree &&
                pity_since_last_up >= hardpity_n;
            if (isJoint) {
                acc.win_5050++;                 // 辉光庆典无硬保底, 每个限定都是掷硬币结果
            } else if (!forced_by_hardpity) {
                acc.win_5050++;
                if (!isWeapon) {            // avg_win 仅对特许池定义
                    acc.count_win++;
                    acc.sum_win += slot_all;
                }
            }
            // 只有【本体抽】出的 UP 才消耗硬保底额度。赠送十连是独立通道, 官方明确
            // 「加急招募所赠送的免费十连, 其抽取结果将不计入本次或其他寻访的保底计数」——
            // 免费十连里出了 UP, 本体的 120 抽硬保底依然成立。
            // (该问题在 main 上就存在, 不是重构池引入的: 旧写法无条件置 true, 会让免费出 UP
            //  之后那次真正由 120 硬保底强制出的 UP 被误算成一次随机"不歪", 抬高胜率。)
            if (!isFree) {
                got_up_banner      = true;
                pity_since_last_up = 0;   // 赠送十连出 UP 不重置水位 (独立通道)
            }
        } else {
            // 非 UP/非限定六星 = 一次独立判定的“负”。终末地可连续歪多次, 全部如实计入。
            acc.lose_5050++;
        }
        // 赠送十连出货不重置 current_pity (独立通道); 正常出货重置
        if (!isFree) current_pity = 0;
    }

    // 右删失:遍历结束时若仍有未结算的 pity,记录为删失样本
    // 这些抽数"存活"到了 current_pity 抽仍未出 6 星(或 UP)
    acc.censored_pity_all = current_pity;
    acc.censored_pity_up  = pity_since_last_up;

    // 防御性 clamp:即使数据异常导致 max_pity / censored_pity > 249,
    // 后续 ComputeKS 与 hazard 循环的索引访问也必须安全
    if (acc.max_pity_all > 259) acc.max_pity_all = 259;
    if (acc.max_pity_up  > 259) acc.max_pity_up  = 259;
    if (acc.censored_pity_all > 259) acc.censored_pity_all = 259;
    if (acc.censored_pity_up  > 259) acc.censored_pity_up  = 259;

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
        // 重构寻访另用 g_cdf_refactor —— 与 g_cdf_char 只差赠送十连节点 (30 → 30/60)
        const std::span<const double> cdf_tbl = isWeapon
            ? std::span<const double>(g_cdf_wep)          // 41
            : (isRefactor ? std::span<const double>(g_cdf_refactor)   // 82
                          : std::span<const double>(g_cdf_char));     // 82
        s.ks_d_all = ComputeKS(acc.freq_all, acc.max_pity_all, acc.count_all,
                               cdf_tbl);
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
        if (max_reach_all > 259) max_reach_all = 259;  // 防御性 clamp(已被上游保证,这里再防一道)
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

        // UP K-S 检验 (v0.1.1.1 起): 用 g_cdf_*_up
        // v0.1.2.2: 辉光庆典走 g_cdf_joint_up (真实前向迭代, n=30 处展开免费十连)
        // 注意 isJoint 时 freq_up 的"事件"语义是"距上次限定的抽数", 与 g_cdf_joint_up
        // 描述的"首次非常驻六星"分布一致 (因为 pity_since_last_up 在每次出限定后重置)。
        std::span<const double> cdf_up_tbl;              // v0.1.3.3: 长度由 span 自带
        if (isWeapon)         cdf_up_tbl = g_cdf_wep_up;      // 81
        else if (isJoint)     cdf_up_tbl = g_cdf_joint_up;    // 242
        else if (isRefactor)  cdf_up_tbl = g_cdf_refactor_up; // 122
        // g_cdf_refactor_up 描述的是【系列内第一个 UP】的分布 —— 它在 n=120 强制收敛到 1,
        // 依据是「前120次寻访必定获取 UP, 该规则在同名重构寻访中仅生效 1 次」。
        // 而 freq_up 记的是每两个 UP 之间的间隔: 第 2 个及以后的 UP 已经没有这个兜底,
        // 分布是无截断的长尾。两者不是同一个统计对象, 一旦样本里出现第 2 个 UP,
        // 整体就成了混合分布, 再拿这条曲线判"符合/偏离"就没有依据了。
        // 这不需要等到复刻才会发生 —— 首期追潜多抽一个 UP 就会遇到。
        // 故这里只标记, 由输出层把判定改成"样本混合"; D 值仍照常算出供参考。
        if (isRefactor && acc.count_up > 1) s.ks_up_mixed = true;
        else                  cdf_up_tbl = g_cdf_char_up;     // 122
        if (isWeapon) {
            // v0.1.3.3 武器 UP K-S: 先把经验 freq_up 按申领 (10 抽) 粒度向上聚合再比较。
            // 原因: g_cdf_wep_up 的质量只在 10 倍数边界记账 (申领内平坦, 机制如此),
            // 而经验 pity_up 记录的是申领内具体单抽落点 (自然出货 ~截断几何分布,
            // 40/80 保底强制出货的拨内落点游戏未公开)。两条阶梯粒度不同, 逐抽比较会被
            // "拨内错位"系统性抬高 D (落点均匀假设下渐近 ~0.37, 12 期样本伪拒绝率 ~63%)。
            // 聚合到申领边界后, 任何拨内落点都映射到同一申领, K-S 对落点假设免疫,
            // 伪拒绝率回到 <= 名义 5% (模拟: ~2%)。
            // 仅 K-S 内部用聚合副本; ECDF/MRL 图与 avg_up 仍为单抽粒度, 曲线连贯不变。
            std::array<int, 260> freq_up_claim{};
            for (int x = 1; x <= acc.max_pity_up; ++x) {
                if (acc.freq_up[x] == 0) continue;
                int slot = ((x + 9) / 10) * 10;   // 向上取整到申领末抽
                if (slot > 259) slot = 259;       // 防御 (正常数据 pity_up <= 80)
                freq_up_claim[slot] += acc.freq_up[x];
            }
            int max_claim = ((acc.max_pity_up + 9) / 10) * 10;
            if (max_claim > 259) max_claim = 259;
            s.ks_d_up = ComputeKS(freq_up_claim, max_claim, acc.count_up,
                                  cdf_up_tbl);
        } else {
            s.ks_d_up = ComputeKS(acc.freq_up, acc.max_pity_up, acc.count_up,
                                  cdf_up_tbl);
        }
        s.ks_is_normal_up = (s.ks_d_up <= (1.36 / std::sqrt((double)acc.count_up)));
    }

    // UP hazard 同理
    if (acc.count_up > 0 || acc.censored_pity_up > 0) {
        int survivors = acc.count_up + (acc.censored_pity_up > 0 ? 1 : 0);
        int max_reach_up = (std::max)(acc.max_pity_up, acc.censored_pity_up);
        if (max_reach_up > 259) max_reach_up = 259;  // 防御性 clamp
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
    // v0.1.3.3: GetWindowTextLengthW 对 RichEdit 文档明示"可能大于实际长度"(估计值)。
    // 旧写法忽略 GetWindowTextW 的实际拷贝数, 高估时 wstring 尾部残留 L'\0' 填充;
    // 这些 NUL 经 WideToUtf8 原样转换, 粘在名单最后一个 token 上 (TrimUtf8 只剥空白
    // 不剥 '\0'), 导致最后一个常驻名 / 最后一条 UP 映射永远匹配不上 JSON 里的名字,
    // 统计被静默污染。修复: 按实际拷贝数 resize。
    std::wstring buf((size_t)len + 1, L'\0');
    int copied = GetWindowTextW(hwnd, buf.data(), len + 1);
    buf.resize(copied > 0 ? (size_t)copied : 0);
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
//   - 线程句柄保留在 g_hWorker (主线程独占): 下次 Submit 开头与 WM_DESTROY 时
//     join + CloseHandle, 保证进程退出前 worker 已完整走完 CRT 尾声 (见 WM_DESTROY)
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
    size_t      fileSize = 0;   // v0.1.3.2: DWORD → size_t, 配合 GetFileSizeEx (不再 32 位截断)

    std::string utf8_chars;     // 来自 hCharEdit (这个无法 mmap,GUI 控件文本必须主线程 GetWindowText)
    std::string utf8_poolMap;
    std::string utf8_weps;

    // === worker 填充 ===
    bool        ok = false;
    StatsResult statsChar;
    StatsResult statsWep;
    StatsResult statsJoint;
    StatsResult statsRefactor;
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

// worker 线程句柄, 仅主线程读写。生命周期: Submit 创建 → (a) 下次 Submit 开头
// join+close (此刻上一个 worker 早已投递结果/自清理, 最多剩微秒级 CRT 尾声), 或
// (b) WM_DESTROY join+close (退出前兜底)。不在 Consume 里关闭: 投递结果 ≠ 线程已
// 退出, 提前关闭会失去 join 能力, 留下 "ExitProcess 终止恰好持有 CRT 堆锁的尾声
// 线程 → 退出挂死" 的风险窗口。
static HANDLE g_hWorker = NULL;

// Worker 线程入口:纯 CPU 工作,不碰任何 GUI。
// 用 _beginthreadex 启动 (见 ProcessFile_Submit), 故签名是 unsigned __stdcall(void*) ——
// 这样 worker 里用到的 CRT (std::string/unordered_map/swprintf/std::ranges::sort/异常等)
// 的 per-thread 状态能被正确初始化与清理; 裸 CreateThread 跑 CRT 在极端低内存下有终止
// 进程的风险 (见 ProcessFile_Submit 的说明)。
unsigned __stdcall ProcessFile_Worker(void* arg) {
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

    // PMR:2MB 单调缓冲池 (monotonic_buffer_resource)。v0.1.3.2 起【改放堆上】(此前在栈上)。
    //
    // 为什么从栈改到堆 (用 make_unique_for_overwrite, 而不是 std::vector<std::byte>(2MB)):
    //   - 修正 v0.1.3.1 的一处错误结论: 当时注释说"栈版只有真正写入的 ~600KB 才落到物理页",
    //     这不对。固定 2MB 栈帧 >= 1 页时 MSVC 序言会调 __chkstk 逐页探测【整块 2MB】以保证栈
    //     能安全扩展 —— 进入 worker 时这 2MB 就被全部触达/提交, 与 PMR 实际用多少无关。
    //   - 反而堆写法更省: make_unique_for_overwrite 不清零 (区别于 std::vector(2MB) / 带括号的
    //     new[]() / calloc 那种值初始化), 内存按需触页 —— 只有 PMR 真正写到的 ~600KB 才 fault-in
    //     成物理页, 不像栈版被 __chkstk 强行摸满 2MB。
    //   - 顺带: 不再需要给 worker 配 4MB 栈 (见 ProcessFile_Submit), 线程栈回默认即可,
    //     去掉了那处 STACK_SIZE_PARAM_IS_A_RESERVATION 特殊化。
    //   - (先前感到"堆版更卡"是因为当时用了会清零的写法; 本写法无清零, 不复现该开销。)
    // 关于缓存: 别再写"L1/L2 热 / TLB 不 miss"。2MB = 512 页, 远超 L1(~48KB) 和 L1 DTLB(~64 项);
    //   能保证的只是减少分配器调用 + 让 temps/bucket 集中在一段连续内存 (利于顺序访问的局部性)。
    //   真实命中率要用 PMU 实测。
    // 关于 fallback: pool 没显式指定 upstream, 默认 = get_default_resource() (= new_delete_resource)。
    //   故【不是】严格只用这 2MB: 超大导入耗尽后会 fallback 到堆而非崩溃 (有意为之, 比抛 bad_alloc
    //   退出更实用)。另注 monotonic_buffer_resource 不回收 vector 扩容前的旧块, 直到整个 pool 析构
    //   —— 一旦 reserve() 预估被大幅突破, arena 占用会比普通 allocator 涨得快。
    //
    // 生命周期: 声明顺序 arena → pool → alloc, 析构逆序 (alloc/pool 先, arena 后), 故 pool 引用
    //   的 arena 内存在 pool 存活期间始终有效; 各 pmr 容器声明在 alloc 之后, 会更早析构。
    constexpr size_t kArenaSize = 2 * 1024 * 1024;
    auto arena = std::make_unique_for_overwrite<std::byte[]>(kArenaSize);  // 堆, 不清零 (C++20)
    std::pmr::monotonic_buffer_resource pool(arena.get(), kArenaSize);
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

        // 角色路径: Special (特许寻访) / Joint (辉光庆典) / Refactor (重构寻访) 都进入
        // 角色统计流程, 但分别送入 bucketChar / bucketJoint / bucketRefac,
        // 在下方按 gt 分桶 (三套机制独立、保底互不继承)
        bool charPath = (it == ItemType::Character &&
                         (gt == GachaType::Special || gt == GachaType::Joint ||
                          gt == GachaType::Refactor));
        bool wepPath  = (it == ItemType::Weapon &&
                         gt != GachaType::Constant &&
                         gt != GachaType::Standard &&
                         gt != GachaType::Beginner);
        if (!charPath && !wepPath) return;

        // v0.1.4.0 幽灵记录防御:「寻访情报书」(kind = "gift_intel_book") 会混在
        //   /api/record/char 的 list 里返回 —— 它不是一次寻访, 没有 charId / charName,
        //   也没有 rarity。新版 main.cpp 已在导出侧滤掉, 但【旧版导出的 uigf_endfield.json
        //   里可能已经存了这类条目】, 那些记录的 rank_type 是空串 → RankType::Unknown。
        //   若照单全收, 它们会被当成"一次没出六星的抽卡"而把保底水位多推 1 抽
        //   (每 60 抽一本, 特许池尤其明显)。稀有度是每条真实抽卡记录必有的字段,
        //   所以这里用 rank_type 解析失败作为判据, 安全且不会误删真实记录。
        //   上游同类工具的判据是 kind != "gift_intel_book" (bhaoo/endfield-gacha #44 等)。
        if (rt == RankType::Unknown) return;

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
        // 防御分支: v0.1.3.3 起 WM_DESTROY 会先 join 本线程再销毁窗口, "关窗导致 HWND
        // 失效"已不会发生; PostMessageW 仍可能因极端情况失败 (如线程消息队列满 10000 条)。
        // 失败则没人消费 out → worker 自己清理, 否则泄漏 ProcessOutput + mmap 句柄, 且
        // g_processing 卡在 1。out 在 Submit 里已 release 给 worker, 此处归 worker 所有。
        if (!PostMessageW(out->hwnd_main, WM_APP_PROCESS_DONE, 0, (LPARAM)out)) {
            delete out;
            InterlockedExchange(&g_processing, 0);
        }
        return 0;
    }

    // 防御 LLONG_MIN: 对 v == LLONG_MIN 取 -v 是有符号溢出 (UB)。正常抽卡 id 不会是
    // LLONG_MIN, 但 id 来自外部文件, 用无符号求绝对值规避 UB (升序排序语义不变)。
    auto abs_ll = [](long long v) -> unsigned long long {
        return v < 0 ? (0ULL - static_cast<unsigned long long>(v))
                     : static_cast<unsigned long long>(v);
    };
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

    PullBucket bucketChar (alloc); bucketChar.reserve(4000);
    PullBucket bucketWep  (alloc); bucketWep .reserve(2000);
    PullBucket bucketJoint(alloc); bucketJoint.reserve(2000);
    PullBucket bucketRefac(alloc); bucketRefac.reserve(1000);
    for (const auto& t : temps) {
        if (t.it == ItemType::Character) {
            // 角色记录: 按 gacha_type 分桶, Special / Joint / Refactor 各走各的
            // (三者机制独立: 保底作用域、赠送十连次数、有无 120 硬保底都不同)
            if      (t.gt == GachaType::Special)  bucketChar .push_back(t.rt, t.name, t.poolName, t.isFree);
            else if (t.gt == GachaType::Joint)    bucketJoint.push_back(t.rt, t.name, t.poolName, t.isFree);
            else if (t.gt == GachaType::Refactor) bucketRefac.push_back(t.rt, t.name, t.poolName, t.isFree);
            // 其它角色池 (Standard/Beginner) 已在解析阶段 charPath 过滤,这里到不了
        } else {
            // 武器记录 (charPath=false → wepPath=true, 否则上面 return 了)
            // 注: 「重构申领」(poolId rerun_wpn_*) 的六星概率/40/80 保底与常规武库申领
            //   逐字段相同 (客户端 GachaWeaponPoolTypeTable type=0 与 type=1 完全一致),
            //   故直接并入武器桶。已知差异: 重构申领的第 8 次申领 UP 保底在【同名系列】
            //   之间继承且一生仅生效 1 次 (常规申领每期清零) —— 首期「点绘申领」是史上
            //   第一期重构申领, 还不存在可继承的历史, 故当前无影响; 待 #2 复刻时再拆分。
            //   官方原文: https://endfield.hypergryph.com/news/4776
            bucketWep.push_back(t.rt, t.name, t.poolName, t.isFree);
        }
    }

    out->statsChar     = Calculate(bucketChar,  /*isWeapon=*/false, stdChars, poolMap, /*isJoint=*/false);
    out->statsWep      = Calculate(bucketWep,   /*isWeapon=*/true,  stdWeps,  {},      /*isJoint=*/false);
    out->statsJoint    = Calculate(bucketJoint, /*isWeapon=*/false, stdChars, {},      /*isJoint=*/true);
    out->statsRefactor = Calculate(bucketRefac, /*isWeapon=*/false, stdChars, poolMap, /*isJoint=*/false,
                                   /*isRefactor=*/true);

    // 在 worker 渲染输出文本(swprintf 是 thread-safe;只有 SetWindowTextW 不是)
    wchar_t winCharStr[64] = L"[无数据]";
    if (out->statsChar.avg_win >= 0)
        swprintf(winCharStr, 64, L"%.2f 抽", out->statsChar.avg_win);

    // Joint 池没有 "不歪" 概念 (无大保底), avg_win 也保持 -1, 这里不需要 winJointStr
    // (text 模板里第三行直接写成静态说明文字, 不消耗 %ls 参数)

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
    wchar_t pendJointStr[96] = L"";
    if (out->statsJoint.censored_pity_all > 0 || out->statsJoint.censored_pity_up > 0) {
        swprintf(pendJointStr, 96, L"  [当前垫刀: 距上次六星 %d 抽 / 距上次限定 %d 抽]",
                 out->statsJoint.censored_pity_all, out->statsJoint.censored_pity_up);
    }
    wchar_t pendRefacStr[96] = L"";
    if (out->statsRefactor.censored_pity_all > 0 || out->statsRefactor.censored_pity_up > 0) {
        swprintf(pendRefacStr, 96, L"  [当前垫刀: 距上次六星 %d 抽 / 距上次 UP %d 抽]",
                 out->statsRefactor.censored_pity_all, out->statsRefactor.censored_pity_up);
    }
    wchar_t winRefacStr[64] = L"[无数据]";
    if (out->statsRefactor.avg_win >= 0)
        swprintf(winRefacStr, 64, L"%.2f 抽", out->statsRefactor.avg_win);

    auto ksLabel = [](const StatsResult& r) -> const wchar_t* {
        if (r.count_all == 0) return L"-";
        return r.ks_is_normal ? L"符合理论模型" : L"偏离过大";
    };
    auto ksUpLabel = [](const StatsResult& r) -> const wchar_t* {
        if (r.count_up == 0) return L"-";
        // 混合样本 (首个 UP 带 120 兜底 / 后续 UP 无兜底) 没有单一理论分布可比, 不作判定
        if (r.ks_up_mixed)   return L"样本混合, 不判定";
        return r.ks_is_normal_up ? L"符合理论模型" : L"偏离过大";
    };
    const wchar_t* ksCharLabel    = ksLabel  (out->statsChar);
    const wchar_t* ksWepLabel     = ksLabel  (out->statsWep);
    const wchar_t* ksJointLabel   = ksLabel  (out->statsJoint);
    const wchar_t* ksCharUpLabel  = ksUpLabel(out->statsChar);
    const wchar_t* ksWepUpLabel   = ksUpLabel(out->statsWep);
    const wchar_t* ksJointUpLabel = ksUpLabel(out->statsJoint);
    const wchar_t* ksRefacLabel   = ksLabel  (out->statsRefactor);
    const wchar_t* ksRefacUpLabel = ksUpLabel(out->statsRefactor);

    // 缓冲区 2880 → 4096 (v0.1.2.0 新增辉光庆典块) → 5632 (v0.1.4.0 新增重构寻访块)。
    // 每个完整池块约 +900 字符, 留出安全余量避免 swprintf 截断
    // (截断会让 SetWindowTextW 显示残缺尾巴)。
    wchar_t outMsg[5632];
    swprintf(outMsg, 5632,
        L"【角色卡池 (特许寻访)】 总计六星: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合六星 (含歪) 出货平均期望:     %5.2f 抽 (理论 ≈ 51.81)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 79.29)   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (理论 50%%) (%d胜%d负)\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\r\n\r\n"
        L"【角色卡池 (辉光庆典)】 总计六星: %d | 出限定 (非常驻): %d%ls\r\n"
        L" ▶ 综合六星 (含常驻) 出货平均期望:   %5.2f 抽 (理论 ≈ 51.81)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到任一限定 (非常驻) 的平均期望: %5.2f 抽 (理论 ≈ 104.68)  [95%% CI: %5.1f ~ %5.1f]    |   非常驻六星率: %5.1f%% (理论 50%%) (%d限定%d常驻)\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n\r\n"
        L"【角色卡池 (重构寻访)】 总计六星: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合六星 (含歪) 出货平均期望:     %5.2f 抽 (理论 ≈ 51.37)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 77.83)   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (理论 50%%*) (%d胜%d负)\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\t\t(* UP 占比官方未公布, 暂沿用特许寻访的 50%%, 待开池后核实)\r\n\r\n"
        L"【武器卡池 (武库申领)】 总计六星: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合六星出货平均期望:             %5.2f 抽 (理论 ≈ 19.17)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 54.74)   [95%% CI: %5.1f ~ %5.1f]    |   6 星中 UP 率: %5.1f%% (理论 25%%) (%d UP / %d 非UP)\t[K-S 检验偏离度 D值: %.3f (%ls)]",
        out->statsChar.count_all, out->statsChar.count_up, pendCharStr,
        out->statsChar.avg_all, (std::max)(1.0, out->statsChar.avg_all - out->statsChar.ci_all_err),
        out->statsChar.avg_all + out->statsChar.ci_all_err, out->statsChar.cv_all * 100.0,
        out->statsChar.ks_d_all, ksCharLabel,
        out->statsChar.avg_up, (std::max)(1.0, out->statsChar.avg_up - out->statsChar.ci_up_err),
        out->statsChar.avg_up + out->statsChar.ci_up_err,
        out->statsChar.win_rate_5050 >= 0 ? out->statsChar.win_rate_5050 * 100.0 : 0.0,
        out->statsChar.win_5050, out->statsChar.lose_5050,
        out->statsChar.ks_d_up, ksCharUpLabel,
        winCharStr,
        out->statsJoint.count_all, out->statsJoint.count_up, pendJointStr,
        out->statsJoint.avg_all, (std::max)(1.0, out->statsJoint.avg_all - out->statsJoint.ci_all_err),
        out->statsJoint.avg_all + out->statsJoint.ci_all_err, out->statsJoint.cv_all * 100.0,
        out->statsJoint.ks_d_all, ksJointLabel,
        out->statsJoint.avg_up, (std::max)(1.0, out->statsJoint.avg_up - out->statsJoint.ci_up_err),
        out->statsJoint.avg_up + out->statsJoint.ci_up_err,
        out->statsJoint.win_rate_5050 >= 0 ? out->statsJoint.win_rate_5050 * 100.0 : 0.0,
        out->statsJoint.win_5050, out->statsJoint.lose_5050,
        out->statsJoint.ks_d_up, ksJointUpLabel,
        out->statsRefactor.count_all, out->statsRefactor.count_up, pendRefacStr,
        out->statsRefactor.avg_all, (std::max)(1.0, out->statsRefactor.avg_all - out->statsRefactor.ci_all_err),
        out->statsRefactor.avg_all + out->statsRefactor.ci_all_err, out->statsRefactor.cv_all * 100.0,
        out->statsRefactor.ks_d_all, ksRefacLabel,
        out->statsRefactor.avg_up, (std::max)(1.0, out->statsRefactor.avg_up - out->statsRefactor.ci_up_err),
        out->statsRefactor.avg_up + out->statsRefactor.ci_up_err,
        out->statsRefactor.win_rate_5050 >= 0 ? out->statsRefactor.win_rate_5050 * 100.0 : 0.0,
        out->statsRefactor.win_5050, out->statsRefactor.lose_5050,
        out->statsRefactor.ks_d_up, ksRefacUpLabel,
        winRefacStr,
        out->statsWep.count_all, out->statsWep.count_up, pendWepStr,
        out->statsWep.avg_all, (std::max)(1.0, out->statsWep.avg_all - out->statsWep.ci_all_err),
        out->statsWep.avg_all + out->statsWep.ci_all_err, out->statsWep.cv_all * 100.0,
        out->statsWep.ks_d_all, ksWepLabel,
        out->statsWep.avg_up, (std::max)(1.0, out->statsWep.avg_up - out->statsWep.ci_up_err),
        out->statsWep.avg_up + out->statsWep.ci_up_err,
        out->statsWep.win_rate_5050 >= 0 ? out->statsWep.win_rate_5050 * 100.0 : 0.0,
        out->statsWep.win_5050, out->statsWep.lose_5050,
        out->statsWep.ks_d_up, ksWepUpLabel
    );
    out->outMsg = outMsg;
    out->ok = true;

    // 同上的防御分支 (消息队列满等极端失败)。窗口关闭路径已由 WM_DESTROY 收口:
    // 先 join 本线程 (彼时 hwnd 仍有效, 本行 PostMessageW 必然成功入队), 再 reap 队列里
    // 这条永远不会被派发的结果消息并释放载荷 —— 见 WndProc 的 WM_DESTROY。
    // (v0.1.3.3 修正: 旧注释称"由 ExitProcess 统一回收, 无实际危害, 不加 join" —— 结论
    //  不成立: ExitProcess 会直接终止其它线程, worker 若恰好在 CRT 堆锁内被终止, 退出
    //  流程可能挂死。现已改为退出前必 join, 该风险窗口不复存在。)
    if (!PostMessageW(out->hwnd_main, WM_APP_PROCESS_DONE, 0, (LPARAM)out)) {
        delete out;
        InterlockedExchange(&g_processing, 0);
    }
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
    // v0.1.3.2: GetFileSize (32 位, 截断 >4GB) → GetFileSizeEx (64 位)。抽卡文件正常远小于 4GB,
    // 但输入来自外部文件, 用 64 位读 + 显式上界校验更稳健 (尤其 32 位构建下 size_t 仅 4GB)。
    LARGE_INTEGER fileSize64{};
    if (!GetFileSizeEx(out->hFile, &fileSize64) ||
        fileSize64.QuadPart <= 0 ||
        static_cast<unsigned long long>(fileSize64.QuadPart) >
            static_cast<unsigned long long>(SIZE_MAX)) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    out->fileSize = static_cast<size_t>(fileSize64.QuadPart);
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

    // v0.1.3.3: 回收上一个 worker 的句柄 (若有)。能走到这里说明 CAS 已拿到锁, 即上一个
    // worker 已投递结果 (Consume 清零 g_processing) 或已自清理 —— 其线程最多只剩微秒级的
    // CRT 尾声, 此处 join 几乎不阻塞, 换来句柄零泄漏 + 任意时刻至多一个未决句柄。
    if (g_hWorker) {
        WaitForSingleObject(g_hWorker, INFINITE);
        CloseHandle(g_hWorker);
        g_hWorker = NULL;
    }

    // 启动 worker。v0.1.3.2: 2MB PMR arena 已挪到堆 (见 ProcessFile_Worker), worker 栈需求很小,
    // 故 stack_size 传 0 (用 EXE 默认栈大小), 不再需要 4MB + STACK_SIZE_PARAM_IS_A_RESERVATION。
    //
    // 仍用 _beginthreadex 而非裸 CreateThread: worker 大量使用 CRT (std::string / unordered_map /
    // swprintf / std::ranges::sort / 异常), 应走 CRT 线程入口以正确初始化/清理 per-thread 状态;
    // 裸 CreateThread 跑 CRT 在极端低内存下有终止进程的风险。
    uintptr_t raw = _beginthreadex(nullptr, 0,
                                   ProcessFile_Worker, out.get(),
                                   0, nullptr);
    if (raw == 0) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    // v0.1.3.3: 句柄不再立即 CloseHandle —— 保留在 g_hWorker 供 join (下次 Submit 开头 /
    // WM_DESTROY 兜底), 保证退出前 worker 完整走完, 杜绝 ExitProcess 截杀尾声线程。
    g_hWorker = reinterpret_cast<HANDLE>(raw);
    out.release();         // worker 接管所有权, 完成时主线程在 WM_APP_PROCESS_DONE 里 delete
    return true;
}

// 主线程消费 worker 结果. 必须在 WM_APP_PROCESS_DONE 里调用
void ProcessFile_Consume(HWND hwnd, ProcessOutput* out) {
    if (out->ok) {
        // 把结果搬到全局 (主线程独占,不需要锁)。
        // ★ 这里【每加一个池子就必须同步加一行】—— 图表绘制读的是这些全局量, 不是
        //   out-> 里的字段。v0.1.4.0 曾漏掉 statsRefactor, 导致文字统计有六星、
        //   重构寻访的两张图却始终显示"暂无出金数据"。
        statsChar     = out->statsChar;
        statsWep      = out->statsWep;
        statsJoint    = out->statsJoint;
        statsRefactor = out->statsRefactor;
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
// 图形渲染 —— 曲线坐标点用栈数组, 避免在绘图热路径里反复 new 小对象。
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
              const std::array<int, 260>& freq_all, const std::array<int, 260>& freq_up,
              int count_all, int count_up,
              [[maybe_unused]] int censored_all, [[maybe_unused]] int censored_up,
              std::span<const double> theory_cdf_all,
              std::span<const double> theory_cdf_up,
              const std::wstring& title, int limit_base,
              int ecdf_up_step_size = 1) {
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
    for (int i = 1; i < 260; i++) {
        if (freq_all[i] > 0 || freq_up[i] > 0) {
            if (i > max_x) max_x = i;
        }
    }
    // v0.1.2.1: 无出金时不再直接 return, 而是继续渲染坐标轴和理论 CDF (蓝/红虚线),
    // 让用户能看到"这个池子的理论分布长什么样"的参考曲线。原版直接 "暂无出金数据"
    // 一句话占满图框, 新池子用户体验差 (查不到任何信息).
    // 跳过的只是: 经验 ECDF 阶梯线 (drawEmpiricalECDF)、KS 偏离度标记。
    max_x = ((max_x / 10) + 1) * 10;
    // v0.1.3.3: 钳到 freq 数组合法上界 259 (容量 260, Calculate 守 slot<260)。
    // 事件落在 250..259 时上一行取整会把 max_x 推到 260, 下游 freq[k] (k<=max_x)
    // 越界读。辉光池限定间隔无硬保底, >=250 的间隔真实存在, 非纯理论场景。
    // (macOS/iOS 移植版两处早已带此钳制, 本次回移; MRL 同款见 DrawMRL。)
    if (max_x > 259) max_x = 259;

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

    // 画理论 CDF (虚线).
    //
    // 形态选择:
    //   stepSize == 1 (角色 / 综合武器): 折线连相邻整数点
    //     —— 角色每抽都是真实采样点, 折线是平缓上升的曲线, dash 平滑展开。
    //     —— 旧版 stepSize==1 强制阶梯 (水平+垂直) 在大量微小 90° 角点上让
    //        dash pattern 反复重启, 视觉糊成"蛆状"小钩 —— 改成纯折线根除。
    //   stepSize > 1  (武器 UP): 真阶梯, 水平 (stepSize-1) 抽 + 垂直跳跃
    //     —— 反映"10 抽一组判定"机制: 拨内 CDF 真的不变, 阶梯是机制必然
    //
    // 自动跳跃检测 (v0.1.1):
    // 在 stepSize=1 的折线模式下用状态机:
    //   - 折线模式: Δ_k / Δ_{k-1} > JUMP_THRESHOLD (=5) → 进入阶梯模式
    //   - 阶梯模式: Δ 持续上升 (Δ_k > Δ_{k-1}) → 保持阶梯; 否则退出折线
    // 这样能正确表达"软保底响应到峰值"这一持续陡升过程, 而不只是把
    // 触发跳跃的那一个点画成阶梯。例如角色 UP CDF 在 k=66 hazard 跳跃,
    // 但 CDF 增量峰值出现在 k=69 (因为 D[s] 迭代积分需要几抽反应):
    //   k=66 (Δ=0.018, 进入阶梯) → k=67 (0.031) → k=68 (0.040) → k=69 (0.045) →
    //   k=70 (0.045 ≤ 0.045 退出阶梯) → 后续平滑衰减
    // 自动覆盖以下场景, 不需要硬编码具体 k:
    //   - 角色综合 k=30 (单点跳跃: 30 抽合并 11 次判定)
    //   - 角色综合 k=66~69 (软保底响应)
    //   - 角色 UP   k=66~69 (软保底响应)
    //   - 角色 UP   k=120 (硬保底)
    // 武器综合 k=31 比值仅 2.86, 不触发, 保持平滑折线 (软保底渐进展开是真实形态)。
    //
    // 画法: 用 GraphicsPath 攒整条路径再一次性 stroke,
    //       dash pattern 沿连续路径走, 跨拐角不重启;
    //       LineJoin=Round 让拐角圆滑过渡, 缓解 dash 实部压在 90° 角的视觉错乱。
    auto drawTheoryCDF = [&](std::span<const double> cdf,
                             int stepSize, Gdiplus::Color color) {
        const int cdf_len = (int)cdf.size();   // v0.1.3.3: span 自带长度
        if (cdf_len < 2) return;
        Gdiplus::Pen pen(color, DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        pen.SetDashPattern(dash, 2);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        int upper = (cdf_len - 1 < max_x) ? cdf_len - 1 : max_x;
        if (upper < 1) return;
        // v0.1.2.2: 截掉两类"伪末端":
        //   1) 已饱和段: 找到第一个 cdf[k] >= 1-eps 的 k_sat, 之后所有 cdf 都等于 1.0
        //      (硬保底之后的延伸 + char_up 122 个槽里 cdf[120]=cdf[121]=1 的哨兵区);
        //      画到 k_sat 就停, 否则末端会冒出一段 1→1 水平虚线 (即"垂直阶梯顶端向右
        //      拐弯"的视觉 bug, 由 step mode 退出时画的水平退出线引起).
        //   2) 未填充哨兵段: 辉光庆典 cdf[241]=0 (没设哨兵), 画上去会从 0.93 跳到 0,
        //      产生倒挂. 检测到 cdf[k] < cdf[k-1] (单调性破坏) 也立即停.
        constexpr double EPS_SAT = 1e-6;
        int upper_eff = upper;
        for (int k = 1; k <= upper; ++k) {
            if (cdf[k] >= 1.0 - EPS_SAT) { upper_eff = k; break; }
            if (cdf[k] + EPS_SAT < cdf[k - 1]) { upper_eff = k - 1; break; }
        }
        if (upper_eff < 1) return;
        Gdiplus::GraphicsPath path;
        auto p0 = getPt(0, cdf[0]);
        Gdiplus::PointF prev = p0;
        if (stepSize == 1) {
            constexpr double JUMP_THRESHOLD = 5.0;
            constexpr double MIN_PREV_DELTA = 1e-6;
            bool inStepMode = false;
            for (int k = 1; k <= upper_eff; ++k) {
                double curDelta  = cdf[k] - cdf[k-1];
                double prevDelta = (k >= 2) ? cdf[k-1] - cdf[k-2] : 0.0;
                bool drawAsStep;
                if (inStepMode) {
                    // 阶梯模式: Δ 持续上升就保持, 否则退出
                    if (curDelta > prevDelta && prevDelta > MIN_PREV_DELTA) {
                        drawAsStep = true;
                    } else {
                        inStepMode = false;
                        drawAsStep = false;
                    }
                } else {
                    // 折线模式: 检测进入条件
                    if (prevDelta > MIN_PREV_DELTA
                        && curDelta / prevDelta > JUMP_THRESHOLD) {
                        inStepMode = true;
                        drawAsStep = true;
                    } else {
                        drawAsStep = false;
                    }
                }
                if (drawAsStep) {
                    auto pH = getPt(k, cdf[k - 1]);
                    auto pV = getPt(k, cdf[k]);
                    path.AddLine(prev, pH);
                    path.AddLine(pH, pV);
                    prev = pV;
                } else {
                    auto p = getPt(k, cdf[k]);
                    path.AddLine(prev, p);
                    prev = p;
                }
            }
        } else {
            // 武器: 阶梯, 水平段 + 垂直段
            int k = stepSize;
            while (k <= upper_eff) {
                auto pH = getPt(k, cdf[k - stepSize]);  // 水平到拐角
                auto pV = getPt(k, cdf[k]);              // 垂直跳跃
                path.AddLine(prev, pH);
                path.AddLine(pH, pV);
                prev = pV;
                k += stepSize;
            }
            // 末段补水平到 upper_eff (如果没走到)
            if (k - stepSize < upper_eff) {
                auto pEnd = getPt(upper_eff, cdf[k - stepSize]);
                path.AddLine(prev, pEnd);
            }
        }
        g.DrawPath(&pen, &path);
    };

    // 画经验 ECDF (实阶梯线).
    // 注: 删失观测(用户当前还在垫的 cur_pity)不画在 ECDF 上 ——
    // 因为它还没事件化, 强行画一个标记反而误导(会落在 ECDF 终点 y=100% 处)。
    // MRL 图已经精确显示"已垫 X 抽 / 预期还需 Y 抽", 这里不重复。
    auto drawEmpiricalECDF = [&](const std::array<int, 260>& freq, int total,
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

    drawTheoryCDF(theory_cdf_all, 1,                 Gdiplus::Color(180, 65, 140, 240));
    drawTheoryCDF(theory_cdf_up,  ecdf_up_step_size, Gdiplus::Color(180, 240, 80, 80));
    drawEmpiricalECDF(freq_all, count_all, Gdiplus::Color(255, 65, 140, 240));
    drawEmpiricalECDF(freq_up,  count_up,  Gdiplus::Color(255, 240, 80, 80));

    // KS 标记 (v0.1.1.1: 双色, 综合蓝色标签左上 / UP 红色标签右下)
    //
    // 标签布局策略:
    //   - 蓝色 (综合): 标签贴在 KS 虚线的左上方 (anchor 右下)
    //   - 红色 (UP):   标签贴在 KS 虚线的右下方 (anchor 左上)
    //   两个标签天然不会撞在一起, 颜色与对应 ECDF 实线一致, 用户能看出
    //   "蓝色 KS 标签 → 测的是综合 ECDF 的偏离"。
    //
    // 标签自带白色描边 (4 偏移方向先画白色底字再叠主文本), 在彩色实线上的可读性更好。
    enum class KSLabelAnchor { LeftTop, RightBottom };
    auto drawKSMarker = [&](const std::array<int, 260>& freq, int total,
                            std::span<const double> cdf,
                            BYTE r, BYTE gC, BYTE b,
                            KSLabelAnchor anchor) {
        const int cdf_len = (int)cdf.size();   // v0.1.3.3: span 自带长度
        if (total == 0 || cdf_len < 2) return;
        // v0.1.2.4: 同 drawTheoryCDF / computeTheoryMRL, 加 upper_eff 截断避免:
        //   - 辉光池 cdf[241]=0 (未填充哨兵段) 让 |cum - 0| ≈ 1, 误判为最大偏离点
        //   - 饱和段 (cdf[k]==1 after hard pity) 上做无意义的比较
        // 注: 此截断在 v0.1.2.2 已加到 drawTheoryCDF 和 computeTheoryMRL, 但当时漏掉
        // drawKSMarker, 直到 v0.1.2.4 才补齐.
        constexpr double EPS_SAT = 1e-6;
        int upper_scan = (cdf_len - 1 < max_x) ? cdf_len - 1 : max_x;
        int upper_eff = upper_scan;
        for (int k = 1; k <= upper_scan; ++k) {
            if (cdf[k] >= 1.0 - EPS_SAT) { upper_eff = k; break; }
            if (cdf[k] + EPS_SAT < cdf[k - 1]) { upper_eff = k - 1; break; }
        }
        if (upper_eff < 1) return;
        double max_d = 0; int max_d_x = 0;
        double cum = 0;
        for (int k = 1; k <= upper_eff; ++k) {
            cum += (double)freq[k] / (double)total;
            double d = std::fabs(cum - cdf[k]);
            if (d > max_d) { max_d = d; max_d_x = k; }
        }
        if (max_d <= 0.01 || max_d_x <= 0) return;
        double emp_y = 0;
        for (int k = 1; k <= max_d_x; ++k) emp_y += (double)freq[k] / (double)total;
        double th_y = cdf[max_d_x];
        auto p_emp = getPt(max_d_x, emp_y);
        auto p_th  = getPt(max_d_x, th_y);
        Gdiplus::Pen ksPen(Gdiplus::Color(255, r, gC, b), DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(2.0f), DPIScaleF(2.0f) };
        ksPen.SetDashPattern(dash, 2);
        g.DrawLine(&ksPen, p_emp.X, p_emp.Y, p_th.X, p_th.Y);

        wchar_t lbl[32];
        swprintf(lbl, 32, L"KS D=%.3f", max_d);
        float midY = (p_emp.Y + p_th.Y) * 0.5f;

        // 测量文字宽度,根据 anchor 计算左上角坐标
        Gdiplus::RectF box;
        g.MeasureString(lbl, -1, &tickFont, Gdiplus::PointF(0, 0), &box);

        float tx, ty;
        if (anchor == KSLabelAnchor::LeftTop) {
            // 蓝色: 标签贴虚线左上 (文字右下角对齐到虚线左上)
            tx = p_emp.X - DPIScaleF(4.0f) - box.Width;
            ty = midY - DPIScaleF(2.0f) - box.Height;
        } else {
            // 红色: 标签贴虚线右下 (文字左上角对齐到虚线右下)
            tx = p_emp.X + DPIScaleF(4.0f);
            ty = midY + DPIScaleF(2.0f);
        }

        // 白色描边
        Gdiplus::SolidBrush whiteBr(Gdiplus::Color(255, 252, 253, 255));
        for (int dx = -1; dx <= 1; dx += 2) {
            for (int dy = -1; dy <= 1; dy += 2) {
                g.DrawString(lbl, -1, &tickFont,
                             Gdiplus::PointF(tx + (float)dx, ty + (float)dy),
                             &whiteBr);
            }
        }
        // 主文本 (与对应 ECDF 实线同色)
        Gdiplus::SolidBrush mainBr(Gdiplus::Color(255, r, gC, b));
        g.DrawString(lbl, -1, &tickFont, Gdiplus::PointF(tx, ty), &mainBr);
    };
    // 蓝色 (综合): 左上
    drawKSMarker(freq_all, count_all, theory_cdf_all,
                 65, 140, 240, KSLabelAnchor::LeftTop);
    // 红色 (UP): 右下
    drawKSMarker(freq_up, count_up, theory_cdf_up,
                 240, 80, 80, KSLabelAnchor::RightBottom);

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

    // 无出金时, 在绘图区中央叠加灰色提示 (v0.1.2.1)
    // 理论 CDF 已经画了, 此提示仅说明"经验数据为空", 不阻塞其它内容显示
    if (!hasData) {
        Gdiplus::Font hintFont(&fontFamily, DPIScaleF(13.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush hintBrush(Gdiplus::Color(200, 130, 130, 130));
        const wchar_t* hint = L"暂无出金数据 (仅显示理论曲线参考)";
        Gdiplus::RectF box;
        g.MeasureString(hint, -1, &hintFont, Gdiplus::PointF(0, 0), &box);
        g.DrawString(hint, -1, &hintFont,
                     Gdiplus::PointF(plotX + plotW * 0.5f - box.Width * 0.5f,
                                     plotY + plotH * 0.5f - box.Height * 0.5f),
                     &hintBrush);
    }
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
             const std::array<int, 260>& freq_all,
             const std::array<int, 260>& freq_up,
             int count_all, int count_up,
             int censored_all, int censored_up,
             std::span<const double> theory_cdf_all,
             std::span<const double> theory_cdf_up,
             const std::wstring& title, int limit_base,
             int theory_all_cap = 0, int theory_up_cap = 0,
             double tail_mean_excess_up = 0.0) {
    // tail_mean_excess_up (v0.1.2.4): 仅辉光池非 0. 含义见 InitCDFTables 注释.
    //   对 UP 理论 MRL, 在 upper_eff 之后追加一个点质量:
    //     位置 = upper_eff + tail_mean_excess_up
    //     质量 = 1 - cdf[upper_eff]
    //   这样 MRL[t] 公式: num += (位置 - t) × 质量
    //   让辉光池 MRL[0] 从无延伸的 ~82 修正回完整真值 ~104.68.
    //   其他池 (特许/武器) 的 UP CDF 在 X 轴末端已饱和 (>1 - 1e-6), upper_eff 截在
    //   饱和点, 此时 (1 - cdf[upper_eff]) ≈ 0, 即使传 0 也无影响.
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
    for (int i = 1; i < 260; i++) {
        if (freq_all[i] > 0 || freq_up[i] > 0) if (i > max_x) max_x = i;
    }
    // v0.1.2.1: 无出金时不再直接 return, 继续渲染理论 MRL 虚线作参考。
    // 经验 MRL 自然为空 (computeEmpiricalMRL 内部已防御 total==0), KS / "你在这里" 标记
    // 也都依赖出金数据, 缺失时跳过。提示在最后叠加灰色字。
    max_x = ((max_x / 10) + 1) * 10;
    // v0.1.3.3: 同 DrawECDF 的 259 钳制 —— 此处更严重: computeEmpiricalMRL 内
    // surv[t]/mrl[t] 在 t=260 是【栈数组越界写】(ASan 实测 stack-buffer-overflow),
    // 末尾 max_y 循环与 mrl_*[t] 读同样越界。触发条件: 任一 freq 事件落在 [250,259]。
    if (max_x > 259) max_x = 259;

    // ---- 计算经验 MRL 序列 (并记录每个 t 处的 surviving 计数) ----
    auto computeEmpiricalMRL = [&](const std::array<int, 260>& freq, int total)
        -> std::pair<std::array<double, 260>, std::array<int, 260>> {
        std::array<double, 260> mrl{}; mrl.fill(-1.0);  // -1 = undefined
        std::array<int, 260> surv{}; surv.fill(0);
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

    // ---- 计算理论 MRL (基于理论 CDF, 可选长尾解析延伸) ----
    auto computeTheoryMRL = [&](std::span<const double> cdf, double tail_mean_excess = 0.0) {
        std::array<double, 260> tmrl{}; tmrl.fill(-1.0);
        const int cdf_len = (int)cdf.size();   // v0.1.3.3: span 自带长度
        if (cdf_len < 2) return tmrl;
        int upper = cdf_len - 1;  // CDF 最大有效索引
        // v0.1.2.2: 与 drawTheoryCDF 同样的 upper_eff 截断逻辑, 避免:
        //   1) 饱和段 (cdf[k]==1 after hard pity): 不必再算
        //   2) 未填充末端 (辉光池 cdf[241]=0 删哨兵后): 算 pdf[k]=cdf[k]-cdf[k-1] 会出负值
        constexpr double EPS_SAT = 1e-6;
        int upper_eff = upper;
        for (int k = 1; k <= upper; ++k) {
            if (cdf[k] >= 1.0 - EPS_SAT) { upper_eff = k; break; }
            if (cdf[k] + EPS_SAT < cdf[k - 1]) { upper_eff = k - 1; break; }
        }
        if (upper_eff < 1) return tmrl;
        // 长尾点质量参数 (v0.1.2.4):
        //   tail_mass     = 1 - cdf[upper_eff]
        //   tail_position = upper_eff + tail_mean_excess
        //   仅当 tail_mean_excess > 0 且 tail_mass > 1e-9 时才追加;
        //   特许/武器池在 upper_eff 已饱和到 ~1.0, 即使传 tail_mean_excess>0 也几乎无贡献.
        double tail_mass     = 1.0 - cdf[upper_eff];
        double tail_position = (double)upper_eff + tail_mean_excess;
        bool   has_tail      = (tail_mean_excess > 0.0) && (tail_mass > 1e-9);
        // 从 PDF: pdf[k] = cdf[k] - cdf[k-1], for k=1..upper_eff
        // MRL(t) = [Σ_{k>t} (k-t) · pdf[k] + (tail_position-t) · tail_mass] / (1 - cdf[t])
        for (int t = 0; t <= upper_eff - 1 && t <= max_x; ++t) {
            double surv_t = 1.0 - cdf[t];
            if (surv_t < 1e-9) break;
            double num = 0.0;
            for (int k = t + 1; k <= upper_eff; ++k) {
                double pdf_k = cdf[k] - cdf[k-1];
                num += (double)(k - t) * pdf_k;
            }
            if (has_tail) {
                num += (tail_position - (double)t) * tail_mass;
            }
            tmrl[t] = num / surv_t;
        }
        return tmrl;
    };
    auto theory_mrl_all = computeTheoryMRL(theory_cdf_all);
    auto theory_mrl_up  = computeTheoryMRL(theory_cdf_up, tail_mean_excess_up);

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
    // LineJoin=Round 让拐角圆滑 (武器 UP MRL 是锯齿状, 每 10 抽内斜率 -1 ,
    // 拐角处线段方向变化, Round 缓解 dash 实部压在拐角的视觉错乱)。
    auto drawTheoryMRL = [&](const std::array<double, 260>& tmrl, int cap, Gdiplus::Color color) {
        Gdiplus::Pen pen(color, DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        pen.SetDashPattern(dash, 2);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
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

    // ---- 画经验 MRL ----
    //
    // 视觉编码 (v0.1.1):
    //   surv >= 2: 满色实线 2.5pt   ← 多个独立样本支撑, 统计可靠
    //   surv == 1: 半透明同色实线 1.8pt (alpha=115/255 ≈ 0.45)  ← 高方差区
    //
    // 历史: 之前 surv==1 段画虚线 (dash 4/3), 但红色 UP 理论 MRL 也是 dash 4/3,
    //       两者撞色撞样式无法分辨。改成半透明实线后, 视觉编码错开:
    //         "颜色淡 = 数据稀薄"   "虚线 = 理论参考"
    //       两个语义彻底分开, 用户一眼能看出哪条是经验数据尾巴、哪条是理论曲线。
    //
    // 实现: 实线段和半透明段分别攒到两个 GraphicsPath, 各自一次性 stroke。
    //       LineJoin=Round 让拐角圆滑过渡。
    auto drawEmpiricalMRL = [&](const std::pair<std::array<double, 260>, std::array<int, 260>>& mrl_data,
                                 BYTE r, BYTE gC, BYTE b) {
        const auto& mrl = mrl_data.first;
        const auto& surv = mrl_data.second;
        Gdiplus::Pen thickPen(Gdiplus::Color(255, r, gC, b), DPIScaleF(2.5f));
        thickPen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::Pen thinPen (Gdiplus::Color(115, r, gC, b), DPIScaleF(1.8f));
        thinPen.SetLineJoin(Gdiplus::LineJoinRound);

        // 累积:满色段进 thickPath, 半透明段进 thinPath (每段都 StartFigure 隔开)
        Gdiplus::GraphicsPath thickPath, thinPath;
        Gdiplus::PointF prev; bool has_prev = false; bool prev_thick = true;
        for (int t = 0; t <= max_x; ++t) {
            if (mrl[t] < 0 || surv[t] == 0) {
                if (has_prev) {
                    thickPath.StartFigure();
                    thinPath.StartFigure();
                }
                has_prev = false; continue;
            }
            auto p = getPt(t, mrl[t]);
            bool thick = (surv[t] >= 2);
            if (has_prev) {
                if (thick && prev_thick) thickPath.AddLine(prev, p);
                else                     thinPath.AddLine(prev, p);
            }
            prev = p; has_prev = true; prev_thick = thick;
        }
        g.DrawPath(&thickPen, &thickPath);
        g.DrawPath(&thinPen,  &thinPath);
    };
    drawEmpiricalMRL(mrl_all, 65, 140, 240);
    drawEmpiricalMRL(mrl_up,  240, 80, 80);

    // ---- "你在这里" 竖线 (当前 censored_pity 位置) ----
    // 关键设计:
    //   - 综合 (蓝): 优先用综合理论 MRL, 否则降级到经验 MRL
    //   - UP (红):   v0.1.1 起也优先用 UP 理论 MRL (新增 g_cdf_*_up 后),
    //                否则降级到经验 MRL。有了精确的 UP 理论曲线后, 即使本次抽卡
    //                数据稀疏 / 全在同一 censored 区段内, 标注线也能给出可靠参考。
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
                                   const std::pair<std::array<double, 260>, std::array<int, 260>>& mrl_data,
                                   const std::array<double, 260>& tmrl,
                                   int theory_cap,
                                   BYTE r, BYTE gC, BYTE b) {
        if (censored <= 0 || censored > max_x) return;
        double y_value = -1.0;
        // 1. 优先 theory MRL (综合和 UP 都有)
        if (censored < (int)tmrl.size() && tmrl[censored] > 0
            && (theory_cap == 0 || censored <= theory_cap)) {
            y_value = tmrl[censored];
        }
        // 2. 降级经验 MRL
        if (y_value <= 0 && mrl_data.first[censored] > 0) {
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
    resolveAndDrawLine(censored_all, mrl_all, theory_mrl_all, theory_all_cap, 65, 140, 240);
    resolveAndDrawLine(censored_up,  mrl_up,  theory_mrl_up,  theory_up_cap,  240, 80, 80);

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

    // 无出金时, 在绘图区中央叠加灰色提示 (v0.1.2.1)
    if (!hasData) {
        Gdiplus::Font hintFont(&fontFamily, DPIScaleF(13.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush hintBrush(Gdiplus::Color(200, 130, 130, 130));
        const wchar_t* hint = L"暂无出金数据 (仅显示理论曲线参考)";
        Gdiplus::RectF box;
        g.MeasureString(hint, -1, &hintFont, Gdiplus::PointF(0, 0), &box);
        g.DrawString(hint, -1, &hintFont,
                     Gdiplus::PointF(plotX + plotW * 0.5f - box.Width * 0.5f,
                                     plotY + plotH * 0.5f - box.Height * 0.5f),
                     &hintBrush);
    }
}

// ---------------------------------------------------------
// [响应式布局 + 垂直滚动]  v0.1.4.0
//
// 参考 Apple 版 (shangyuan0818/Endfield-Gacha-Apple) macOS ContentView.swift 的做法:
//   整个主区域套一层 ScrollView; 图表宽度撑满父容器 (maxWidth: .infinity),
//   高度给 minHeight 保底, 窗口够高时几行图表平分剩余空间。
//   原注释:「小屏 MacBook 缩小窗口时武器卡池/底部图表会被 Dock 或屏幕底端遮挡,
//            这里让用户能滚动查看」——— Windows 端在加入第 4 行图表后是同样的问题。
//
// Win32 等价实现:
//   - 窗口恢复可缩放 (WS_THICKFRAME | WS_MAXIMIZEBOX), 并加 WS_VSCROLL
//   - 所有子控件与图表的坐标改由 ComputeLayout() 从客户区尺寸算出, 不再写死常量
//   - 内容总高 > 客户区高时可滚动。滚动【不重绘图表】: 图表位图按"内容坐标系"缓存
//     (高度 = contentH 而非客户区高), 滚动只改 BitBlt 的源点 Y 与子控件的 Y 偏移。
//   - 滚动条用 SIF_DISABLENOSCROLL 常驻显示 (内容装得下时变灰而非隐藏)。这样客户区
//     宽度不会因滚动条显隐而跳变, 避免 SetScrollInfo → WM_SIZE → SetScrollInfo 的
//     重入抖动。
//
// 坐标约定: 下面 ui:: 里的 k* 常量是【逻辑像素】, Layout 结构体里的字段是
//           【物理像素】(已过 DPIScale)。
// ---------------------------------------------------------
namespace ui {
    constexpr int kMarginX      = 20;   // 左右留白
    constexpr int kMarginBottom = 20;
    constexpr int kLabelW       = 95;
    constexpr int kEditX        = 120;  // 标签右侧, 输入框起点
    constexpr int kHintY        = 15;
    constexpr int kRowY0        = 40;   // 第一行输入框 (常驻六星角色)
    constexpr int kRowStep      = 30;   // 行距
    constexpr int kLabelDY      = 5;    // 标签相对同行输入框的垂直微调
    constexpr int kEditH        = 26;
    constexpr int kLabelH       = 20;
    constexpr int kOutY         = 135;  // 输出文本框顶端
    constexpr int kOutH         = 280;  // 输出文本框高度 (4 个池块)
    constexpr int kChartGapX    = 20;   // 同一行左右两图之间
    constexpr int kChartGapY    = 5;    // 上下两行之间
    constexpr int kChartMinRowH = 250;  // 单行图表最小高度 (等价 Apple 的 minHeight)
    constexpr int kChartRows    = 4;    // 特许 / 辉光 / 重构 / 武器
    constexpr int kMinWindowW   = 900;  // 再窄图表就没法读了
    constexpr int kMinWindowH   = 360;
    constexpr int kScrollLine   = 30;   // 一次 SB_LINEUP/DOWN 的步长
}

struct Layout {
    int clientW = 0, clientH = 0;   // 客户区尺寸 (物理像素)
    int contentH = 0;               // 内容总高, 滚动范围的依据
    int editX = 0, editW = 0;       // 三个配置输入框
    int outY = 0, outH = 0, outW = 0;
    int chartX1 = 0, chartX2 = 0;   // 左列 / 右列 X
    int chartTop = 0, chartW = 0, chartRowH = 0;
    // 第 i 行 (0-based) 图表的顶端 Y, 内容坐标系
    int RowY(int i) const { return chartTop + i * (chartRowH + DPIScale(ui::kChartGapY)); }
};

Layout ComputeLayout(HWND hwnd) {
    Layout L;
    RECT rc; GetClientRect(hwnd, &rc);
    L.clientW = (int)rc.right; L.clientH = (int)rc.bottom;   // RECT 成员是 LONG, 显式收敛
    if (L.clientW <= 0 || L.clientH <= 0) return L;

    const int mx = DPIScale(ui::kMarginX);
    L.editX = DPIScale(ui::kEditX);
    // clamp 到一个正的最小值: 窗口被拖到极窄时 (WM_GETMINMAXINFO 之外仍可能发生,
    // 例如最小化/还原过程中的瞬时尺寸) 避免出现负宽度传给 MoveWindow/位图。
    L.editW = (std::max)(DPIScale(40), L.clientW - L.editX - mx);
    L.outW  = (std::max)(DPIScale(40), L.clientW - 2 * mx);
    L.outY  = DPIScale(ui::kOutY);
    L.outH  = DPIScale(ui::kOutH);

    L.chartTop = L.outY + L.outH + DPIScale(ui::kChartGapY);
    L.chartW   = (std::max)(DPIScale(80),
                            (L.clientW - 2 * mx - DPIScale(ui::kChartGapX)) / 2);
    L.chartX1  = mx;
    L.chartX2  = mx + L.chartW + DPIScale(ui::kChartGapX);

    // 行高: 客户区给 4 行图表剩多少。够高就平分 (等价 Apple 那边"填满容器"的行为),
    // 不够就退回最小行高, 内容溢出客户区 → 由滚动条兜住。
    const int gapsY = (ui::kChartRows - 1) * DPIScale(ui::kChartGapY);
    const int avail = L.clientH - L.chartTop - DPIScale(ui::kMarginBottom) - gapsY;
    L.chartRowH = (std::max)(DPIScale(ui::kChartMinRowH), avail / ui::kChartRows);

    L.contentH = L.chartTop + ui::kChartRows * L.chartRowH + gapsY + DPIScale(ui::kMarginBottom);
    // 平分时的整除误差会让 contentH 比 clientH 少几像素, 那样 WM_PAINT 会 BitBlt 到
    // 位图外边; 这里抬平到至少等于客户区高。
    if (L.contentH < L.clientH) L.contentH = L.clientH;
    return L;
}

// 按当前 g_scrollY 摆放全部子控件。子控件跟随内容一起滚动 (与 Apple 版把配置行也放进
// ScrollView 的行为一致), 所以统一加 -g_scrollY 的 Y 偏移。
void LayoutChildren(HWND hwnd, const Layout& L) {
    (void)hwnd;                          // 摆位只用子窗口句柄, 父句柄留着保持接口对称
    if (!hCharEdit) return;              // WM_CREATE 还没建完控件
    const int mx = DPIScale(ui::kMarginX);
    const int dy = -g_scrollY;

    // 先把 8 个控件的目标位置算齐, 再统一提交。分两步是为了让 DeferWindowPos 中途失败时
    // 能【整体】退回 MoveWindow —— 边算边 Defer 的话, 一旦失败, 之前已入队的那几个控件
    // 会随着 HDWP 作废而停在原位, 出现"一半控件跟着滚、一半不动"的错位。
    struct Place { HWND h; int x, y, w, hgt; };
    Place places[8];
    int n = 0;
    auto add = [&](HWND h, int x, int y, int w, int hgt) {
        if (h && n < 8) places[n++] = Place{h, x, y + dy, w, hgt};
    };
    add(hHintLabel, mx, DPIScale(ui::kHintY),
        (std::max)(DPIScale(100), L.clientW - 2 * mx), DPIScale(ui::kLabelH));
    for (int i = 0; i < 3; ++i) {
        const int rowY = DPIScale(ui::kRowY0 + i * ui::kRowStep);
        HWND lbl  = (i == 0) ? hCharLabel : (i == 1) ? hPoolMapLabel : hWepLabel;
        HWND edit = (i == 0) ? hCharEdit  : (i == 1) ? hPoolMapEdit  : hWepEdit;
        add(lbl,  mx,      rowY + DPIScale(ui::kLabelDY), DPIScale(ui::kLabelW), DPIScale(ui::kLabelH));
        add(edit, L.editX, rowY,                          L.editW,               DPIScale(ui::kEditH));
    }
    add(hOutEdit, mx, L.outY, L.outW, L.outH);

    // 批量摆位, 减少缩放/滚动时的闪烁
    HDWP hdwp = BeginDeferWindowPos(n);
    for (int i = 0; i < n && hdwp; ++i) {
        hdwp = DeferWindowPos(hdwp, places[i].h, NULL,
                              places[i].x, places[i].y, places[i].w, places[i].hgt,
                              SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (hdwp) { EndDeferWindowPos(hdwp); return; }
    for (int i = 0; i < n; ++i)          // 兜底: 批量提交失败时逐个摆
        MoveWindow(places[i].h, places[i].x, places[i].y, places[i].w, places[i].hgt, TRUE);
}

void UpdateScrollInfo(HWND hwnd, const Layout& L) {
    g_contentH = L.contentH;
    SCROLLINFO si = { sizeof(si) };
    // SIF_DISABLENOSCROLL: 内容装得下时把滚动条置灰而不是隐藏, 客户区宽度因此恒定,
    // 不会触发 "设滚动条 → 客户区变窄 → WM_SIZE → 再设滚动条" 的重入。
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin  = 0;
    si.nMax  = L.contentH - 1;
    si.nPage = (UINT)L.clientH;
    si.nPos  = g_scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    // 系统会按 range/page 夹紧 nPos, 读回来才是真实生效值
    si.fMask = SIF_POS;
    if (GetScrollInfo(hwnd, SB_VERT, &si)) g_scrollY = si.nPos;
}

// 滚动到指定位置。图表缓存按内容坐标系画好且与滚动无关, 所以这里【不】重建位图,
// 只重新摆子控件 + 触发一次 WM_PAINT 换 BitBlt 源点 —— 滚动因此是廉价操作。
void ScrollTo(HWND hwnd, int pos) {
    RECT rc; GetClientRect(hwnd, &rc);
    // RECT 成员是 LONG, 与 int 混用会让 std::max 的模板实参推导出现歧义 (MSVC C2672),
    // 故先显式收敛到 int 再比较。下面 ComputeLayout 里也是同样的处理。
    const int clientH = (int)rc.bottom;
    const int maxPos  = (std::max)(0, g_contentH - clientH);
    pos = (std::min)((std::max)(pos, 0), maxPos);
    if (pos == g_scrollY) return;
    g_scrollY = pos;
    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_POS; si.nPos = g_scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    LayoutChildren(hwnd, ComputeLayout(hwnd));
    InvalidateRect(hwnd, NULL, FALSE);   // WM_ERASEBKGND 恒返回 1, 不需要擦除
}

void RebuildChartCache(HWND hwnd) {
    const Layout L = ComputeLayout(hwnd);
    // v0.1.4.0: 位图不再是"客户区大小", 而是【内容坐标系】的整幅高度 (contentH)。
    //   滚动时只换 BitBlt 的源点 Y, 不必重画; 只有尺寸变化才需要重建。
    //   代价是位图内存 = clientW × contentH × 4B (1260×1460 约 7MB; 4K 全屏下约 23MB),
    //   对本工具可接受, 换来的是滚动完全不掉帧。
    const int w = L.clientW, h = L.contentH;
    if (w <= 0 || h <= 0) return;
    RECT rc = {0, 0, w, h};

    HDC hdcWnd = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcWnd);
    if (g_hChartBmp) DeleteObject(g_hChartBmp);
    g_hChartBmp = CreateCompatibleBitmap(hdcWnd, w, h);
    // v0.1.3.3: DC / 位图创建失败 (极端低资源、超大窗口) 时早退。旧逻辑会把 NULL 选进
    // hdcMem 继续画 (落在默认 1x1 位图上, 不崩但全部白画); WM_PAINT 端本就有空检查,
    // 这里补对称守卫, 失败时保持 g_hChartBmp = NULL 走"无缓存"路径。
    if (!hdcMem || !g_hChartBmp) {
        if (g_hChartBmp) { DeleteObject(g_hChartBmp); g_hChartBmp = NULL; }
        if (hdcMem) DeleteDC(hdcMem);
        ReleaseDC(hwnd, hdcWnd);
        return;
    }

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hChartBmp);
    FillRect(hdcMem, &rc, (HBRUSH)(COLOR_WINDOW + 1));

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // 布局 (v0.1.4.0 起全部由 ComputeLayout() 从客户区尺寸算出, 不再写死坐标):
        //   header inputs :  15 .. kOutY            (三行"标签 + 输入框", 输入框宽度跟随窗口)
        //   output text   :  kOutY, 高 kOutH
        //   四行图表      :  L.RowY(0..3), 每行 "左 ECDF / 右 MRL"
        //     row0 特许寻访(蓝) / row1 辉光庆典(青) / row2 重构寻访(紫) / row3 武库申领(红)
        //   每张图宽 = (客户区宽 - 两侧留白 - 中缝) / 2, 随窗口横向缩放;
        //   行高 = 剩余高度四等分, 但不低于 kChartMinRowH, 低于就靠滚动条查看。
        // 注意这里画的是【内容坐标系】(Y 不减 g_scrollY) —— 滚动发生在 WM_PAINT 的
        // BitBlt 源点上, 见 WM_PAINT / ScrollTo。

        // ===== 角色池 (特许寻访) =====
        // 角色 ECDF: X 轴覆盖 UP 硬保底 120 (UP 分布延伸到此)
        // v0.1.1 起新增 UP 理论 CDF (g_cdf_char_up): 双状态前向迭代算法
        DrawECDF  (g, Gdiplus::Rect(L.chartX1, L.RowY(0), L.chartW, L.chartRowH),
                   statsChar.freq_all, statsChar.freq_up,
                   statsChar.count_all, statsChar.count_up,
                   statsChar.censored_pity_all, statsChar.censored_pity_up,
                   g_cdf_char, g_cdf_char_up,
                   L"角色 (特许寻访) 累积分布 (ECDF)", 120,
                   /*ecdf_up_step_size=*/1);
        // 角色 MRL: X=80 是综合 6 星硬保底 (理论 MRL 上限), X=120 是 UP 硬保底
        DrawMRL   (g, Gdiplus::Rect(L.chartX2, L.RowY(0), L.chartW, L.chartRowH),
                   statsChar.freq_all, statsChar.freq_up,
                   statsChar.count_all, statsChar.count_up,
                   statsChar.censored_pity_all, statsChar.censored_pity_up,
                   g_cdf_char, g_cdf_char_up,
                   L"角色 (特许寻访) 剩余抽数期望 (MRL)", 120,
                   /*theory_all_cap=*/80, /*theory_up_cap=*/120);

        // ===== 辉光庆典 (Joint) =====
        // 复刻类卡池, 4 个池中 6 星: 莱万汀/洁尔佩塔/艾尔黛拉/骏卫
        // (后两者同时也在常驻名单, 排除后剩下"非常驻"= 真·限定)
        // 综合六星: 机制与 Special 池一致 (0.8% 基础, k=66 软保底, k=80 硬保底,
        //           第 30 抽赠送十连), 直接复用 g_cdf_char。
        // 限定 (非常驻): 与 Special 池 UP 显著不同!
        //   - Special 池: 50/50 歪率 + 120 抽 UP 硬保底, E[首 UP] ≈ 79.29 (原始抽; 净成本 ≈74.33)
        //   - Joint  池: 50/50 但无 UP 大保底 / 无 UP 硬保底 (120 抽是赠送选择券,
        //                与抽卡概率独立), E[首限定] ≈ 104.68 抽 (前向迭代;
        //                几何捷径 2×51.81=103.62 不成立, 见 InitCDFTables 注释)
        //   两者差异: Joint 池长尾远长 — 没有兜底, 极端非酋可能要 200+ 抽。
        //   理论 CDF 单独建表 g_cdf_joint_up (真实前向迭代, n=30 展开 11 次判定, 见 InitCDFTables)。
        // X 轴上限设 240 (= 3 × 80), 覆盖 CDF ~93.5%, 视觉上足以体现长尾形态。
        // v0.1.2.4: cdf_len = 242 (joint UP CDF 数组缩回 X=240, 与图表 X 轴一致).
        //   MRL 计算用解析长尾延伸 (g_joint_tail_mean_excess), 把 CDF 在 240 之后的
        //   ~7% 长尾质量用单点近似补回, 让 MRL[0] 从无延伸的 ~82 修正回真值 ~104.68.
        //   drawTheoryCDF 仍画到数组末端 (cdf[240]≈0.93), ECDF 视觉上诚实显示截断.
        DrawECDF  (g, Gdiplus::Rect(L.chartX1, L.RowY(1), L.chartW, L.chartRowH),
                   statsJoint.freq_all, statsJoint.freq_up,
                   statsJoint.count_all, statsJoint.count_up,
                   statsJoint.censored_pity_all, statsJoint.censored_pity_up,
                   g_cdf_char, g_cdf_joint_up,
                   L"角色 (辉光庆典) 累积分布 (ECDF)", 240,
                   /*ecdf_up_step_size=*/1);
        DrawMRL   (g, Gdiplus::Rect(L.chartX2, L.RowY(1), L.chartW, L.chartRowH),
                   statsJoint.freq_all, statsJoint.freq_up,
                   statsJoint.count_all, statsJoint.count_up,
                   statsJoint.censored_pity_all, statsJoint.censored_pity_up,
                   g_cdf_char, g_cdf_joint_up,
                   L"角色 (辉光庆典) 剩余抽数期望 (MRL)", 240,
                   /*theory_all_cap=*/80, /*theory_up_cap=*/240,
                   /*tail_mean_excess_up=*/g_joint_tail_mean_excess);

        // ===== 重构寻访 (Refactor / RE-Factor, 1.5 新增) =====
        // 首期「绚丽异彩」重构寻访#1, 2026/09/24 12:00 开启, UP = 伊冯 (旧限定复刻)。
        // 池中六星只有 6 个 = 当期 UP + 5 名常驻 (不含往期滞留的限定角), 所以"非常驻 = UP"
        //   这条判定在本池是严格成立的 —— 比特许池 (8 个六星, 含前两期限定) 还干净。
        // 数值与特许寻访逐字段相同 (0.8% 基础 / 66 抽起 +5% 软保底 / 80 硬保底 / 120 UP 硬保底),
        //   唯一差异是赠送十连从 1 次 (累计 30 抽) 变成 3 次 (累计 30/60/90 抽),
        //   故综合与 UP 都另建表: g_cdf_refactor / g_cdf_refactor_up (见 InitCDFTables)。
        //   E[首六星] ≈ 51.37 (特许 51.81), E[首 UP] ≈ 77.83 (特许 79.29) —— 多出的两次
        //   赠送十连让两个期望都略微下降。
        // X 轴与特许池一致取 120 (UP 硬保底), MRL 的理论上限同为 80 / 120。
        DrawECDF  (g, Gdiplus::Rect(L.chartX1, L.RowY(2), L.chartW, L.chartRowH),
                   statsRefactor.freq_all, statsRefactor.freq_up,
                   statsRefactor.count_all, statsRefactor.count_up,
                   statsRefactor.censored_pity_all, statsRefactor.censored_pity_up,
                   g_cdf_refactor, g_cdf_refactor_up,
                   L"角色 (重构寻访) 累积分布 (ECDF)", 120,
                   /*ecdf_up_step_size=*/1);
        DrawMRL   (g, Gdiplus::Rect(L.chartX2, L.RowY(2), L.chartW, L.chartRowH),
                   statsRefactor.freq_all, statsRefactor.freq_up,
                   statsRefactor.count_all, statsRefactor.count_up,
                   statsRefactor.censored_pity_all, statsRefactor.censored_pity_up,
                   g_cdf_refactor, g_cdf_refactor_up,
                   L"角色 (重构寻访) 剩余抽数期望 (MRL)", 120,
                   /*theory_all_cap=*/80, /*theory_up_cap=*/120);

        // ===== 武器池 =====
        // 武器 ECDF: X 轴覆盖 UP 硬保底 80
        // v0.1.1 起新增 UP 理论 CDF (g_cdf_wep_up): 4×8 状态机
        // ECDF 用真阶梯 (拨内 CDF 平坦, 10 倍数处跳跃) 体现"10 抽一组"机制
        DrawECDF  (g, Gdiplus::Rect(L.chartX1, L.RowY(3), L.chartW, L.chartRowH),
                   statsWep.freq_all, statsWep.freq_up,
                   statsWep.count_all, statsWep.count_up,
                   statsWep.censored_pity_all, statsWep.censored_pity_up,
                   g_cdf_wep, g_cdf_wep_up,
                   L"武器累积分布 (ECDF)", 80,
                   /*ecdf_up_step_size=*/10);
        // 武器 MRL: X=40 综合硬保底, X=80 UP 硬保底
        DrawMRL   (g, Gdiplus::Rect(L.chartX2, L.RowY(3), L.chartW, L.chartRowH),
                   statsWep.freq_all, statsWep.freq_up,
                   statsWep.count_all, statsWep.count_up,
                   statsWep.censored_pity_all, statsWep.censored_pity_up,
                   g_cdf_wep, g_cdf_wep_up,
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
        hHintLabel = CreateWindowW(L"STATIC",
            L"支持\x201C限定角色卡池:当期UP角色\x201D映射。未包含的限定角色卡池将仅排查常驻六星角色名单。",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(15), DPIScale(1000), DPIScale(20), hwnd, NULL, NULL, NULL);
        // 常驻(基础寻访)六星角色。截至 2026-09-06 仍是这 5 人, 自公测以来【没有增补过】——
        // 每期「特许寻访」公告都带同一条条款:「※ 在「特许寻访」中概率提升的6星干员, 将于
        // 3次「特许寻访」结束后, 移出「特许寻访」全部可能出现的干员列表。移出后, 概率提升的
        // 6星干员不会进入「基础寻访」。」即终末地【没有】限定角色下放常驻的机制。
        // 数据源: 客户端 GachaCharPoolContentTable 的 standard / beginner 两池六星恒为这 5 人。
        hCharLabel = CreateWindowW(L"STATIC", L"常驻六星角色:",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(45), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
        hCharEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"骏卫,黎风,别礼,余烬,艾尔黛拉",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            DPIScale(120), DPIScale(40), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);
        hPoolMapLabel = CreateWindowW(L"STATIC", L"当期UP角色:",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(75), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
        // 「卡池名 : 当期UP角色」映射。这份映射【必须补全】, 因为特许寻访池里的六星
        // 恒为 8 个 = 当期 UP + 前两期的限定干员 + 5 名常驻 (限定角色 UP 期结束后还会
        // 在池中滞留 2 期才移出)。例如「冬猎」池 = 提弗洛斯(UP)/梨诺/诀 + 5 常驻 ——
        // 若缺映射而回退到"不在常驻名单 = UP"的排除法, 歪出的 梨诺/诀 会被误判成当期 UP。
        // 下列 12 期与客户端 GachaCharPoolTable 的 name.cn / upCharIds 逐条核对一致:
        //   special_1_0_1 熔火灼痕:莱万汀   special_1_0_2 热烈色彩:伊冯
        //   special_1_0_3 轻飘飘的信使:洁尔佩塔  special_1_1_1 河流的女儿:汤汤
        //   special_1_1_2 狼珀:洛茜        special_1_2_1 春雷动，万物生:庄方宜
        //   special_1_3_1 拳出无悔:弭弗    special_1_3_2 逐罪者:卡缪
        //   special_1_4_1 临渊望北:诀      special_1_4_2 晨星于此闪耀:梨诺
        //   special_1_5_1 冬猎:提弗洛斯    rerun_chr_yvonne 绚丽异彩:伊冯 (重构寻访, 9/24 开)
        // 注意「绚丽异彩」是伊冯的复刻, 与她 1.0 的原池「热烈色彩」并列, 两条都要留。
        hPoolMapEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"熔火灼痕:莱万汀,轻飘飘的信使:洁尔佩塔,热烈色彩:伊冯,河流的女儿:汤汤,狼珀:洛茜,春雷动，万物生:庄方宜,拳出无悔:弭弗,逐罪者:卡缪,临渊望北:诀,晨星于此闪耀:梨诺,冬猎:提弗洛斯,绚丽异彩:伊冯",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            DPIScale(120), DPIScale(70), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);
        hWepLabel = CreateWindowW(L"STATIC", L"常驻六星武器:",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(105), DPIScale(95), DPIScale(20), hwnd, NULL, NULL, NULL);
        // 这份名单的语义是【已知的"非当期 UP"六星武器白名单】: 武器池的 Calculate() 传的
        // pool_map 是空的 {}, UP 判定 100% 靠"不在本名单里 ⇒ 当期 UP"。所以名单里混入一件
        // 限定 UP 武器, 抽到它的玩家就会被记成"歪", 武器池 UP 率被系统性拉低。
        //
        // v0.1.4.0 修正: 删除【赤缨】。它是 1.3 上半「绛结申领」的当期 UP (弭弗专武),
        //   是 2026-06-08 那次数据更新 (commit 5440f37) 一并塞进来的录入失误 ——
        //   同批的 雾中微光/灯火使命/幻想苦痛 确实不是 UP, 只有赤缨归类错了。
        //   证据: 把客户端 GachaWeaponPoolContentTable 全部 19 个武器池的六星按
        //   isHardGuaranteeItem 展开后, "当 UP 出现过"与"当陪跑出现过"两个集合【完全不相交】,
        //   赤缨只出现在 weponbox_1_3_1 的 UP 位, 从未作为陪跑六星出现在任何池里。
        //   (也可用官方免 token 接口自查:
        //    https://ef-webview.gryphline.com/api/content?lang=zh-cn&pool_id=<池ID>&server_id=3 )
        //
        // 限定武器 (只作为某期 UP 出现, 都【不该】进本名单):
        //   熔铸火焰、艺术暴君、使命必达、落草、狼之绯、孤舟、赤缨、镀红祝福、
        //   四二式·肃阵 (1.4 军列申领, 诀专武)、曜夜的首演 (1.4 明曜申领, 梨诺专武)、
        //   寒夜幽影 (1.5 幽寒申领, 提弗洛斯专武)
        //   注: "限定"不等于"只出现一次" —— 艺术暴君已在 9/24 的「点绘申领」(重构申领) 复刻。
        //   注: 四二式·肃阵 中间是间隔号 U+00B7, 不是全角冒号也不是 ASCII 句点。
        //
        // 名单里另有 8 件是【通行证(武器补给)/活动直给】的六星: 黯色火炬、领航者、
        //   作品：蚀迹、光荣记忆、望乡、雾中微光、灯火使命、幻想苦痛。它们不在任何申领池,
        //   永远不会出现在 /api/record/weapon 里, 留着无害也无作用, 保留以免误删。
        //
        // 名单里的 赫拉芬格/沧溟星梦/不知归/负山/大雷斑 是 5 个【常驻武器申领池】
        //   (weaponbox_constant_1..5, 长期开放) 各自的固定 UP。按上面的语义它们本不该在
        //   白名单里, 但这些池的 poolId 含 "constant" → ParseGachaType 判为 GachaType::Constant
        //   → 已被 wepPath 整体排除在武器统计之外, 记录根本进不了武器桶, 故留着无影响;
        //   且按"常驻武器"的字面语义它们也确实属实。★ 若将来放开 Constant 池参与统计,
        //   这 5 件必须同时从本名单移除, 否则常驻池玩家的 win_5050 会恒为 0。
        hWepEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"宏愿,不知归,黯色火炬,扶摇,热熔切割器,显赫声名,白夜新星,大雷斑,赫拉芬格,典范,昔日精品,破碎君王,J.E.T.,骁勇,负山,同类相食,楔子,领航者,骑士精神,遗忘,爆破单元,作品：蚀迹,沧溟星梦,光荣记忆,望乡,雾中微光,灯火使命,幻想苦痛",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            DPIScale(120), DPIScale(100), DPIScale(1120), DPIScale(26), hwnd, NULL, NULL, NULL);

        hOutEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"等待拖入文件...",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            DPIScale(20), DPIScale(135), DPIScale(1220), DPIScale(280), hwnd, NULL, NULL, NULL);

        DWORD tabStops[] = {50};
        SendMessage(hOutEdit, EM_SETTABSTOPS, 1, (LPARAM)tabStops);
        SendMessage(hOutEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor(COLOR_3DFACE));

        for (HWND h : {hHintLabel, hCharLabel, hCharEdit, hPoolMapLabel, hPoolMapEdit,
                       hWepLabel, hWepEdit, hOutEdit})
            SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);

        // v0.1.4.0: 上面 CreateWindow 里那些坐标只是占位 —— 真正的位置一律由
        //   LayoutChildren() 按当前客户区尺寸决定, 窗口每次缩放/滚动都会重算。
        {
            const Layout L = ComputeLayout(hwnd);
            UpdateScrollInfo(hwnd, L);
            LayoutChildren(hwnd, L);
        }
        RebuildChartCache(hwnd);
        break;
    }
    // v0.1.4.0: 窗口可缩放后, 尺寸一变就要重排控件 + 按新宽高重画图表。
    //   图表行高/宽度都与客户区尺寸相关, 所以这里必须重建位图, 不能只 Invalidate。
    //   重建 = 8 张 GDI+ 图重画, 拖动边框时每帧都会跑一次; 实测若觉得卡, 可以改成
    //   WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE 期间只重排控件、松手后再重建图表。
    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) break;      // 最小化时客户区为 0, 没必要折腾
        const Layout L = ComputeLayout(hwnd);
        if (L.clientW <= 0 || L.clientH <= 0) break;
        // WM_SIZE 会以【相同尺寸】重复到达 (例如 SetScrollInfo 改变滚动条状态之后),
        // 尺寸没变就没必要重画 8 张图。
        static int s_lastW = -1, s_lastH = -1;
        const bool sizeChanged = (L.clientW != s_lastW || L.clientH != s_lastH);
        s_lastW = L.clientW; s_lastH = L.clientH;

        const int maxScroll = (std::max)(0, L.contentH - L.clientH);
        if (g_scrollY > maxScroll) g_scrollY = maxScroll;   // 窗口变高时把内容拉回来
        UpdateScrollInfo(hwnd, L);
        LayoutChildren(hwnd, L);
        if (sizeChanged) RebuildChartCache(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    // 限制最小尺寸: 再小图表就没有可读性了, 且极端窄高会让布局退化。
    // g_dpi 在 CreateWindowW 之前就已设置好, 这里可以安全使用。
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = DPIScale(ui::kMinWindowW);
        mmi->ptMinTrackSize.y = DPIScale(ui::kMinWindowH);
        break;
    }
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si) };
        si.fMask = SIF_ALL;
        if (!GetScrollInfo(hwnd, SB_VERT, &si)) break;
        int pos = si.nPos;
        const int line = DPIScale(ui::kScrollLine);
        switch (LOWORD(wParam)) {
        case SB_TOP:           pos  = si.nMin;          break;
        case SB_BOTTOM:        pos  = si.nMax;          break;
        case SB_LINEUP:        pos -= line;             break;
        case SB_LINEDOWN:      pos += line;             break;
        case SB_PAGEUP:        pos -= (int)si.nPage;    break;
        case SB_PAGEDOWN:      pos += (int)si.nPage;    break;
        // THUMBTRACK 用 nTrackPos 才能跟手 (nPos 在拖动过程中不更新)
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos  = si.nTrackPos;     break;
        default: break;
        }
        ScrollTo(hwnd, pos);
        break;
    }
    // 滚轮: 一格滚三"行"。注意鼠标停在输出文本框上时消息会先给那个 RichEdit
    // (它自带 WS_VSCROLL), 由它滚自己的内容 —— 与 Apple 版嵌套 ScrollView 行为一致。
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        ScrollTo(hwnd, g_scrollY - delta * DPIScale(ui::kScrollLine) * 3 / WHEEL_DELTA);
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
            // v0.1.4.0: 位图是内容坐标系的整幅图, 这里按当前滚动量偏移源点 Y 取一片。
            //   滚动之所以廉价就在于此 —— 只是换个源点, 不重画任何图表。
            BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top,
                   hdcMem, ps.rcPaint.left, ps.rcPaint.top + g_scrollY, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: {
        // v0.1.3.3: 退出前先 join worker。此刻 hwnd 仍有效, worker 的 PostMessageW 仍能
        // 成功入队; worker 从不 SendMessage 回主线程 (只用非阻塞 PostMessage), 故这里等待
        // 不会死锁。等待时长 = 剩余分析时间 (本工具数据量下为毫秒级)。
        // 不等待的后果: WinMain 返回 → ExitProcess 直接终止 worker, 其若恰好持有 CRT 堆锁,
        // 退出流程可能挂死 —— 这才是真实风险 (旧注释"OS 统一回收、无实际危害"不成立)。
        if (g_hWorker) {
            WaitForSingleObject(g_hWorker, INFINITE);
            CloseHandle(g_hWorker);
            g_hWorker = NULL;
        }
        // join 之后, 若 worker 投递过结果, 该消息还躺在线程队列里且永远不会被派发
        // (窗口即将销毁) —— 在此 reap 并释放载荷, 否则 ProcessOutput + mmap 句柄泄漏到
        // 进程退出。与 Consume 不会双重 delete: 消息要么已派发 (队列里没有), 要么在队列
        // 里 (未派发), 二者互斥; reap 只删队列里取出的那份。
        MSG pending;
        while (PeekMessageW(&pending, hwnd, WM_APP_PROCESS_DONE, WM_APP_PROCESS_DONE, PM_REMOVE)) {
            delete reinterpret_cast<ProcessOutput*>(pending.lParam);
        }
        InterlockedExchange(&g_processing, 0);
        if (g_hChartBmp) { DeleteObject(g_hChartBmp); g_hChartBmp = NULL; }
        if (hFont) DeleteObject(hFont);
        PostQuitMessage(0);
        break;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // v0.1.3.3: RichEdit50W 依赖 Msftedit.dll。消费级 Windows (含 N 版) 均随系统分发,
    // 仅 WinPE / 深度精简系统可能缺失 —— 旧版忽略返回值, 缺失时四个 RichEdit 创建为 NULL,
    // 后续 SendMessage / SetWindowText 全部静默落空, 界面空白且无任何提示。现显式报错退出。
    if (!LoadLibraryW(L"Msftedit.dll")) {
        MessageBoxW(NULL,
                    L"无法加载 Msftedit.dll (RichEdit 控件库)。\n"
                    L"本程序的文本界面依赖该系统组件, 请在完整版 Windows 上运行。",
                    L"组件缺失", MB_OK | MB_ICONERROR);
        return 1;
    }
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

    // v0.1.4.0: 窗口改为【可缩放 + 可最大化 + 带垂直滚动条】。
    //   此前为了保证图表布局不被破坏, 特意去掉了 WS_THICKFRAME / WS_MAXIMIZEBOX,
    //   代价是窗口高度必须能装下全部内容 —— 加入第 4 行图表 (重构寻访) 后需要 1460px,
    //   在 1080p 屏上底部会被截掉且无法滚动查看。
    //   现在坐标全部由 ComputeLayout() 动态算出, 缩放安全; 装不下就滚动 (参考 Apple 版
    //   macOS ContentView 把主区域套进 ScrollView 的做法)。
    DWORD dwStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL;
    // 初始窗口尺寸 (之后用户可以随意拖动):
    //   宽度 (v0.1.2.4): 1280 → 1260, 让左右留白对称 (内容右边界 x=1240, 左留白 20 → 右留白也 20).
    //   高度: 取 4 行图表都不压缩时的完整内容高 (1460), 但不超过工作区高度的 92% ——
    //         这样 1440p/4K 上开箱即见全部内容, 1080p 上则自动开成能放下的高度,
    //         底部内容用滚动条查看, 不会一启动就有一半在屏幕外。
    RECT wa = {0};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int waH = (std::max)(DPIScale(ui::kMinWindowH), (int)((wa.bottom - wa.top) * 92 / 100));
    RECT rect = {0, 0, DPIScale(1260), (std::min)(DPIScale(1460), waH)};
    AdjustWindowRectEx(&rect, dwStyle, FALSE, 0);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"终末地抽卡记录分析与可视化",
        dwStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    // v0.1.3.3: GetMessage 的返回值是三态 (>0 取到消息 / 0 收到 WM_QUIT / -1 出错),
    // 旧写法 while (GetMessage(...)) 把 -1 当真值 —— 出错时 msg 内容未定义, 循环会带着
    // 无效消息空转。当前参数 (hwnd=NULL 过滤 + 有效指针) 下 -1 实际不可达, 属防御性修正;
    // 顺带显式用 W 版 (GetMessageW / DispatchMessageW), 与本文件全 W 系窗口代码一致,
    // 不再依赖 UNICODE 宏决定 A/W。
    for (;;) {
        BOOL ret = GetMessageW(&msg, NULL, 0, 0);
        if (ret == 0)  break;   // WM_QUIT: 正常退出
        if (ret == -1) break;   // 错误: msg 未定义, 不可 Dispatch, 直接进入清理
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
