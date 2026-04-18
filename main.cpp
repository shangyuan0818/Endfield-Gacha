// ============================================================
// Endfield Gacha Exporter - UIGF v3.0 / 面向数据 / PMR 栈分配
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

// O(N) 逐字符扫描
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
std::string FetchPath(HINTERNET hConnect, const std::wstring& path) {
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
        char stackBuf[8192];
        DWORD dwSize = 0, dwDownloaded = 0;
        while (true) {
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;
            if (dwSize <= sizeof(stackBuf)) {
                if (!WinHttpReadData(hRequest, stackBuf, dwSize, &dwDownloaded)) break;
                if (dwDownloaded == 0) break;
                response.append(stackBuf, dwDownloaded);
            } else {
                std::vector<char> heapBuf(dwSize);
                if (!WinHttpReadData(hRequest, heapBuf.data(), dwSize, &dwDownloaded)) break;
                if (dwDownloaded == 0) break;
                response.append(heapBuf.data(), dwDownloaded);
            }
        }
    }
    WinHttpCloseHandle(hRequest);
    return response;
}

struct PoolConfig { std::string poolType, displayName; bool isWeapon; };

// ---------------------------------------------------------
// [BufferedWriter - 析构 RAII Flush]
// ---------------------------------------------------------
struct BufferedWriter {
    HANDLE hFile;
    char buf[65536];
    DWORD pos = 0;

    explicit BufferedWriter(HANDLE h) : hFile(h) {}
    ~BufferedWriter() { Flush(); }

    BufferedWriter(const BufferedWriter&) = delete;
    BufferedWriter& operator=(const BufferedWriter&) = delete;

    void Flush() {
        if (pos > 0 && hFile != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(hFile, buf, pos, &written, NULL);
            pos = 0;
        }
    }
    void Write(const char* data, DWORD len) {
        while (len > 0) {
            DWORD space = sizeof(buf) - pos;
            DWORD chunk = (len < space) ? len : space;
            std::memcpy(buf + pos, data, chunk);
            pos += chunk; data += chunk; len -= chunk;
            if (pos == sizeof(buf)) Flush();
        }
    }
    void Write(std::string_view sv) { Write(sv.data(), (DWORD)sv.size()); }

    template<size_t N>
    void WriteLit(const char (&s)[N]) {
        constexpr DWORD len = N - 1;
        if (pos + len > sizeof(buf)) Flush();
        std::memcpy(buf + pos, s, len);
        pos += len;
    }

    void WriteEscaped(std::string_view s) {
        const char* p = s.data();
        const char* end = p + s.size();
        while (p < end) {
            const char* clean = p;
            while (p < end && *p != '"' && *p != '\\') ++p;
            if (p > clean) Write(clean, (DWORD)(p - clean));
            if (p < end) {
                if (*p == '"') WriteLit("\\\"");
                else           WriteLit("\\\\");
                ++p;
            }
        }
    }

    void WriteKV(std::string_view key, std::string_view val) {
        WriteLit("            \"");
        Write(key);
        WriteLit("\": \"");
        WriteEscaped(val);
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

    std::vector<PoolConfig> pools = {
        {"E_CharacterGachaPoolType_Special",  "角色 - 特许寻访", false},
        {"E_CharacterGachaPoolType_Standard", "角色 - 基础寻访", false},
        {"E_CharacterGachaPoolType_Beginner", "角色 - 启程寻访", false},
        {"",                                   "武器 - 全历史记录", true}
    };

    // PMR:栈上 2MB 池
    std::array<std::byte, 2 * 1024 * 1024> stackBuffer;
    std::pmr::monotonic_buffer_resource pool(stackBuffer.data(), stackBuffer.size());
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    // AoS 记录
    std::pmr::vector<ExportRecord> records(alloc);
    records.reserve(10000);

    std::deque<std::string> networkPayloads;

    // 去重:unordered_set O(1),原版 vector O(n²) 插入
    std::pmr::unordered_set<long long> local_safe_ids(alloc);
    local_safe_ids.reserve(10000);

    std::string uigfFilename = "uigf_endfield.json";

    // ---- 读取本地老记录(读完立即释放句柄,避免锁住目标文件)----
    {
        FileHandle hFile;
        hFile.h = CreateFileA(uigfFilename.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile.h != INVALID_HANDLE_VALUE) {
            DWORD fileSize = GetFileSize(hFile, NULL);
            if (fileSize != INVALID_FILE_SIZE && fileSize > 0) {
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

                        ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
                            std::string_view raw_id = ExtractJsonValue(itemStr, "id", true);
                            long long parsed_id = 0, parsed_ts = 0;
                            if (!raw_id.empty()) {
                                std::from_chars(raw_id.data(), raw_id.data() + raw_id.size(), parsed_id);
                            }
                            std::string_view tsStr = ExtractJsonValue(itemStr, "gachaTs", true);
                            if (!tsStr.empty()) {
                                std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), parsed_ts);
                            }

                            ItemType it = ParseItemType(ExtractJsonValue(itemStr, "item_type", true));

                            records.push_back(ExportRecord{
                                parsed_id,
                                parsed_ts,
                                ExtractJsonValue(itemStr, "uigf_gacha_type", true),
                                ExtractJsonValue(itemStr, "item_id", true),
                                ExtractJsonValue(itemStr, "name", true),
                                it,
                                ExtractJsonValue(itemStr, "rank_type", true),
                                ExtractJsonValue(itemStr, "poolName", true),
                                ExtractJsonValue(itemStr, "weaponType", true),
                                (uint8_t)(ExtractJsonValue(itemStr, "isNew", false)  == "true" ? 1 : 0),
                                (uint8_t)(ExtractJsonValue(itemStr, "isFree", false) == "true" ? 1 : 0)
                            });
                            local_safe_ids.insert(parsed_id);
                        });
                    }
                }
            }
            printf("成功加载本地存储的 %zu 条抽卡记录。\n", records.size());
        } else {
            printf("未发现本地记录,将创建新文件。\n");
        }
    }  // <- Guard 全部析构,文件完全释放

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

            networkPayloads.emplace_back(FetchPath(hConnect, Utf8ToWstring(currentPath)));
            std::string_view resView = networkPayloads.back();

            if (resView.empty()) {
                printf("  [错误] 网络请求失败或 Token 已失效。\n");
                break;
            }

            std::string_view codeStr = ExtractJsonValue(resView, "code", false);
            if (codeStr.empty()) {
                printf("  [错误] 接口返回了非 JSON 数据或格式异常。\n");
                break;
            }
            if (codeStr != "0") {
                auto msgStr = ExtractJsonValue(resView, "msg", true);
                printf("  [提示] 接口返回信息: %.*s\n", (int)msgStr.size(), msgStr.data());
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
                long long safeUniqueId = poolCfg.isWeapon ? -rawSeqId : rawSeqId;

                if (local_safe_ids.contains(safeUniqueId)) {
                    reachedExisting = true;
                    printf("  * 触达本地老记录 (ID: %lld),停止追溯。\n", rawSeqId);
                    return;
                }
                if (sessionIds.contains(safeUniqueId)) {
                    printf("\n  [警告] 遇到重复数据 (ID: %lld),防死循环中止。\n", rawSeqId);
                    hasMore = false;
                    return;
                }
                sessionIds.insert(safeUniqueId);

                long long parsed_ts = 0;
                std::string_view tsStr = ExtractJsonValue(itemStr, "gachaTs", true);
                if (!tsStr.empty()) {
                    std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), parsed_ts);
                }

                ExportRecord rec;
                rec.safe_id    = safeUniqueId;
                rec.timestamp  = parsed_ts;
                rec.poolId     = ExtractJsonValue(itemStr, "poolId", true);
                rec.rank_type  = ExtractJsonValue(itemStr, "rarity", false);
                rec.poolName   = ExtractJsonValue(itemStr, "poolName", true);
                rec.isNew      = (uint8_t)(ExtractJsonValue(itemStr, "isNew", false)  == "true" ? 1 : 0);
                rec.isFree     = (uint8_t)(ExtractJsonValue(itemStr, "isFree", false) == "true" ? 1 : 0);

                if (poolCfg.isWeapon) {
                    rec.item_id    = ExtractJsonValue(itemStr, "weaponId",   true);
                    rec.name       = ExtractJsonValue(itemStr, "weaponName", true);
                    rec.item_type  = ItemType::Weapon;
                    rec.weaponType = ExtractJsonValue(itemStr, "weaponType", true);
                } else {
                    rec.item_id    = ExtractJsonValue(itemStr, "charId",   true);
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
        printf(">>> [%s] 抓取完成,本次新增拉取: %d 条。\n",
               poolCfg.displayName.c_str(), poolFetchedCount);
        Sleep(500);
    }

    printf("\n========================================\n");
    printf("已完成全部抓取!总计新增拉取了 %zu 条记录。\n", sessionIds.size());

    // AoS 直接排序 —— 终末地特有规则:
    //   1. 先分区:角色(id 正) 在前,武器(id 负) 在后
    //   2. 再按时间升序
    //   3. 再按 |id| 升序(同一秒内的多抽)
    auto abs_ll = [](long long v) { return v < 0 ? -v : v; };
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
        {
            BufferedWriter w(hOut);
            char numBuf[32];

            time_t t = export_ts;
            struct tm tm_info;
            localtime_s(&tm_info, &t);
            char tbuf[64];
            int tlen = wsprintfA(tbuf, "%04d-%02d-%02d %02d:%02d:%02d",
                                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

            w.WriteLit("{\n    \"info\": {\n");
            w.WriteLit("        \"uid\": \"0\",\n        \"lang\": \"zh-cn\",\n");
            w.WriteLit("        \"export_time\": \""); w.Write(tbuf, tlen); w.WriteLit("\",\n");
            w.WriteLit("        \"export_timestamp\": ");
            auto [ptr, ec] = std::to_chars(numBuf, numBuf + 32, export_ts);
            w.Write(numBuf, (DWORD)(ptr - numBuf));
            w.WriteLit(",\n");
            w.WriteLit("        \"export_app\": \"Endfield Exporter\",\n"
                       "        \"export_app_version\": \"v2.4.0\",\n"
                       "        \"uigf_version\": \"v3.0\"\n    },\n");
            w.WriteLit("    \"list\": [\n");

            const size_t n = records.size();
            for (size_t i = 0; i < n; ++i) {
                const auto& r = records[i];
                w.WriteLit("        {\n");

                w.WriteKV("uigf_gacha_type", r.poolId);     w.WriteLit(",\n");
                w.WriteI64KV("id", r.safe_id, true);        w.WriteLit(",\n");
                w.WriteKV("item_id", r.item_id);            w.WriteLit(",\n");
                w.WriteKV("name", r.name);                  w.WriteLit(",\n");
                w.WriteKV("item_type", ItemTypeToStr(r.item_type));
                w.WriteLit(",\n");
                w.WriteKV("rank_type", r.rank_type);        w.WriteLit(",\n");
                w.WriteTimeKV("time", r.timestamp);         w.WriteLit(",\n");
                w.WriteI64KV("gachaTs", r.timestamp, true); w.WriteLit(",\n");

                if (!r.poolName.empty())   { w.WriteKV("poolName",   r.poolName);   w.WriteLit(",\n"); }
                if (!r.weaponType.empty()) { w.WriteKV("weaponType", r.weaponType); w.WriteLit(",\n"); }

                w.WriteLit("            \"isNew\": ");
                w.Write(r.isNew ? "true" : "false");
                w.WriteLit(",\n");
                w.WriteLit("            \"isFree\": ");
                w.Write(r.isFree ? "true" : "false");
                w.WriteLit("\n");
                w.WriteLit("        }");
                if (i < n - 1) w.WriteLit(",");
                w.WriteLit("\n");
            }

            w.WriteLit("    ]\n}\n");
            // BufferedWriter 析构自动 Flush
        }
        CloseHandle(hOut);

        if (MoveFileExA(tempFilename.c_str(), uigfFilename.c_str(),
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
