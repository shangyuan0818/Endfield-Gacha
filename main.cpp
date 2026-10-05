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
#include "JsonScan.h"

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
// [共享 JSON 扫描器与结构化字段读取]
// ---------------------------------------------------------
using JsonValueKind = efjson::ValueKind;
using JsonValueRef = efjson::ValueRef;
using JsonArrayScan = efjson::ArrayScan;
using LocateResult = efjson::LocateResult;

inline JsonValueRef FindTopLevelValue(std::string_view obj, std::string_view key) {
    return efjson::FindMember(obj, key);
}
inline bool ParseFullInt64(std::string_view text, long long& out) {
    return efjson::ParseFullInt64(text, out);
}
template<typename Callback>
[[nodiscard]] JsonArrayScan ForEachObjectInArray(std::string_view text, Callback&& cb) {
    return efjson::ForEachObjectIn(text, std::forward<Callback>(cb));
}

// 字符串和数字都可承载整数; 有字段但解析失败与字段缺失必须分开。
inline bool ReadIntegerField(std::string_view obj, std::string_view key,
                             long long& out, bool& present) {
    const JsonValueRef v = FindTopLevelValue(obj, key);
    present = v.malformed || (v.kind != JsonValueKind::None && v.kind != JsonValueKind::Null);
    if (v.kind != JsonValueKind::String && v.kind != JsonValueKind::Number) return false;
    return ParseFullInt64(v.text, out);
}

// 保留字符串原有转义; 第三方存档使用数字或布尔字段时也不会被静默清空。
inline std::string_view ReadTextField(std::string_view obj, std::string_view key) {
    const JsonValueRef v = FindTopLevelValue(obj, key);
    if (v.kind == JsonValueKind::String || v.kind == JsonValueKind::Number ||
        v.kind == JsonValueKind::Bool) return v.text;
    return {};
}
inline bool FieldAbsentOrEmpty(std::string_view obj, std::string_view key) {
    const JsonValueRef v = FindTopLevelValue(obj, key);
    return v.kind == JsonValueKind::None || v.kind == JsonValueKind::Null ||
           (v.kind == JsonValueKind::String && v.text.empty());
}

struct UigfListLocation {
    JsonArrayScan endfieldScan = JsonArrayScan::NotFound;
    size_t endfieldEntries = 0;
    bool usable = false;
    LocateResult listStatus = LocateResult::NotFound;
    std::string_view listText;
};
// 完整遍历外层账号数组, 避免读取第一个账号后覆盖掉其余账号。
inline UigfListLocation LocateUigfPullList(std::string_view doc) {
    UigfListLocation out;
    const JsonValueRef game = FindTopLevelValue(doc, "endfield");
    if (game.kind != JsonValueKind::Array) {
        out.endfieldScan = JsonArrayScan::Malformed;
        return out;
    }
    out.endfieldScan = ForEachObjectInArray(game.text,
        [&out](std::string_view) { ++out.endfieldEntries; });
    out.usable = out.endfieldScan == JsonArrayScan::Ok && out.endfieldEntries == 1;
    if (!out.usable) return out;
    const JsonValueRef entry = efjson::FirstElement(game.text);
    const JsonValueRef list = FindTopLevelValue(entry.text, "list");
    if (list.kind == JsonValueKind::Array) {
        out.listStatus = LocateResult::Located;
        out.listText = list.text;
    } else if (list.malformed || (list.kind != JsonValueKind::None && list.kind != JsonValueKind::Null)) {
        out.listStatus = LocateResult::Malformed;
    }
    return out;
}

struct PageEnvelope {
    bool complete = false;
    bool codeFound = false;
    std::string_view code, msg;
    LocateResult listStatus = LocateResult::NotFound;
    std::string_view listText;
    bool hasMoreKnown = false, hasMoreValue = false, hasMoreBad = false;
};
// 信封字段只读取对象本层; 记录内的同名 list/hasMore/code 不可代替分页信封。
inline PageEnvelope InspectPageEnvelope(std::string_view doc) {
    PageEnvelope out;
    // 只查结构完整 (括号 / 引号配对闭合, 后面没有别的内容): 这一条就能挡住被截断的正文。
    out.complete = efjson::IsCompleteObjectDocument(doc);
    if (!out.complete) return out;
    const JsonValueRef code = FindTopLevelValue(doc, "code");
    out.codeFound = code.kind == JsonValueKind::String || code.kind == JsonValueKind::Number;
    if (out.codeFound) out.code = code.text;
    out.msg = ReadTextField(doc, "msg");

    std::string_view host = doc;
    const JsonValueRef data = FindTopLevelValue(doc, "data");
    if (data.kind == JsonValueKind::Object) host = data.text;
    else if (data.kind != JsonValueKind::None && data.kind != JsonValueKind::Null) {
        out.listStatus = LocateResult::Malformed;
        return out;
    }
    JsonValueRef list = FindTopLevelValue(host, "list");
    // data 本层没有 list 时兼容根对象本层 list, 不把记录内部的同名键当作分页信封。
    if (host.data() != doc.data() && (list.kind == JsonValueKind::None || list.kind == JsonValueKind::Null)) {
        const JsonValueRef rootList = FindTopLevelValue(doc, "list");
        if (rootList.kind != JsonValueKind::None && rootList.kind != JsonValueKind::Null) {
            host = doc;
            list = rootList;
        }
    }
    if (list.kind == JsonValueKind::Array) {
        out.listStatus = LocateResult::Located;
        out.listText = list.text;
    } else if (list.malformed || (list.kind != JsonValueKind::None && list.kind != JsonValueKind::Null)) {
        out.listStatus = LocateResult::Malformed;
    }
    // 未识别的嵌套记录数组不能当作空池, 也不能读取记录内部的同名键作为信封。
    if (out.listStatus == LocateResult::NotFound) {
        const auto nestedList = efjson::LocateArrayFullText(doc, "list");
        if (nestedList.first != LocateResult::NotFound) out.listStatus = LocateResult::Malformed;
    }
    auto readHasMore = [&out](std::string_view obj) {
        if (out.hasMoreKnown || out.hasMoreBad) return;
        const JsonValueRef flag = FindTopLevelValue(obj, "hasMore");
        if (flag.kind == JsonValueKind::Bool ||
            (flag.kind == JsonValueKind::String && (flag.text == "true" || flag.text == "false"))) {
            out.hasMoreKnown = true;
            out.hasMoreValue = flag.text == "true";
        } else if (flag.malformed || (flag.kind != JsonValueKind::None && flag.kind != JsonValueKind::Null)) {
            out.hasMoreBad = true;
        }
    };
    readHasMore(host);
    if (host.data() != doc.data()) readHasMore(doc);
    return out;
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
// [非抽卡事件]  v0.1.4.1
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
// 时间转换失败返回 -1, 调用方不能读取未初始化的 tm 或写出随机日期。
inline int FormatLocalTime(long long seconds, char* buf, size_t capacity) {
    const time_t t = static_cast<time_t>(seconds);
    if (static_cast<long long>(t) != seconds) return -1;
    struct tm value{};
    if (localtime_s(&value, &t) != 0) return -1;
    const int len = std::snprintf(buf, capacity, "%04d-%02d-%02d %02d:%02d:%02d",
                                  value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
                                  value.tm_hour, value.tm_min, value.tm_sec);
    return len >= 0 && static_cast<size_t>(len) < capacity ? len : -1;
}

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
    void Write(std::string_view sv) {
        // 原文视图可能超过 DWORD 上限, 分块写入以免长度窄化后静默截断。
        while (!sv.empty() && ok) {
            const DWORD len = sv.size() > MAXDWORD ? MAXDWORD : static_cast<DWORD>(sv.size());
            Write(sv.data(), len);
            sv.remove_prefix(len);
        }
    }

    template<size_t N>
    void WriteLit(const char (&s)[N]) {
        if (!ok) return;
        constexpr DWORD len = N - 1;
        if (pos + len > sizeof(buf) && !Flush()) return;
        std::memcpy(buf + pos, s, len);
        pos += len;
    }

    // v0.1.3.3: WriteKV 的 val 全部来自 ReadTextField 的【原始转义形态】视图 (扫描器
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
        char tbuf[64];
        int len = FormatLocalTime(ms_ts / 1000, tbuf, sizeof(tbuf));
        if (len < 0) len = 0;   // 无法表示的时间写成空串, 不因此让整次写盘失败 (与 Apple 端一致)
        WriteLit("            \"");
        Write(key);
        WriteLit("\": \"");
        Write(tbuf, len);
        WriteLit("\"");
    }

    void WriteI64KV(std::string_view key, long long val, bool quotes) {
        char nbuf[32];
        auto [ptr, ec] = std::to_chars(nbuf, nbuf + 32, val);
        if (ec != std::errc{}) { ok = false; return; }
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
    // v0.1.4.1 新增 E_CharacterGachaPoolType_Rerun (重构寻访 RE-Factor Headhunting):
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
    size_t migratedLegacy = 0;   // 从旧版 list 里迁出的非抽卡事件条数 (仅用于提示)

    std::string uigfFilename = "uigf_endfield.json";

    // ---- 读取本地老记录(读完立即释放句柄,避免锁住目标文件)----
    // v0.1.3.3 (A2) 基底验收口径: 文件【不存在】= 全新拉取 (正常); 文件存在但打不开 /
    // 0 字节 / 映射失败 / 找不到 "list" 数组结构 = 按损坏处理, 中止且不写盘, 防止运行
    // 结束时 MoveFileEx 覆盖原历史; "list" 数组存在但为空 = 结构正确的空数据, 0 条正常继续。
    bool baseFileExists = false;   // 文件存在 (无论能否读)
    bool baseLoadOk     = false;   // 打开、结构完整、单账号结构与字段解析全部通过
    // v0.1.4.1 存档保护: 事件区读坏了同样必须中止, 不能"读不懂就当没有"然后覆盖。
    //   eventsCorrupt 为真 = 文件里【有】non_pull_events 键, 但数组没闭合 (截断) 或存在
    //   无法解析的条目。此时原文件里那些事件是唯一的副本 —— 抽卡记录接口只保留 90 天,
    //   一旦被覆盖就永久丢失。
    bool   eventsCorrupt  = false;
    bool   eventsBadShape = false;   // 键在, 但值不是一个正常闭合、元素全为对象的数组
    size_t eventsMalformed = 0;
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

                        // 存档必须是一个结构完整的 JSON 对象且只有一个账号; 未读懂的任何内容都不回写。
                        if (efjson::IsCompleteObjectDocument(bufferView)) {
                            const UigfListLocation loc = LocateUigfPullList(bufferView);
                            JsonArrayScan pullScan = JsonArrayScan::Malformed;
                            size_t recordsMalformed = 0;
                            if (loc.usable && loc.listStatus == LocateResult::Located) {
                                pullScan = ForEachObjectInArray(loc.listText, [&](std::string_view item) {
                                    long long parsed_id = 0, parsed_ts = 0;
                                    bool idPresent = false, tsPresent = false;
                                    if (!ReadIntegerField(item, "id", parsed_id, idPresent) ||
                                        (!ReadIntegerField(item, "gacha_ts", parsed_ts, tsPresent) && tsPresent)) {
                                        ++recordsMalformed;
                                        return;
                                    }

                                    // 只迁出物品、稀有度和名字均缺失的旧版事件; 数字稀有度不会被判空。
                                    if (FieldAbsentOrEmpty(item, "item_id") &&
                                        FieldAbsentOrEmpty(item, "rank_type") &&
                                        FieldAbsentOrEmpty(item, "item_name")) {
                                        events.push_back(NonPullEvent{parsed_id, parsed_ts, item});
                                        local_safe_ids.insert(parsed_id);
                                        ++migratedLegacy;
                                        return;
                                    }
                                    records.push_back(ExportRecord{
                                        parsed_id, parsed_ts,
                                        ReadTextField(item, "gacha_type"),
                                        ReadTextField(item, "item_id"),
                                        ReadTextField(item, "item_name"),
                                        ParseItemType(ReadTextField(item, "item_type")),
                                        ReadTextField(item, "rank_type"),
                                        ReadTextField(item, "pool_name"),
                                        ReadTextField(item, "weapon_type"),
                                        (uint8_t)(ReadTextField(item, "is_new") == "true" ? 1 : 0),
                                        (uint8_t)(ReadTextField(item, "is_free") == "true" ? 1 : 0)
                                    });
                                    local_safe_ids.insert(parsed_id);
                                });
                            }
                            baseLoadOk = loc.usable && pullScan == JsonArrayScan::Ok && recordsMalformed == 0;

                            // 事件包装字段只读本层, raw 必须是对象 (原样回写)。
                            const JsonValueRef evtV = FindTopLevelValue(bufferView, "non_pull_events");
                            JsonArrayScan eventsScan = JsonArrayScan::NotFound;
                            if (evtV.malformed) {
                                eventsScan = JsonArrayScan::Malformed;
                            } else if (evtV.kind == JsonValueKind::Array) {
                                eventsScan = ForEachObjectInArray(evtV.text, [&](std::string_view item) {
                                    NonPullEvent ev;
                                    bool idPresent = false, tsPresent = false;
                                    const JsonValueRef raw = FindTopLevelValue(item, "raw");
                                    if (!ReadIntegerField(item, "id", ev.safe_id, idPresent) ||
                                        (!ReadIntegerField(item, "gacha_ts", ev.timestamp, tsPresent) && tsPresent) ||
                                        raw.kind != JsonValueKind::Object) {
                                        ++eventsMalformed;
                                        return;
                                    }
                                    ev.raw = raw.text;
                                    events.push_back(ev);
                                    local_safe_ids.insert(ev.safe_id);
                                });
                            } else if (evtV.kind != JsonValueKind::None && evtV.kind != JsonValueKind::Null) {
                                eventsScan = JsonArrayScan::Malformed;
                            }
                            eventsBadShape = eventsScan == JsonArrayScan::Malformed;
                            eventsCorrupt = eventsBadShape || eventsMalformed > 0;
                        }
                    }
                }
            }
            if (baseLoadOk) {
                printf("成功加载本地存储的 %zu 条抽卡记录", records.size());
                if (!events.empty()) printf(" 与 %zu 条非抽卡事件", events.size());
                printf("。\n");
                if (migratedLegacy > 0) {
                    printf("已把 %zu 条误存在抽卡数组里的非抽卡事件迁移到 non_pull_events。\n",
                           migratedLegacy);
                }
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
        printf("[错误] 本地记录文件 %s 存在, 但无法读取, 或结构不是 UIGF v4.2 的\n", uigfFilename.c_str());
        printf("       单账号 endfield[0].list 数组 (结构不完整、多账号或字段无法完整解析)。\n");
        printf("       (0 字节、被占用、已损坏或非本工具格式)。\n");
        printf("       为防止本次运行结束时覆盖原有历史, 已中止。请检查或移走该文件后重试。\n");
        system("pause");
        return 1;
    }

    // v0.1.4.1: 事件区受损与 list 受损同等对待 —— 都中止, 都不写盘。
    //   "读不懂就当没有"在这里是危险的默认: 抽卡记录接口只保留最近 90 天, 本地文件是
    //   这些事件的唯一副本, 一旦按"读到的部分"覆盖回去, 读不出来的那些就永久没了。
    //   宁可让用户看到报错去处理, 也不要静默地少写一段。
    if (eventsCorrupt) {
        printf("[错误] 本地记录文件 %s 的 \"non_pull_events\" 段已损坏:\n", uigfFilename.c_str());
        if (eventsBadShape) {
            printf("       该键的值不是一个正常闭合的对象数组 (被截断、写成了别的类型,\n");
            printf("       或数组里混进了非对象元素)。\n");
        }
        if (eventsMalformed > 0) {
            printf("       有 %zu 条事件的 id / gacha_ts / raw 字段缺失或类型不对而无法解析。\n",
                   eventsMalformed);
        }
        printf("       这些事件在本地文件之外没有副本 (接口只保留最近 90 天),\n");
        printf("       若照常写盘会把读不出来的那部分永久删除, 故已中止、原文件保持原样。\n");
        printf("       请修复或移走该文件后重试; 若确认可以放弃这些事件, 手工删掉\n");
        printf("       \"non_pull_events\" 整段再运行即可 (抽卡记录不受影响)。\n");
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

    // 任一卡池的任何错误都使本次更新作废, 原存档直到完整成功前都不替换。
    bool fetchAborted = false;
    size_t newPulls = 0, newEvents = 0;
    for (const auto& poolCfg : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", poolCfg.displayName.c_str());
        bool hasMore = true, reachedExisting = false;
        long long nextSeqIdCursor = 0;
        int page = 1;
        size_t poolPulls = 0, poolEvents = 0;
        char seqIdBuf[32];
        auto fail = [&](const char* reason) {
            printf("  [错误] %s\n", reason);
            fetchAborted = true;
        };

        while (hasMore && !reachedExisting) {
            std::string currentPath = poolCfg.isWeapon
                ? "/api/record/weapon?lang=zh-cn&token=" + tokenStr + "&server_id=" + serverIdStr
                : "/api/record/char?lang=zh-cn&pool_type=" + poolCfg.poolType
                    + "&token=" + tokenStr + "&server_id=" + serverIdStr;
            if (page > 1 && nextSeqIdCursor > 0) {
                auto [ptr, ec] = std::to_chars(seqIdBuf, seqIdBuf + 32, nextSeqIdCursor);
                if (ec != std::errc{}) { fail("无法生成分页游标。"); break; }
                currentPath.append("&seq_id=").append(seqIdBuf, ptr - seqIdBuf);
            }

            bool netOk = false;
            networkPayloads.emplace_back(FetchPath(hConnect, Utf8ToWstring(currentPath), netOk));
            std::string_view resView = networkPayloads.back();
            if (!netOk || resView.empty()) {
                fail("网络请求失败、响应不完整或 Token 已失效。");
                break;
            }
            if (resView.size() >= 3 && (uint8_t)resView[0] == 0xEF &&
                (uint8_t)resView[1] == 0xBB && (uint8_t)resView[2] == 0xBF)
                resView.remove_prefix(3);
            const PageEnvelope env = InspectPageEnvelope(resView);
            if (!env.complete) {
                fail("接口正文不是完整的 JSON 对象 (多半是传输被截断)。");
                break;
            }
            if (!env.codeFound || env.code != "0") {
                printf("  [接口信息] %.*s\n", (int)env.msg.size(), env.msg.data());
                fail("响应缺少业务码或接口报告业务错误。");
                break;
            }
            if (env.listStatus == LocateResult::Malformed || env.hasMoreBad) {
                fail("接口记录数组或 hasMore 字段类型异常。");
                break;
            }

            long long lastSeqParsed = 0;
            size_t itemsSeen = 0;
            JsonArrayScan pageScan = JsonArrayScan::NotFound;
            if (env.listStatus == LocateResult::Located) {
                pageScan = ForEachObjectInArray(env.listText, [&](std::string_view item) {
                    if (fetchAborted) return;
                    ++itemsSeen;
                    long long rawSeqId = 0, parsed_ts = 0;
                    bool seqPresent = false, tsPresent = false;
                    if (!ReadIntegerField(item, "seqId", rawSeqId, seqPresent) || rawSeqId <= 0) {
                        fail("记录的 seqId 缺失或不是完整的正整数。");
                        return;
                    }
                    if (!ReadIntegerField(item, "gachaTs", parsed_ts, tsPresent) && tsPresent) {
                        fail("记录的时间戳不是完整的整数。");
                        return;
                    }
                    lastSeqParsed = rawSeqId;
                    // 找到本地边界后仍校验本页余下记录的 seqId / gachaTs, 任意错误都不能按成功收尾。
                    if (reachedExisting) return;
                    const long long safeUniqueId = poolCfg.isWeapon ? -rawSeqId : rawSeqId;
                    if (local_safe_ids.contains(safeUniqueId)) {
                        reachedExisting = true;
                        printf("  * 触达本地老记录 (ID: %lld),停止追溯。\n", rawSeqId);
                        return;
                    }
                    if (sessionIds.contains(safeUniqueId)) {
                        fail("同一会话遇到重复 seqId, 分页可能未前进。");
                        return;
                    }
                    sessionIds.insert(safeUniqueId);

                    // 物品 id 与稀有度决定是否是抽卡; 未知 kind 只提示, 不改变分类。
                    const std::string_view kindStr = ReadTextField(item, "kind");
                    const std::string_view rarityStr = ReadTextField(item, "rarity");
                    const std::string_view itemIdStr = ReadTextField(item, poolCfg.isWeapon ? "weaponId" : "charId");
                    if (itemIdStr.empty() || rarityStr.empty()) {
                        events.push_back(NonPullEvent{safeUniqueId, parsed_ts, item});
                        ++poolEvents; ++newEvents;
                        const std::string_view label = ReadTextField(item, "nameText");
                        printf("  获取到(非抽卡事件): %.*s [kind=%.*s]\n",
                               (int)label.size(), label.data(), (int)kindStr.size(), kindStr.data());
                        return;
                    }
                    if (!kindStr.empty() && kindStr != "draw")
                        printf("  [提示] 未见过的 kind=%.*s, 按完整物品字段作为抽卡处理。\n",
                               (int)kindStr.size(), kindStr.data());

                    ExportRecord rec{};
                    rec.safe_id = safeUniqueId;
                    rec.timestamp = parsed_ts;
                    rec.poolId = ReadTextField(item, "poolId");
                    rec.rank_type = rarityStr;
                    rec.poolName = ReadTextField(item, "poolName");
                    rec.isNew = (uint8_t)(ReadTextField(item, "isNew") == "true" ? 1 : 0);
                    rec.isFree = (uint8_t)(ReadTextField(item, "isFree") == "true" ? 1 : 0);
                    rec.item_id = itemIdStr;
                    rec.name = ReadTextField(item, poolCfg.isWeapon ? "weaponName" : "charName");
                    rec.item_type = poolCfg.isWeapon ? ItemType::Weapon : ItemType::Character;
                    if (poolCfg.isWeapon) rec.weaponType = ReadTextField(item, "weaponType");
                    records.push_back(rec);
                    ++poolPulls; ++newPulls;
                    printf("  获取到: %.*s (%.*s 星) [%.*s]\n",
                           (int)rec.name.size(), rec.name.data(),
                           (int)rec.rank_type.size(), rec.rank_type.data(),
                           (int)rec.poolName.size(), rec.poolName.data());
                });
            }
            if (fetchAborted) break;
            if (pageScan == JsonArrayScan::Malformed) {
                fail("接口记录数组结构异常 (含非对象元素或分隔符异常)。");
                break;
            }
            if (env.listStatus == LocateResult::NotFound &&
                (page > 1 || poolPulls + poolEvents > 0)) {
                fail("翻页中途返回的这一页缺少记录数组。");
                break;
            }
            if (itemsSeen > 0 && !env.hasMoreKnown) {
                fail("非空记录页缺少可用的 hasMore, 无法判断是否还有历史。");
                break;
            }
            if (itemsSeen == 0 && env.hasMoreKnown && env.hasMoreValue) {
                fail("接口称仍有历史, 却返回空页。");
                break;
            }
            if (itemsSeen == 0) break;  // 合法空池/空数组和零新增都是正常完成。
            if (lastSeqParsed <= 0 ||
                (page > 1 && lastSeqParsed >= nextSeqIdCursor)) {
                fail("分页游标没有严格递减。");
                break;
            }
            if (reachedExisting || !env.hasMoreValue) break;
            nextSeqIdCursor = lastSeqParsed;
            hasMore = env.hasMoreValue;
            ++page;
            Sleep(300);
        }
        if (fetchAborted) {
            printf(">>> [%s] 抓取失败。\n", poolCfg.displayName.c_str());
            break;
        }
        printf(">>> [%s] 抓取完成,本次新增: %zu 条抽卡记录, %zu 条非抽卡事件。\n",
               poolCfg.displayName.c_str(), poolPulls, poolEvents);
        Sleep(500);
    }

    if (fetchAborted) {
        printf("\n========================================\n");
        printf("本次拉取发生错误: 已放弃整次更新, 本次【不写盘】,原记录文件保持原样。\n");
        printf("请处理错误或稍后重试, 以完整拉取。\n");
        system("pause");
        return 1;
    }
    printf("\n========================================\n");
    printf("已完成全部抓取!新增 %zu 条抽卡记录和 %zu 条非抽卡事件; 共计 %zu 条抽卡记录和 %zu 条事件。\n",
           newPulls, newEvents, records.size(), events.size());

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

    time_t rawtime = time(nullptr);
    char exportTime[64];
    int exportTimeLen = FormatLocalTime(static_cast<long long>(rawtime), exportTime, sizeof(exportTime));
    if (exportTimeLen < 0) exportTimeLen = 0;   // 转换不了就把 export_time 写成空串, 照常写盘 (与 Apple 端一致)
    const long long export_ts = static_cast<long long>(rawtime);
    bool exportOk = false;

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
            //     "non_pull_events": [ ... ]   // v0.1.4.1 新增, 仅在非空时出现
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

            // ---- info 块 ----
            w.WriteLit("{\n    \"info\": {\n");
            w.WriteLit("        \"export_timestamp\": ");
            auto [ptr, ec] = std::to_chars(numBuf, numBuf + 32, export_ts);
            w.Write(numBuf, (DWORD)(ptr - numBuf));
            w.WriteLit(",\n");
            w.WriteLit("        \"export_app\": \"Endfield Exporter\",\n"
                       "        \"export_app_version\": \"v2.7.0\",\n"
                       "        \"version\": \"v4.2\",\n");
            // export_time 不在 v4.2 必需字段里,但保留作为人类可读辅助信息
            w.WriteLit("        \"export_time\": \""); w.Write(exportTime, (DWORD)exportTimeLen); w.WriteLit("\"\n    },\n");

            // ---- endfield 数组(单账号 → 单元素) ----
            // timezone 用本地时区偏移(单位:小时)。Windows 上没有 tm_gmtoff,
            // 用 GetTimeZoneInformation 取偏置(Bias 单位是分钟,且符号约定是
            // "UTC = local + Bias",所以东 8 区返回 -480,需要取负再除 60)。
            TIME_ZONE_INFORMATION tzi{};
            DWORD tzKind = GetTimeZoneInformation(&tzi);
            LONG biasMinutes = tzi.Bias;   // 取不到时区时 tzi 保持全零, timezone 写 0 (与 Apple 端一致)
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

            // ---- 非抽卡事件 (v0.1.4.1) ----
            // 放在 "endfield" 之后的顶层键。有意【不】混进 list:
            //   list 是 UIGF 定义的抽卡记录数组, 任何读这个文件的第三方工具都会按抽卡来数;
            //   而这些行不是抽卡, 混进去会让不做过滤的工具把保底水位每期多算 1 抽。
            //   放在独立键里, list 对所有 UIGF 工具保持干净, 信息也一条不丢。
            // 每个元素是 { "id", "gacha_ts", "raw" }: 前两个是本工具自用的检索字段
            // (读取时只认包装对象本层字段), raw 是服务器原始对象,
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
        // 缓冲区写完后确认系统落盘与关闭成功, 才可替换原文件。
        if (writeOk && !FlushFileBuffers(hOut)) writeOk = false;
        if (!CloseHandle(hOut)) writeOk = false;

        if (!writeOk) {
            // 写入中途失败 (磁盘满 / IO 错误): 绝不能用半截 tmp 覆盖好的原文件 —— 删掉 tmp, 保留原文件。
            DeleteFileA(tempFilename.c_str());
            printf("写入失败 (磁盘空间不足或 IO 错误)!已保留原记录文件,未做替换。\n");
        } else if (MoveFileExA(tempFilename.c_str(), uigfFilename.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            exportOk = true;
            printf("已成功更新记录并保存至: %s\n", uigfFilename.c_str());
        } else {
            DeleteFileA(tempFilename.c_str());
            printf("文件替换失败!本次更新已放弃, 原记录文件保持原样。\n");
        }
    } else {
        printf("临时文件创建失败!请检查目录权限。\n");
    }

    system("pause");
    return exportOk ? 0 : 1;
}
