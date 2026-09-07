// ============================================================
// Endfield Gacha Exporter - UIGF v4.2 / 面向数据 / PMR / AoS
// ============================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <ctime>
#include <windows.h>
#include <winhttp.h>
#include <string_view>
#include <charconv>
#include <ranges>
#include <memory_resource>
#include <memory>           // std::make_unique_for_overwrite (C++20): 2MB PMR arena 改在堆上不清零分配
#include <cstdint>          // uint8_t / SIZE_MAX (此前靠传递包含, 这里显式)
#include <array>
#include <numeric>
#include <unordered_set>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "User32.lib")

// ---------------------------------------------------------
// [枚举 / 无堆分配的大小写不敏感包含比较]
// ---------------------------------------------------------
enum class ItemType : uint8_t { Unknown = 0, Character, Weapon };

// 无堆分配的大小写不敏感 find —— 原版每次都 std::string 拷贝,这是 hot-path bug
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
    // 精确匹配优先(UIGF 规范值是 "Character"/"Weapon"),命中率高,路径更快
    if (sv == "Character") return ItemType::Character;
    if (sv == "Weapon")    return ItemType::Weapon;
    // 防御性大小写不敏感回退
    if (ContainsCI(sv, "character")) return ItemType::Character;
    if (ContainsCI(sv, "weapon"))    return ItemType::Weapon;
    return ItemType::Unknown;
}

inline std::string_view ItemTypeToStr(ItemType type) {
    if (type == ItemType::Character) return "Character";
    if (type == ItemType::Weapon)    return "Weapon";
    return "Unknown";
}

// ---------------------------------------------------------
// [极简 JSON 解析 - 修复转义边界]
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
            // 修复:\\ 处理必须是"跳 2 字节",原版 source[endPos]='\\' 后只 endPos++ 一次,边界上会越界
            if (source[endPos] == '\\' && endPos + 1 < source.length()) endPos += 2;
            else ++endPos;
        }
        return (endPos < source.length()) ? source.substr(pos, endPos - pos) : std::string_view{};
    } else {
        size_t endPos = pos;
        while (endPos < source.length() &&
               source[endPos] != ',' && source[endPos] != '}' &&
               source[endPos] != ']' && source[endPos] != ' ' &&
               source[endPos] != '\n' && source[endPos] != '\r') ++endPos;
        return source.substr(pos, endPos - pos);
    }
}

// O(N) 逐字符扫描。
// v0.1.3.3 (A2): 返回值改为 bool —— 是否定位到了 "arrayKey": [ ... ] 数组结构 (数组为空也算
// 定位成功)。基底加载用它区分"结构正确的空数据"(正常, 0 条) 与"无结构的损坏/异类文件"
// (中止, 防止覆盖原历史)。原有调用方不取返回值, 行为不变。
template<typename Callback>
bool ForEachJsonObject(std::string_view source, std::string_view arrayKey, Callback&& cb) {
    size_t pos = FindJsonKey(source, arrayKey);
    if (pos == std::string_view::npos) return false;
    pos = source.find(':', pos + arrayKey.length() + 2);
    if (pos == std::string_view::npos) return false;
    pos = source.find('[', pos);
    if (pos == std::string_view::npos) return false;

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
    return true;   // 已定位数组 (即便其中 0 个对象)
}

inline std::wstring Utf8ToWstring(std::string_view str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

inline std::string_view ExtractUrlParam(std::string_view url, std::string_view key) {
    size_t pos = url.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.length();
    size_t end = url.find('&', pos);
    return (end == std::string_view::npos) ? url.substr(pos) : url.substr(pos, end - pos);
}

// ---------------------------------------------------------
// [AoS 记录 - 导出场景多字段一起访问,AoS 空间局部性更好]
// safe_id 和 original_id 合并为一个(原版两个字段值完全相同)
// ---------------------------------------------------------
struct ExportRecord {
    long long safe_id;       // 武器取负,用于去重和分区排序
    long long timestamp;
    std::string_view poolId;
    std::string_view item_id;
    std::string_view name;
    ItemType item_type;
    std::string_view rank_type;
    std::string_view poolName;
    std::string_view weaponType;
    uint8_t isNew;
    uint8_t isFree;
};

// ---------------------------------------------------------
// [RAII 句柄]
// ---------------------------------------------------------
struct FileHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    operator HANDLE() const { return h; }
};
struct MappingHandle {
    HANDLE h = NULL;
    ~MappingHandle() { if (h) CloseHandle(h); }
    operator HANDLE() const { return h; }
};
struct MapView {
    const void* p = nullptr;
    ~MapView() { if (p) UnmapViewOfFile(p); }
};
struct WinHttpHandle {
    HINTERNET h = NULL;
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

// ---------------------------------------------------------
// [FetchPath - 修复 WinHttpQueryDataAvailable 失败死循环]
// ---------------------------------------------------------
// v0.1.3.3: 增加 netOk 出参 —— 旧版把"读流中途失败"与"读到自然结束"混为一谈, 都静默返回
// 已收到的部分字节。截断恰好落在 list 中段时, 解析端能读出 code:0 与若干完整记录, 而
// "hasMore" 键缺失会被当成 false → 看似自然翻页结束 → 部分数据被当完整数据提交, 形成
// 记录缺口且【无任何报错】。现在: 仅 HTTP 200 且响应体完整读毕才置 netOk=true; 任一环节
// (打开/发送/接收/状态码/可用量查询/读取) 失败都置 false, 由调用方按失败处理。
std::string FetchPath(HINTERNET hConnect, const std::wstring& path, bool& netOk) {
    netOk = false;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    std::string response;
    if (!hRequest) return response;

    bool ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hRequest, NULL);

    if (ok) {
        DWORD status = 0, statusSize = sizeof(status);
        ok = WinHttpQueryHeaders(hRequest,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX) &&
             status == 200;
    }

    if (ok) {
        // 固定 16KB 复用缓冲: 不再为 >8KB 的区块反复 new/销毁 std::vector<char>。
        // WinHttpQueryDataAvailable 给出当前可读字节数, 再用固定缓冲分块读完该批。
        std::array<char, 16384> readBuf;
        bool readFailed = false;
        bool reading = true;
        while (reading) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &available)) { readFailed = true; break; }
            if (available == 0) break;   // 自然结束 (与查询失败区分开)
            while (available > 0) {
                DWORD bufSz = (DWORD)readBuf.size();
                DWORD chunk  = (available < bufSz) ? available : bufSz;
                DWORD downloaded = 0;
                if (!WinHttpReadData(hRequest, readBuf.data(), chunk, &downloaded) ||
                    downloaded == 0) {
                    readFailed = true;   // 读失败 / 流提前终止: 不可当自然结束
                    reading = false;
                    break;
                }
                response.append(readBuf.data(), downloaded);
                available -= downloaded;
            }
        }
        netOk = !readFailed;
    }
    WinHttpCloseHandle(hRequest);
    return response;
}

struct PoolConfig { std::string poolType, displayName; bool isWeapon; };

// ---------------------------------------------------------
// [非抽卡事件]  v0.1.5.0
//
// /api/record/char 的 list 里除了真实抽卡, 还会混入"发放某个道具"的事件行。
// 目前已确认的一种是【寻访情报书】(kind = "gift_intel_book"): 特许寻访累计 60 次本体抽
// 发放 1 本, 于下一次特许寻访开启后自动转化为该池专有寻访凭证 ×10
// (客户端 GachaCharPoolTypeTable type=0 的 testimonialPullCount = 60, 每个 special_* 池
//  带 testimonialRewardItemId 如 "item_gacha_introletter_1_5_1";
//  官方公告原文见 https://endfield.hypergryph.com/news/6097 的「寻访情报书」一条)。
// 这类行有 seqId / gachaTs / poolId / poolName, 但【没有】charId / charName / rarity。
//
// 处理策略:
//   - 不写进 UIGF 的 "list" —— 那是抽卡记录数组, 混入非抽卡行会让所有读这个文件的
//     工具都得知道这个怪癖 (实测: 第三方平台导出把它们全部剔除, 多个同类工具也都写了
//     过滤)。放进去还会让不做过滤的工具把保底水位每期多算 1 抽。
//   - 但也【不丢弃】: 抽卡记录接口只保留最近 90 天, 本地文件是唯一的长期存档,
//     丢掉就再也取不回来。历史被 90 天窗口截断时, 这条事件的时间戳还能反推出
//     "此刻我在该池已累计满 60 抽"这一信息。
//   - 折中: 存到顶层的 "non_pull_events" 数组里, 且【原样保留服务器返回的整个 JSON 对象】,
//     这样将来出现新的 kind (例如 240 抽的 UP 信物、武器申领的补充武库箱) 也不会丢字段。
//
// raw 是指向 networkPayloads 里某个 std::string 的视图 (与 ExportRecord 的字段同源),
// 在写盘前始终有效。
// ---------------------------------------------------------
struct NonPullEvent {
    long long        safe_id   = 0;   // 与抽卡记录同一套 id 口径 (角色正 / 武器负), 用于去重
    long long        timestamp = 0;   // gachaTs, 仅用于排序
    std::string_view raw;             // 服务器原始 JSON 对象 (含大括号), 原样回写
};

// ---------------------------------------------------------
// [BufferedWriter - 析构 RAII Flush + 短写/失败检查]
// ---------------------------------------------------------
struct BufferedWriter {
    HANDLE hFile;
    char buf[65536];
    DWORD pos = 0;
    bool ok = true;   // 一旦写失败置 false: 后续 Flush/Write 短路, 调用方据此决定是否提交结果

    explicit BufferedWriter(HANDLE h) : hFile(h) {}
    ~BufferedWriter() { Flush(); }

    BufferedWriter(const BufferedWriter&) = delete;
    BufferedWriter& operator=(const BufferedWriter&) = delete;

    // 循环写完整块, 处理短写 (written < pos) 与写失败。任一失败置 ok=false 并停止;
    // 已失败后再调用直接短路, 不向坏句柄重复写 (也避免 pos 不清零导致 Write 死循环)。
    bool Flush() {
        if (!ok) return false;
        if (hFile == INVALID_HANDLE_VALUE) { ok = false; return false; }
        DWORD offset = 0;
        while (offset < pos) {
            DWORD written = 0;
            if (!WriteFile(hFile, buf + offset, pos - offset, &written, nullptr) ||
                written == 0) {
                ok = false;
                return false;
            }
            offset += written;
        }
        pos = 0;
        return true;
    }
    void Write(const char* data, DWORD len) {
        if (!ok) return;
        while (len > 0) {
            DWORD space = sizeof(buf) - pos;
            DWORD chunk = (len < space) ? len : space;
            std::memcpy(buf + pos, data, chunk);
            pos += chunk; data += chunk; len -= chunk;
            if (pos == sizeof(buf) && !Flush()) return;
        }
    }
    void Write(std::string_view sv) { Write(sv.data(), (DWORD)sv.size()); }

    template<size_t N>
    void WriteLit(const char (&s)[N]) {
        if (!ok) return;
        constexpr DWORD len = N - 1;
        if (pos + len > sizeof(buf) && !Flush()) return;
        std::memcpy(buf + pos, s, len);
        pos += len;
    }

    // v0.1.3.3: WriteKV 的 val 全部来自 ExtractJsonValue 的【原始转义形态】视图 (扫描器
    // 不解码转义, 返回引号之间的原文, 例如 `\"` 仍是反斜杠+引号两个字符), 本就是合法的
    // JSON 字符串内容, 必须【原样写出】。旧版 WriteEscaped 在其上再转义一遍, 会把 `\`
    // 翻倍 (`\"` → `\\\"`), 解码后凭空多出反斜杠 —— 每导出一轮膨胀一次, 破坏往返幂等。
    // 游戏名称目前不含 `"` / `\`, 属潜伏缺陷, 但口径必须正确。(若日后需要写【程序生成】
    // 的字符串值, 那才需要转义; 本文件已无此类调用, WriteEscaped 一并移除以防误用。)
    void WriteKV(std::string_view key, std::string_view val) {
        WriteLit("            \"");
        Write(key);
        WriteLit("\": \"");
        Write(val);          // 原始转义形态, 原样写出
        WriteLit("\"");
    }

    void WriteTimeKV(std::string_view key, long long ms_ts) {
        time_t t = ms_ts / 1000;
        struct tm tm_info;
        localtime_s(&tm_info, &t);
        char tbuf[64];
        int len = wsprintfA(tbuf, "%04d-%02d-%02d %02d:%02d:%02d",
                            tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                            tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        WriteLit("            \"");
        Write(key);
        WriteLit("\": \"");
        Write(tbuf, len);
        WriteLit("\"");
    }

    void WriteI64KV(std::string_view key, long long val, bool quotes) {
        char nbuf[32];
        auto [ptr, ec] = std::to_chars(nbuf, nbuf + 32, val);
        WriteLit("            \"");
        Write(key);
        WriteLit("\": ");
        if (quotes) WriteLit("\"");
        Write(nbuf, (DWORD)(ptr - nbuf));
        if (quotes) WriteLit("\"");
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    char urlBuffer[1024];
    printf("请输入您的终末地抽卡记录完整链接 (https://ef-webview.gryphline.com/api/record/<参数>):\n> ");
    if (!fgets(urlBuffer, sizeof(urlBuffer), stdin)) return 1;

    std::string_view inputUrl(urlBuffer);
    while (!inputUrl.empty() &&
           (inputUrl.back() == ' ' || inputUrl.back() == '\n' ||
            inputUrl.back() == '\r' || inputUrl.back() == '\t')) {
        inputUrl.remove_suffix(1);
    }

    auto token = ExtractUrlParam(inputUrl, "token=");
    if (token.empty()) { printf("错误: 无法提取 token。\n"); system("pause"); return 1; }

    auto serverId = ExtractUrlParam(inputUrl, "server_id=");
    if (serverId.empty()) serverId = "1";
    printf("\n已自动识别 Server ID: %.*s\n", (int)serverId.size(), serverId.data());

    // 角色寻访的 pool_type 枚举。
    //
    // v0.1.4.0 新增 E_CharacterGachaPoolType_Rerun (重构寻访 RE-Factor Headhunting):
    //   1.5「雪凇幽梦」引入的第五种角色寻访类型, 首期「绚丽异彩」2026/09/24 12:00 开启,
    //   poolId 形如 "rerun_chr_yvonne" (与其余四种一样, poolId 前缀 = 枚举后缀的小写)。
    //
    // 这个枚举值是【实测确认】的, 不是猜测 —— /api/record/char 会先校验 pool_type 再校验
    // token, 所以不带有效 token 也能判定一个枚举名是否合法:
    //     合法枚举 → {"code":40100,"msg":"Token is invalid"}
    //     非法枚举 → {"code":40000,"msg":"Invalid pool_type"}
    // 于是可以直接枚举出服务端接受的全集 (大小写敏感), 例如:
    //     curl -sG 'https://ef-webview.gryphline.com/api/record/char' \
    //          --data-urlencode 'lang=zh-cn' --data-urlencode 'token=x' \
    //          --data-urlencode 'server_id=1' \
    //          --data-urlencode 'pool_type=E_CharacterGachaPoolType_Rerun'
    //   2026-09-06 实测: 服务端只接受下面这 5 个值, 没有第 6 个。
    //   将来官方再加新池型时, 用同样的方法几秒就能试出新枚举名, 不需要等别人逆向。
    //
    // 武器记录接口没有 pool_type 参数, 所有武器池 (含 1.5 新增的「重构申领」
    // rerun_wpn_*) 都在同一条 /api/record/weapon 时间线里返回, 无需在此登记。
    std::vector<PoolConfig> pools = {
        {"E_CharacterGachaPoolType_Special",  "角色 - 特许寻访", false},
        {"E_CharacterGachaPoolType_Joint",    "角色 - 辉光庆典", false},
        {"E_CharacterGachaPoolType_Rerun",    "角色 - 重构寻访", false},
        {"E_CharacterGachaPoolType_Standard", "角色 - 基础寻访", false},
        {"E_CharacterGachaPoolType_Beginner", "角色 - 启程寻访", false},
        {"",                                   "武器 - 全历史记录", true}
    };

    // PMR:2MB 单调缓冲池, 现放在【堆】上 (与 gui.cpp 的 stack→heap 修复同步, 此前在栈上)。
    //   - make_unique_for_overwrite 不主动清零整个 arena (区别于 std::vector(2MB) / 带括号的
    //     new[]() / calloc 那种值初始化), 可避免无意义地写满 2MB。实际页面提交、物理驻留与
    //     page fault 数仍取决于 Windows 堆分配器、页面复用情况与运行时访问模式 —— 别写死成
    //     "只有写入部分才落物理页"。
    //   - 移到堆后 main 线程不再需要大栈: 构建时的 /STACK:4194304 可去掉, 回默认栈即可
    //     (本程序除这块外最大的栈对象是 BufferedWriter 的 64KB, 默认 1MB 栈绰绰有余)。
    //   - 没显式指定 upstream → 默认 get_default_resource(): 记录数远超 reserve(10000) 把 2MB
    //     用尽时会 fallback 到堆而非崩溃 (有意为之)。monotonic_buffer_resource 不回收扩容前的
    //     旧块, 直到整个 pool 析构。
    // 生命周期: arena → pool → alloc 顺序声明, 析构逆序 (各 pmr 容器更早析构), pool 引用的
    //   arena 内存在 pool 存活期间始终有效。
    constexpr size_t kArenaSize = 2 * 1024 * 1024;
    auto arena = std::make_unique_for_overwrite<std::byte[]>(kArenaSize);  // 堆, 不清零 (C++20)
    std::pmr::monotonic_buffer_resource pool(arena.get(), kArenaSize);
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    // AoS 记录
    std::pmr::vector<ExportRecord> records(alloc);
    records.reserve(10000);

    std::deque<std::string> networkPayloads;

    // 去重:unordered_set O(1),原版 vector O(n²) 插入
    std::pmr::unordered_set<long long> local_safe_ids(alloc);
    local_safe_ids.reserve(10000);

    // 非抽卡事件 (见 NonPullEvent 说明): 与抽卡记录共用 local_safe_ids 做去重,
    // 但单独存放、单独写盘, 不进 UIGF 的 "list"。
    std::pmr::vector<NonPullEvent> events(alloc);
    events.reserve(64);

    std::string uigfFilename = "uigf_endfield.json";

    // ---- 读取本地老记录(读完立即释放句柄,避免锁住目标文件)----
    // v0.1.3.3 (A2) 基底验收口径: 文件【不存在】= 全新拉取 (正常); 文件存在但打不开 /
    // 0 字节 / 映射失败 / 找不到 "list" 数组结构 = 按损坏处理, 中止且不写盘, 防止运行
    // 结束时 MoveFileEx 覆盖原历史; "list" 数组存在但为空 = 结构正确的空数据, 0 条正常继续。
    bool baseFileExists = false;   // 文件存在 (无论能否读)
    bool baseLoadOk     = false;   // 打开 + 映射 + 结构 ("list" 数组) 三关全过
    {
        FileHandle hFile;
        hFile.h = CreateFileA(uigfFilename.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile.h != INVALID_HANDLE_VALUE) {
            baseFileExists = true;
            // 与 gui.cpp 同步: GetFileSize (32 位, 截断 >4GB) → GetFileSizeEx (64 位) + size_t
            // 上界校验。本地 uigf 文件正常远小于 4GB, 但用 64 位读 + 显式校验更稳健
            // (尤其 32 位构建下 size_t 仅 4GB)。
            LARGE_INTEGER fileSize64{};
            if (GetFileSizeEx(hFile, &fileSize64) &&
                fileSize64.QuadPart > 0 &&
                static_cast<unsigned long long>(fileSize64.QuadPart) <=
                    static_cast<unsigned long long>(SIZE_MAX)) {
                size_t fileSize = static_cast<size_t>(fileSize64.QuadPart);
                MappingHandle hMap;
                hMap.h = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
                if (hMap.h) {
                    MapView view;
                    view.p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                    if (view.p) {
                        // 关键修复:把 mmap 数据复制到 networkPayloads,让 string_view
                        // 指向 deque 里的 string(deque push_back 不失效指针)。
                        // 这样就可以立即关闭 mmap/file 句柄,不会锁住目标文件导致
                        // 后续 MoveFileExA 失败。
                        networkPayloads.emplace_back(
                            std::string((const char*)view.p, fileSize));
                        std::string_view bufferView = networkPayloads.back();

                        // RAII Guard 会在退出本作用域时自动 unmap / close

                        if (bufferView.size() >= 3 &&
                            (unsigned char)bufferView[0] == 0xEF &&
                            (unsigned char)bufferView[1] == 0xBB &&
                            (unsigned char)bufferView[2] == 0xBF) {
                            bufferView.remove_prefix(3);
                        }

                        baseLoadOk = ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
                            std::string_view raw_id = ExtractJsonValue(itemStr, "id", true);
                            long long parsed_id = 0, parsed_ts = 0;
                            if (!raw_id.empty()) {
                                std::from_chars(raw_id.data(), raw_id.data() + raw_id.size(), parsed_id);
                            }
                            // UIGF v4.2: gacha_ts (原 gachaTs)
                            std::string_view tsStr = ExtractJsonValue(itemStr, "gacha_ts", true);
                            if (!tsStr.empty()) {
                                std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), parsed_ts);
                            }

                            ItemType it = ParseItemType(ExtractJsonValue(itemStr, "item_type", true));

                            // UIGF v4.2: gacha_type / item_name / pool_name / weapon_type / is_new / is_free
                            // (原: uigf_gacha_type / name / poolName / weaponType / isNew / isFree)
                            //
                            // ForEachJsonObject 找的是 "list" 这个 key —— v4.2 里 "list" 仅在
                            // endfield[0] 内层出现,顶层 info 块没有 list,所以直接命中正确数组,
                            // 不需要先穿透 endfield。
                            records.push_back(ExportRecord{
                                parsed_id,
                                parsed_ts,
                                ExtractJsonValue(itemStr, "gacha_type", true),
                                ExtractJsonValue(itemStr, "item_id", true),
                                ExtractJsonValue(itemStr, "item_name", true),
                                it,
                                ExtractJsonValue(itemStr, "rank_type", true),
                                ExtractJsonValue(itemStr, "pool_name", true),
                                ExtractJsonValue(itemStr, "weapon_type", true),
                                (uint8_t)(ExtractJsonValue(itemStr, "is_new",  false) == "true" ? 1 : 0),
                                (uint8_t)(ExtractJsonValue(itemStr, "is_free", false) == "true" ? 1 : 0)
                            });
                            local_safe_ids.insert(parsed_id);
                        });

                        // 非抽卡事件的往返读取。旧版文件没有这个键, ForEachJsonObject
                        // 直接返回 false, 这里不关心返回值 —— 缺失 = 0 条, 属正常情况,
                        // 不能影响 baseLoadOk (那是"文件是否可用"的判据, 只看 "list")。
                        //
                        // 包装对象的形状是 { "id", "gacha_ts", "raw": {服务器原始对象} }。
                        // "id" / "gacha_ts" 写在 "raw" 之前, 而 FindJsonKey 取的是首个匹配,
                        // 所以即便将来某个 kind 的原始对象里也有同名键, 也不会被读串。
                        ForEachJsonObject(bufferView, "non_pull_events", [&](std::string_view evtStr) {
                            NonPullEvent ev;
                            std::string_view idStr = ExtractJsonValue(evtStr, "id", true);
                            if (idStr.empty()) return;
                            std::from_chars(idStr.data(), idStr.data() + idStr.size(), ev.safe_id);
                            std::string_view tsStr = ExtractJsonValue(evtStr, "gacha_ts", true);
                            if (!tsStr.empty()) {
                                std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), ev.timestamp);
                            }
                            // 取出 "raw" 后面那个完整对象 (含大括号), 原样留存
                            size_t rawPos = FindJsonKey(evtStr, "raw");
                            if (rawPos != std::string_view::npos) {
                                size_t braceOpen = evtStr.find('{', rawPos);
                                if (braceOpen != std::string_view::npos) {
                                    int depth = 0;
                                    for (size_t k = braceOpen; k < evtStr.size(); ++k) {
                                        char c = evtStr[k];
                                        if (c == '"') {   // 跳过字符串, 里面的花括号不计深度
                                            for (++k; k < evtStr.size(); ++k) {
                                                if (evtStr[k] == '\\' && k + 1 < evtStr.size()) { ++k; continue; }
                                                if (evtStr[k] == '"') break;
                                            }
                                            continue;
                                        }
                                        if (c == '{') ++depth;
                                        else if (c == '}') {
                                            if (--depth == 0) {
                                                ev.raw = evtStr.substr(braceOpen, k - braceOpen + 1);
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            if (ev.raw.empty()) return;      // 结构异常的条目直接丢弃, 不回写
                            events.push_back(ev);
                            local_safe_ids.insert(ev.safe_id);
                        });
                    }
                }
            }
            if (baseLoadOk) {
                printf("成功加载本地存储的 %zu 条抽卡记录", records.size());
                if (!events.empty()) printf(" 与 %zu 条非抽卡事件", events.size());
                printf("。\n");
            }
        } else {
            DWORD openErr = GetLastError();
            if (openErr == ERROR_FILE_NOT_FOUND || openErr == ERROR_PATH_NOT_FOUND) {
                printf("未发现本地记录,将创建新文件。\n");
            } else {
                baseFileExists = true;   // 存在但打不开 (占用/权限): 按"存在但不可用"走下方中止
            }
        }
    }  // <- Guard 全部析构,文件完全释放

    if (baseFileExists && !baseLoadOk) {
        printf("[错误] 本地记录文件 %s 存在, 但无法读取或不含 \"list\" 数组结构\n", uigfFilename.c_str());
        printf("       (0 字节、被占用、已损坏或非本工具格式)。\n");
        printf("       为防止本次运行结束时覆盖原有历史, 已中止。请检查或移走该文件后重试。\n");
        system("pause");
        return 1;
    }

    std::wstring hostName = L"ef-webview.gryphline.com";
    if (inputUrl.find("hypergryph") != std::string_view::npos) {
        hostName = L"ef-webview.hypergryph.com";
        printf("已自动识别区服: 国服 (Hypergryph)\n");
    } else {
        printf("已自动识别区服: 国际服 (Gryphline)\n");
    }

    printf("\n========================================\n");
    printf("        开始向服务器拉取抽卡数据\n");
    printf("========================================\n");

    WinHttpHandle hSession;
    hSession.h = WinHttpOpen(L"Endfield Gacha Tool", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    WinHttpHandle hConnect;
    if (hSession.h) {
        hConnect.h = WinHttpConnect(hSession, hostName.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if (!hConnect.h) { printf("网络初始化失败!\n"); system("pause"); return 1; }

    // sessionIds 用 unordered_set(O(1) 去重)
    std::pmr::unordered_set<long long> sessionIds(alloc);
    sessionIds.reserve(2000);

    std::string tokenStr(token), serverIdStr(serverId);

    // v0.1.3.3: 翻页中途异常停止保护。本池已吃进部分新记录后再异常停止时, 若照常写盘,
    // 部分新记录一旦落地, 下次增量拉取在最新记录处即触达老记录而停 —— 中间缺失的更早
    // 页【永远不会回补】。置位后整次更新中止、不写盘。语义与 macOS / iOS 端对齐。
    bool fetchAborted = false;

    for (const auto& poolCfg : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", poolCfg.displayName.c_str());
        bool hasMore = true, reachedExisting = false;
        long long nextSeqIdCursor = 0;
        int page = 1, poolFetchedCount = 0;
        char seqIdBuf[32];

        while (hasMore && !reachedExisting) {
            std::string currentPath = poolCfg.isWeapon
                ? "/api/record/weapon?lang=zh-cn&token=" + tokenStr + "&server_id=" + serverIdStr
                : "/api/record/char?lang=zh-cn&pool_type=" + poolCfg.poolType
                    + "&token=" + tokenStr + "&server_id=" + serverIdStr;
            if (page > 1 && nextSeqIdCursor > 0) {
                auto [ptr, ec] = std::to_chars(seqIdBuf, seqIdBuf + 32, nextSeqIdCursor);
                currentPath.append("&seq_id=").append(seqIdBuf, ptr - seqIdBuf);
            }

            bool netOk = false;
            networkPayloads.emplace_back(FetchPath(hConnect, Utf8ToWstring(currentPath), netOk));
            std::string_view resView = networkPayloads.back();

            // 三个异常分支: 页 1 失败 (poolFetchedCount == 0, 本池无部分状态, 无缺口风险)
            // 维持宽松跳池; 翻页中途失败升级为整次中止 (见 fetchAborted 声明处注释)。
            if (!netOk || resView.empty()) {
                printf("  [错误] 网络请求失败、响应不完整或 Token 已失效。\n");
                if (poolFetchedCount > 0) fetchAborted = true;
                break;
            }

            std::string_view codeStr = ExtractJsonValue(resView, "code", false);
            if (codeStr.empty()) {
                printf("  [错误] 接口返回了非 JSON 数据或格式异常。\n");
                if (poolFetchedCount > 0) fetchAborted = true;
                break;
            }
            if (codeStr != "0") {
                auto msgStr = ExtractJsonValue(resView, "msg", true);
                printf("  [提示] 接口返回信息: %.*s\n", (int)msgStr.size(), msgStr.data());
                if (poolFetchedCount > 0) fetchAborted = true;
                break;
            }

            long long lastSeqParsed = 0;
            ForEachJsonObject(resView, "list", [&](std::string_view itemStr) {
                if (reachedExisting) return;

                std::string_view rawSeqIdStr = ExtractJsonValue(itemStr, "seqId", true);
                if (rawSeqIdStr.empty()) return;

                long long rawSeqId = 0;
                std::from_chars(rawSeqIdStr.data(), rawSeqIdStr.data() + rawSeqIdStr.size(), rawSeqId);
                lastSeqParsed = rawSeqId;

                // v0.1.3.3: 取反改无符号形式 —— 直接 -rawSeqId 在 rawSeqId==LLONG_MIN 时是
                // 有符号溢出 UB。该值来自服务器正序列号, 实际不可达, 属零成本加固
                // (与分析器 abs_ll 口径对齐); 无符号模运算取反 + 补码窄化全程有定义。
                long long safeUniqueId = poolCfg.isWeapon
                    ? (long long)(0ULL - (unsigned long long)rawSeqId) : rawSeqId;

                // 去重与防缺口的判定【对抽卡和非抽卡事件一视同仁】(v0.1.5.0):
                //   两者共用同一套 seqId 序列, 都要能触发"触达本地老记录"的停止条件,
                //   否则事件行会被反复重新拉取。分类放在这些检查【之后】。
                if (local_safe_ids.contains(safeUniqueId)) {
                    reachedExisting = true;
                    printf("  * 触达本地老记录 (ID: %lld),停止追溯。\n", rawSeqId);
                    return;
                }
                if (sessionIds.contains(safeUniqueId)) {
                    printf("\n  [警告] 遇到重复数据 (ID: %lld),防死循环中止。\n", rawSeqId);
                    hasMore = false;
                    // v0.1.3.3: 重复数据 = 分页游标异常 (服务器返回未推进)。已吃进部分新
                    // 记录时与下方未拉取的历史之间存在缺口, 同样升级为整次中止。
                    if (poolFetchedCount > 0) fetchAborted = true;
                    return;
                }
                sessionIds.insert(safeUniqueId);

                long long parsed_ts = 0;
                std::string_view tsStr = ExtractJsonValue(itemStr, "gachaTs", true);
                if (!tsStr.empty()) {
                    std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), parsed_ts);
                }

                // ---- 抽卡 / 非抽卡事件 的分流 (v0.1.5.0) ----
                // 用【正向判据】而不是"kind == gift_intel_book"的黑名单: 已知的非抽卡 kind
                // 目前只有寻访情报书一种, 但官方还有 240 抽的 UP 干员信物、武器申领累计
                // 10/18 次的补充武库箱等发放节点, 它们会不会也进这个接口尚无证据。
                // 白名单写法让任何未知的新 kind 自动落到事件通道, 而不是等到有人发现
                // 统计数字不对才去补黑名单。
                //   条件一: kind 缺失 (老记录本来就没这个字段) 或等于 "draw"
                //   条件二: 物品 id 与稀有度都在 —— 真实抽卡必然两者俱全, 这道保险能兜住
                //           "服务器某个版本/区服没下发 kind" 的情况
                std::string_view kindStr   = ExtractJsonValue(itemStr, "kind", true);
                std::string_view rarityStr = ExtractJsonValue(itemStr, "rarity", false);
                std::string_view itemIdStr = poolCfg.isWeapon
                    ? ExtractJsonValue(itemStr, "weaponId", true)
                    : ExtractJsonValue(itemStr, "charId",   true);
                const bool kindSaysPull = kindStr.empty() || kindStr == "draw";

                if (!kindSaysPull || itemIdStr.empty() || rarityStr.empty()) {
                    NonPullEvent ev;
                    ev.safe_id   = safeUniqueId;
                    ev.timestamp = parsed_ts;
                    ev.raw       = itemStr;          // 原样保留整个服务器对象
                    events.push_back(ev);
                    poolFetchedCount++;              // 计入本池已吃进的条数 (缺口保护同样适用)
                    std::string_view label = ExtractJsonValue(itemStr, "nameText", true);
                    printf("  获取到(非抽卡事件): %.*s [kind=%.*s]\n",
                           (int)label.size(),   label.data(),
                           (int)kindStr.size(), kindStr.data());
                    return;
                }

                ExportRecord rec;
                rec.safe_id    = safeUniqueId;
                rec.timestamp  = parsed_ts;
                rec.poolId     = ExtractJsonValue(itemStr, "poolId", true);
                rec.rank_type  = rarityStr;
                rec.poolName   = ExtractJsonValue(itemStr, "poolName", true);
                rec.isNew      = (uint8_t)(ExtractJsonValue(itemStr, "isNew", false)  == "true" ? 1 : 0);
                rec.isFree     = (uint8_t)(ExtractJsonValue(itemStr, "isFree", false) == "true" ? 1 : 0);

                if (poolCfg.isWeapon) {
                    rec.item_id    = itemIdStr;
                    rec.name       = ExtractJsonValue(itemStr, "weaponName", true);
                    rec.item_type  = ItemType::Weapon;
                    rec.weaponType = ExtractJsonValue(itemStr, "weaponType", true);
                } else {
                    rec.item_id    = itemIdStr;
                    rec.name       = ExtractJsonValue(itemStr, "charName", true);
                    rec.item_type  = ItemType::Character;
                    rec.weaponType = {};
                }

                records.push_back(std::move(rec));
                poolFetchedCount++;
                printf("  获取到: %.*s (%.*s 星) [%.*s]\n",
                    (int)records.back().name.size(),      records.back().name.data(),
                    (int)records.back().rank_type.size(), records.back().rank_type.data(),
                    (int)records.back().poolName.size(),  records.back().poolName.data());
            });

            if (reachedExisting || !hasMore) break;

            nextSeqIdCursor = lastSeqParsed;
            hasMore = (ExtractJsonValue(resView, "hasMore", false) == "true");
            page++;
            Sleep(300);
        }
        if (fetchAborted) {
            printf(">>> [%s] 拉取在翻页中途异常停止。\n", poolCfg.displayName.c_str());
            break;   // 后续池无需再拉, 本次整体不写盘
        }
        printf(">>> [%s] 抓取完成,本次新增拉取: %d 条。\n",
               poolCfg.displayName.c_str(), poolFetchedCount);
        Sleep(500);
    }

    if (fetchAborted) {
        printf("\n========================================\n");
        printf("本次拉取在翻页中途异常停止: 为避免写入带缺口的记录历史, 本次【不写盘】,\n");
        printf("原记录文件保持原样。请稍后重新运行以完整拉取。\n");
        system("pause");
        return 1;
    }

    printf("\n========================================\n");
    printf("已完成全部抓取!总计新增拉取了 %zu 条记录。\n", sessionIds.size());

    // AoS 直接排序 —— 终末地特有规则:
    //   1. 先分区:角色(id 正) 在前,武器(id 负) 在后
    //   2. 再按时间升序
    //   3. 再按 |id| 升序(同一秒内的多抽)
    // 与 gui.cpp 同步: 防 LLONG_MIN 取负的有符号溢出 (UB) —— 用无符号求绝对值, 排序语义不变。
    auto abs_ll = [](long long v) -> unsigned long long {
        return v < 0 ? (0ULL - static_cast<unsigned long long>(v))
                     : static_cast<unsigned long long>(v);
    };
    std::ranges::sort(records, [&](const ExportRecord& a, const ExportRecord& b) {
        bool isWepA = a.safe_id < 0;
        bool isWepB = b.safe_id < 0;
        if (isWepA != isWepB) return isWepA < isWepB;
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return abs_ll(a.safe_id) < abs_ll(b.safe_id);
    });

    time_t rawtime; time(&rawtime);
    long long export_ts = (long long)rawtime;

    // 安全写入:tmp → 替换
    std::string tempFilename = uigfFilename + ".tmp";
    HANDLE hOut = CreateFileA(tempFilename.c_str(), GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hOut != INVALID_HANDLE_VALUE) {
        bool writeOk = false;   // 写出是否全部成功; 失败则不替换原文件 (避免用半截 tmp 覆盖好文件)
        {
            BufferedWriter w(hOut);
            char numBuf[32];

            // ==========================================================
            // UIGF v4.2 输出
            // ----------------------------------------------------------
            // 文档地址: https://uigf.org/standards/UIGF.html
            //
            // 终末地不在 UIGF 官方支持的游戏列表里(米哈游系: hk4e/hkrpg/nap/hk4e_ugc),
            // 但 v4.2 schema 顶层用 "properties" 而非 "additionalProperties: false",
            // 允许新增自定义游戏 key。我们用 "endfield" 作为终末地的容器。
            //
            // 顶层结构:
            //   { "info": { ... v4.2 公共字段 ... },
            //     "endfield": [ { "uid", "timezone", "lang", "list": [ ... ] } ],
            //     "non_pull_events": [ ... ]   // v0.1.5.0 新增, 仅在非空时出现
            //   }
            //
            // "non_pull_events" 是本工具的扩展键, 不属于 UIGF 标准, 也【不应】被当作抽卡
            // 记录读取。UIGF 标准本身没有规定非抽卡事件该放哪里 (它只定义抽卡记录的
            // schema), 这里选择独立键而非塞进 list, 是为了让 list 对所有第三方 UIGF
            // 工具保持"每一条都是一次抽卡"的语义。详见 NonPullEvent 的说明。
            //
            // 注意: v4.2 info 不再含 uid/lang/uigf_version,而是:
            //   - export_timestamp / export_app / export_app_version (必需)
            //   - version: "v4.2" (替代 uigf_version)
            // uid/lang 下沉到游戏数组的元素里。
            //
            // 自定义业务字段(API 原始信息保留)统一改为 snake_case:
            //   gacha_ts / pool_name / weapon_type / is_new / is_free
            // (例外: non_pull_events[].raw 里是服务器原始对象, 保持其原有的 camelCase,
            //  因为那一段是原样透传, 不做任何改写)
            // ==========================================================

            time_t t = export_ts;
            struct tm tm_info;
            localtime_s(&tm_info, &t);
            char tbuf[64];
            int tlen = wsprintfA(tbuf, "%04d-%02d-%02d %02d:%02d:%02d",
                                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

            // ---- info 块 ----
            w.WriteLit("{\n    \"info\": {\n");
            w.WriteLit("        \"export_timestamp\": ");
            auto [ptr, ec] = std::to_chars(numBuf, numBuf + 32, export_ts);
            w.Write(numBuf, (DWORD)(ptr - numBuf));
            w.WriteLit(",\n");
            w.WriteLit("        \"export_app\": \"Endfield Exporter\",\n"
                       "        \"export_app_version\": \"v2.8.0\",\n"
                       "        \"version\": \"v4.2\",\n");
            // export_time 不在 v4.2 必需字段里,但保留作为人类可读辅助信息
            w.WriteLit("        \"export_time\": \""); w.Write(tbuf, tlen); w.WriteLit("\"\n    },\n");

            // ---- endfield 数组(单账号 → 单元素) ----
            // timezone 用本地时区偏移(单位:小时)。Windows 上没有 tm_gmtoff,
            // 用 GetTimeZoneInformation 取偏置(Bias 单位是分钟,且符号约定是
            // "UTC = local + Bias",所以东 8 区返回 -480,需要取负再除 60)。
            TIME_ZONE_INFORMATION tzi;
            DWORD tzKind = GetTimeZoneInformation(&tzi);
            LONG biasMinutes = tzi.Bias;
            if (tzKind == TIME_ZONE_ID_DAYLIGHT) biasMinutes += tzi.DaylightBias;
            else if (tzKind == TIME_ZONE_ID_STANDARD) biasMinutes += tzi.StandardBias;
            int tzHours = (int)(-biasMinutes / 60);

            w.WriteLit("    \"endfield\": [\n        {\n");
            w.WriteLit("            \"uid\": \"0\",\n");
            w.WriteLit("            \"timezone\": ");
            auto [tzPtr, tzEc] = std::to_chars(numBuf, numBuf + 32, tzHours);
            w.Write(numBuf, (DWORD)(tzPtr - numBuf));
            w.WriteLit(",\n");
            w.WriteLit("            \"lang\": \"zh-cn\",\n");
            w.WriteLit("            \"list\": [\n");

            const size_t n = records.size();
            for (size_t i = 0; i < n; ++i) {
                const auto& r = records[i];
                w.WriteLit("        {\n");

                // v4.2 标准字段:gacha_type (替代 v3.0 的 uigf_gacha_type)
                w.WriteKV("gacha_type", r.poolId);          w.WriteLit(",\n");
                w.WriteI64KV("id", r.safe_id, true);        w.WriteLit(",\n");
                w.WriteKV("item_id", r.item_id);            w.WriteLit(",\n");
                // v4.2 标准字段:item_name (替代 v3.0 的 name)
                w.WriteKV("item_name", r.name);             w.WriteLit(",\n");
                w.WriteKV("item_type", ItemTypeToStr(r.item_type));
                w.WriteLit(",\n");
                w.WriteKV("rank_type", r.rank_type);        w.WriteLit(",\n");
                w.WriteTimeKV("time", r.timestamp);         w.WriteLit(",\n");
                // 自定义业务字段(snake_case)
                w.WriteI64KV("gacha_ts", r.timestamp, true); w.WriteLit(",\n");

                if (!r.poolName.empty())   { w.WriteKV("pool_name",   r.poolName);   w.WriteLit(",\n"); }
                if (!r.weaponType.empty()) { w.WriteKV("weapon_type", r.weaponType); w.WriteLit(",\n"); }

                w.WriteLit("            \"is_new\": ");
                w.Write(r.isNew ? "true" : "false");
                w.WriteLit(",\n");
                w.WriteLit("            \"is_free\": ");
                w.Write(r.isFree ? "true" : "false");
                w.WriteLit("\n");
                w.WriteLit("        }");
                if (i < n - 1) w.WriteLit(",");
                w.WriteLit("\n");
            }

            // ---- 非抽卡事件 (v0.1.5.0) ----
            // 放在 "endfield" 之后的顶层键。有意【不】混进 list:
            //   list 是 UIGF 定义的抽卡记录数组, 任何读这个文件的第三方工具都会按抽卡来数;
            //   而这些行不是抽卡, 混进去会让不做过滤的工具把保底水位每期多算 1 抽。
            //   放在独立键里, list 对所有 UIGF 工具保持干净, 信息也一条不丢。
            // 每个元素是 { "id", "gacha_ts", "raw" }: 前两个是本工具自用的检索字段
            // (写在前面, 保证 FindJsonKey 的首个匹配一定命中它们), raw 是服务器原始对象,
            // 原样透传 —— 将来出现新的 kind 也不会因为字段没被识别而丢失。
            if (!events.empty()) {
                std::ranges::sort(events, [&](const NonPullEvent& a, const NonPullEvent& b) {
                    if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
                    return abs_ll(a.safe_id) < abs_ll(b.safe_id);
                });
                w.WriteLit("            ]\n        }\n    ],\n");
                w.WriteLit("    \"non_pull_events\": [\n");
                const size_t m = events.size();
                for (size_t i = 0; i < m; ++i) {
                    const auto& ev = events[i];
                    w.WriteLit("        {\n");
                    w.WriteI64KV("id", ev.safe_id, true);         w.WriteLit(",\n");
                    w.WriteI64KV("gacha_ts", ev.timestamp, true); w.WriteLit(",\n");
                    w.WriteLit("            \"raw\": ");
                    w.Write(ev.raw);
                    w.WriteLit("\n        }");
                    if (i < m - 1) w.WriteLit(",");
                    w.WriteLit("\n");
                }
                w.WriteLit("    ]\n}\n");
            } else {
                w.WriteLit("            ]\n        }\n    ]\n}\n");
            }
            w.Flush();              // 显式收尾 flush 并捕获结果 (析构里那次因 pos==0 成 no-op)
            writeOk = w.ok;
        }
        CloseHandle(hOut);

        if (!writeOk) {
            // 写入中途失败 (磁盘满 / IO 错误): 绝不能用半截 tmp 覆盖好的原文件 —— 删掉 tmp, 保留原文件。
            DeleteFileA(tempFilename.c_str());
            printf("写入失败 (磁盘空间不足或 IO 错误)!已保留原记录文件,未做替换。\n");
        } else if (MoveFileExA(tempFilename.c_str(), uigfFilename.c_str(),
                               MOVEFILE_REPLACE_EXISTING)) {
            printf("已成功更新记录并保存至: %s\n", uigfFilename.c_str());
        } else {
            printf("文件覆盖失败!请手动将 %s 重命名为 %s\n",
                   tempFilename.c_str(), uigfFilename.c_str());
        }
    } else {
        printf("临时文件创建失败!请检查目录权限。\n");
    }

    system("pause");
    return 0;
}
