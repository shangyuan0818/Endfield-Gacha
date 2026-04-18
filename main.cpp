#include <cstdio>
#include <cstdlib>
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

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "User32.lib")

// ---------------------------------------------------------
// [枚举与模糊解析层]
// ---------------------------------------------------------
enum class ItemType : uint8_t { Unknown = 0, Character, Weapon };
enum class RankType : uint8_t { Unknown = 0, Rank3 = 3, Rank4 = 4, Rank5 = 5, Rank6 = 6 };

ItemType ParseItemType(std::string_view sv) {
    std::string lower_sv; lower_sv.reserve(sv.size());
    for(char c : sv) lower_sv.push_back((char)std::tolower((unsigned char)c));
    if (lower_sv.find("character") != std::string::npos) return ItemType::Character;
    if (lower_sv.find("weapon") != std::string::npos) return ItemType::Weapon;
    return ItemType::Unknown;
}

std::string_view ItemTypeToStr(ItemType type) {
    if (type == ItemType::Character) return "Character";
    if (type == ItemType::Weapon) return "Weapon";
    return "Unknown";
}

// ---------------------------------------------------------
// [极简 JSON 解析模块]
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
        while (endPos < source.length() && source[endPos] != ',' && source[endPos] != '}' && source[endPos] != ']' && source[endPos] != ' ' && source[endPos] != '\n' && source[endPos] != '\r') endPos++;
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

std::wstring Utf8ToWstring(std::string_view str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

std::string_view ExtractUrlParam(std::string_view url, std::string_view key) {
    size_t pos = url.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.length();
    size_t end = url.find('&', pos);
    return (end == std::string_view::npos) ? url.substr(pos) : url.substr(pos, end - pos);
}

// ---------------------------------------------------------
// [数据结构层：面向数据设计 (SoA)]
// ---------------------------------------------------------
struct ExportDataSoA {
    std::pmr::vector<long long> original_ids;
    std::pmr::vector<long long> safe_ids; 
    std::pmr::vector<long long> timestamps;
    std::pmr::vector<std::string_view> poolIds;
    std::pmr::vector<std::string_view> item_ids;
    std::pmr::vector<std::string_view> names;
    std::pmr::vector<ItemType> item_types;
    std::pmr::vector<std::string_view> rank_types; 
    std::pmr::vector<std::string_view> poolNames;
    std::pmr::vector<std::string_view> weaponTypes;
    std::pmr::vector<uint8_t> isNew;
    std::pmr::vector<uint8_t> isFree;

    explicit ExportDataSoA(std::pmr::polymorphic_allocator<std::byte> alloc)
        : original_ids(alloc), safe_ids(alloc), timestamps(alloc), poolIds(alloc), item_ids(alloc),
          names(alloc), item_types(alloc), rank_types(alloc), poolNames(alloc), weaponTypes(alloc),
          isNew(alloc), isFree(alloc) {}

    void reserve(size_t cap) {
        original_ids.reserve(cap); safe_ids.reserve(cap); timestamps.reserve(cap);
        poolIds.reserve(cap); item_ids.reserve(cap); names.reserve(cap);
        item_types.reserve(cap); rank_types.reserve(cap); poolNames.reserve(cap);
        weaponTypes.reserve(cap); isNew.reserve(cap); isFree.reserve(cap);
    }
};

// 恢复使用 std::string 并放在 std::deque 中，彻底解决内存搬运导致的悬垂指针
std::string FetchPath(HINTERNET hConnect, const std::wstring& path) {
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    std::string response;
    if (hRequest) {
        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD dwSize = 0, dwDownloaded = 0;
            char stackBuf[8192]; 
            do {
                WinHttpQueryDataAvailable(hRequest, &dwSize);
                if (dwSize == 0) break;
                if (dwSize <= sizeof(stackBuf)) {
                    if (WinHttpReadData(hRequest, stackBuf, dwSize, &dwDownloaded)) response.append(stackBuf, dwDownloaded);
                } else {
                    std::vector<char> heapBuf(dwSize);
                    if (WinHttpReadData(hRequest, heapBuf.data(), dwSize, &dwDownloaded)) response.append(heapBuf.data(), dwDownloaded);
                }
            } while (dwSize > 0);
        }
        WinHttpCloseHandle(hRequest);
    }
    return response;
}

struct PoolConfig { std::string poolType, displayName; bool isWeapon; };

// ---------------------------------------------------------
// 缓冲写入器与延迟时间渲染
// ---------------------------------------------------------
struct BufferedWriter {
    HANDLE hFile;
    char buf[65536];
    DWORD pos = 0;
    
    void Flush() { if (pos > 0) { DWORD written; WriteFile(hFile, buf, pos, &written, NULL); pos = 0; } }
    void Write(const char* data, DWORD len) {
        while (len > 0) {
            DWORD space = sizeof(buf) - pos; DWORD chunk = (len < space) ? len : space;
            CopyMemory(buf + pos, data, chunk);
            pos += chunk; data += chunk; len -= chunk;
            if (pos == sizeof(buf)) Flush();
        }
    }
    void Write(std::string_view sv) { Write(sv.data(), (DWORD)sv.size()); }
    
    void WriteEscaped(std::string_view s) {
        const char* p = s.data(); const char* end = p + s.size();
        while (p < end) {
            const char* clean = p;
            while (p < end && *p != '"' && *p != '\\') ++p;
            if (p > clean) Write(clean, (DWORD)(p - clean)); 
            if (p < end) {
                if (*p == '"') Write("\\\"", 2);
                else if (*p == '\\') Write("\\\\", 2);
                ++p;
            }
        }
    }
    
    void WriteKV(std::string_view key, std::string_view val) {
        Write("            \"", 13); Write(key); Write("\": \"", 4);
        WriteEscaped(val); Write("\"", 1);
    }

    void WriteTimeKV(std::string_view key, long long ms_ts) {
        time_t t = ms_ts / 1000; struct tm tm_info; localtime_s(&tm_info, &t);
        char tbuf[64];
        int len = wsprintfA(tbuf, "%04d-%02d-%02d %02d:%02d:%02d", tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        Write("            \"", 13); Write(key); Write("\": \"", 4);
        Write(tbuf, len); Write("\"", 1);
    }
    
    void WriteI64KV(std::string_view key, long long val, bool quotes) {
        char nbuf[32]; auto [ptr, ec] = std::to_chars(nbuf, nbuf + 32, val);
        Write("            \"", 13); Write(key); Write("\": ", 3);
        if (quotes) Write("\"", 1);
        Write(nbuf, (DWORD)(ptr - nbuf));
        if (quotes) Write("\"", 1);
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    char urlBuffer[1024];
    printf("请输入您的终末地抽卡记录完整链接 (https://ef-webview.gryphline.com/api/record/<参数>):\n> ");
    if (!fgets(urlBuffer, sizeof(urlBuffer), stdin)) return 1;
    
    std::string_view inputUrl(urlBuffer);
    while (!inputUrl.empty() && (inputUrl.back() == ' ' || inputUrl.back() == '\n' || inputUrl.back() == '\r' || inputUrl.back() == '\t'))
        inputUrl.remove_suffix(1);

    auto token = ExtractUrlParam(inputUrl, "token=");
    if (token.empty()) { printf("错误: 无法提取 token。\n"); system("pause"); return 1; }
    
    auto serverId = ExtractUrlParam(inputUrl, "server_id=");
    if (serverId.empty()) serverId = "1";
    printf("\n已自动识别 Server ID: %.*s\n", (int)serverId.size(), serverId.data());

    std::vector<PoolConfig> pools = {
        {"E_CharacterGachaPoolType_Special", "角色 - 特许寻访", false},
        {"E_CharacterGachaPoolType_Standard", "角色 - 基础寻访", false},
        {"E_CharacterGachaPoolType_Beginner", "角色 - 启程寻访", false},
        {"", "武器 - 全历史记录", true}
    };
    
    // PMR：在 Stack 上开辟 2MB 内存池
    std::array<std::byte, 2 * 1024 * 1024> stackBuffer;
    std::pmr::monotonic_buffer_resource pool(stackBuffer.data(), stackBuffer.size());
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    ExportDataSoA pulls(alloc);
    pulls.reserve(10000); 

    // 核心修复：使用 std::deque 彻底告别扩容时的内存指针失效
    std::deque<std::string> networkPayloads;

    std::pmr::vector<long long> local_safe_ids(&pool);
    local_safe_ids.reserve(10000);

    std::string uigfFilename = "uigf_endfield.json";
    HANDLE hFile = CreateFileA(uigfFilename.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    const char* mapData = nullptr;
    HANDLE hMap = NULL;

    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD fileSize = GetFileSize(hFile, NULL);
        if (fileSize != INVALID_FILE_SIZE && fileSize > 0) {
            hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
            if (hMap) {
                mapData = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                if (mapData) {
                    std::string_view bufferView(mapData, fileSize);
                    // 核心修复：恢复对 UTF-8 BOM 的剔除，防止本地文件读取彻底失败
                    if (bufferView.size() >= 3 && (unsigned char)bufferView[0] == 0xEF && (unsigned char)bufferView[1] == 0xBB && (unsigned char)bufferView[2] == 0xBF) {
                        bufferView.remove_prefix(3);
                    }
                    
                    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
                        std::string_view raw_id = ExtractJsonValue(itemStr, "id", true);
                        long long parsed_id = 0, parsed_ts = 0;
                        if (!raw_id.empty()) std::from_chars(raw_id.data(), raw_id.data() + raw_id.size(), parsed_id);
                        
                        std::string_view tsStr = ExtractJsonValue(itemStr, "gachaTs", true);
                        if (!tsStr.empty()) std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), parsed_ts);
                        
                        ItemType it = ParseItemType(ExtractJsonValue(itemStr, "item_type", true));
                        
                        pulls.original_ids.push_back(parsed_id);
                        pulls.safe_ids.push_back(parsed_id);
                        pulls.timestamps.push_back(parsed_ts);
                        pulls.poolIds.push_back(ExtractJsonValue(itemStr, "uigf_gacha_type", true));
                        pulls.item_ids.push_back(ExtractJsonValue(itemStr, "item_id", true));
                        pulls.names.push_back(ExtractJsonValue(itemStr, "name", true));
                        pulls.item_types.push_back(it);
                        pulls.rank_types.push_back(ExtractJsonValue(itemStr, "rank_type", true));
                        pulls.poolNames.push_back(ExtractJsonValue(itemStr, "poolName", true));
                        pulls.weaponTypes.push_back(ExtractJsonValue(itemStr, "weaponType", true));
                        pulls.isNew.push_back(ExtractJsonValue(itemStr, "isNew", false) == "true" ? 1 : 0);
                        pulls.isFree.push_back(ExtractJsonValue(itemStr, "isFree", false) == "true" ? 1 : 0);

                        local_safe_ids.push_back(parsed_id);
                    });
                }
            }
        }
        printf("成功加载本地存储的 %zu 条抽卡记录。\n", pulls.original_ids.size());
    } else {
        printf("未发现本地记录，将创建新文件。\n");
    }

    // 核心修复：坚决不能用 abs_ll 查重！恢复标准升序排序，武器和角色在物理内存上彻底隔离
    std::ranges::sort(local_safe_ids);

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

    HINTERNET hSession = WinHttpOpen(L"Endfield Gacha Tool", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = hSession ? WinHttpConnect(hSession, hostName.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    if (!hConnect) { printf("网络初始化失败！\n"); system("pause"); return 1; }

    std::pmr::vector<long long> sessionIds(&pool); 
    sessionIds.reserve(2000);

    std::string tokenStr(token);
    std::string serverIdStr(serverId);

    for (const auto& poolCfg : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", poolCfg.displayName.c_str());
        bool hasMore = true, reachedExisting = false;
        long long nextSeqIdCursor = 0; 
        int page = 1, poolFetchedCount = 0;
        char seqIdBuf[32];

        while (hasMore && !reachedExisting) {
            std::string currentPath = poolCfg.isWeapon 
                ? "/api/record/weapon?lang=zh-cn&token=" + tokenStr + "&server_id=" + serverIdStr
                : "/api/record/char?lang=zh-cn&pool_type=" + poolCfg.poolType + "&token=" + tokenStr + "&server_id=" + serverIdStr;
            if (page > 1 && nextSeqIdCursor > 0) {
                auto [ptr, ec] = std::to_chars(seqIdBuf, seqIdBuf + 32, nextSeqIdCursor);
                currentPath.append("&seq_id=").append(seqIdBuf, ptr - seqIdBuf);
            }

            networkPayloads.emplace_back(FetchPath(hConnect, Utf8ToWstring(currentPath)));
            std::string_view resView = networkPayloads.back();

            if (resView.empty()) { printf("  [错误] 网络请求失败或 Token 已失效。\n"); break; }

            std::string_view codeStr = ExtractJsonValue(resView, "code", false);
            if (codeStr.empty()) { printf("  [错误] 接口返回了非 JSON 数据或格式异常。\n"); break; }
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
                
                // 核心修复：纯粹利用自然二分查找，不再受绝对值干扰
                auto it = std::ranges::lower_bound(local_safe_ids, safeUniqueId);
                if (it != local_safe_ids.end() && *it == safeUniqueId) {
                    reachedExisting = true;
                    printf("  * 触达本地老记录 (ID: %lld)，停止追溯。\n", rawSeqId);
                    return;
                }
                
                auto s_it = std::ranges::lower_bound(sessionIds, safeUniqueId);
                if (s_it != sessionIds.end() && *s_it == safeUniqueId) {
                    printf("\n  [警告] 遇到重复数据 (ID: %lld)，防死循环中止。\n", rawSeqId);
                    hasMore = false; return;
                }
                sessionIds.insert(s_it, safeUniqueId); 

                long long parsed_ts = 0;
                std::string_view tsStr = ExtractJsonValue(itemStr, "gachaTs", true);
                if (!tsStr.empty()) std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), parsed_ts);

                pulls.original_ids.push_back(safeUniqueId);
                pulls.safe_ids.push_back(safeUniqueId);
                pulls.timestamps.push_back(parsed_ts);
                pulls.poolIds.push_back(ExtractJsonValue(itemStr, "poolId", true));
                pulls.rank_types.push_back(ExtractJsonValue(itemStr, "rarity", false));
                pulls.poolNames.push_back(ExtractJsonValue(itemStr, "poolName", true));
                pulls.isNew.push_back(ExtractJsonValue(itemStr, "isNew", false) == "true" ? 1 : 0);
                pulls.isFree.push_back(ExtractJsonValue(itemStr, "isFree", false) == "true" ? 1 : 0);

                if (poolCfg.isWeapon) {
                    pulls.item_ids.push_back(ExtractJsonValue(itemStr, "weaponId", true));
                    pulls.names.push_back(ExtractJsonValue(itemStr, "weaponName", true));
                    pulls.item_types.push_back(ItemType::Weapon);
                    pulls.weaponTypes.push_back(ExtractJsonValue(itemStr, "weaponType", true));
                } else {
                    pulls.item_ids.push_back(ExtractJsonValue(itemStr, "charId", true));
                    pulls.names.push_back(ExtractJsonValue(itemStr, "charName", true));
                    pulls.item_types.push_back(ItemType::Character);
                    pulls.weaponTypes.push_back({}); 
                }
                
                poolFetchedCount++;
                printf("  获取到: %.*s (%.*s 星) [%.*s]\n", 
                    (int)pulls.names.back().size(), pulls.names.back().data(), 
                    (int)pulls.rank_types.back().size(), pulls.rank_types.back().data(), 
                    (int)pulls.poolNames.back().size(), pulls.poolNames.back().data());
            });

            if (reachedExisting || !hasMore) break;
            
            nextSeqIdCursor = lastSeqParsed;
            hasMore = (ExtractJsonValue(resView, "hasMore", false) == "true");
            page++;
            Sleep(300); 
        }
        printf(">>> [%s] 抓取完成，本次新增拉取: %d 条。\n", poolCfg.displayName.c_str(), poolFetchedCount);
        Sleep(500);
    }

    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    printf("\n========================================\n");
    printf("已完成全部抓取！总计新增拉取了 %zu 条记录。\n", sessionIds.size());

    // 创建索引数组进行排序
    auto abs_ll = [](long long v) { return v < 0 ? -v : v; };
    std::pmr::vector<size_t> indices(pulls.original_ids.size(), &pool);
    std::iota(indices.begin(), indices.end(), 0);
    std::ranges::sort(indices, [&](size_t a, size_t b) {
        bool isWepA = pulls.safe_ids[a] < 0; bool isWepB = pulls.safe_ids[b] < 0;
        if (isWepA != isWepB) return isWepA < isWepB; 
        if (pulls.timestamps[a] != pulls.timestamps[b]) return pulls.timestamps[a] < pulls.timestamps[b];
        return abs_ll(pulls.safe_ids[a]) < abs_ll(pulls.safe_ids[b]);
    });

    time_t rawtime; time(&rawtime); long long export_ts = (long long)rawtime;

    // 安全写入机制
    std::string tempFilename = uigfFilename + ".tmp";
    HANDLE hOut = CreateFileA(tempFilename.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    
    if (hOut != INVALID_HANDLE_VALUE) {
        BufferedWriter w{hOut}; char numBuf[32];
        
        time_t t = export_ts; struct tm tm_info; localtime_s(&tm_info, &t);
        char tbuf[64];
        int tlen = wsprintfA(tbuf, "%04d-%02d-%02d %02d:%02d:%02d", 
                             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday, 
                             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

        w.Write("{\n    \"info\": {\n");
        w.Write("        \"uid\": \"0\",\n        \"lang\": \"zh-cn\",\n");
        w.Write("        \"export_time\": \""); w.Write(tbuf, tlen); w.Write("\",\n");
        w.Write("        \"export_timestamp\": "); 
        auto [ptr, ec] = std::to_chars(numBuf, numBuf + 32, export_ts);
        w.Write(numBuf, (DWORD)(ptr - numBuf)); w.Write(",\n");
        w.Write("        \"export_app\": \"Endfield Exporter\",\n        \"export_app_version\": \"v2.4.0 (DoD Core)\",\n        \"uigf_version\": \"v3.0\"\n    },\n");
        w.Write("    \"list\": [\n");

        for (size_t i = 0; i < indices.size(); ++i) {
            size_t idx = indices[i];
            w.Write("        {\n");
            
            w.WriteKV("uigf_gacha_type", pulls.poolIds[idx]); w.Write(",\n");
            w.WriteI64KV("id", pulls.original_ids[idx], true); w.Write(",\n");
            w.WriteKV("item_id", pulls.item_ids[idx]); w.Write(",\n");
            w.WriteKV("name", pulls.names[idx]); w.Write(",\n");
            w.WriteKV("item_type", ItemTypeToStr(pulls.item_types[idx])); w.Write(",\n");
            w.WriteKV("rank_type", pulls.rank_types[idx]); w.Write(",\n");
            w.WriteTimeKV("time", pulls.timestamps[idx]); w.Write(",\n");
            w.WriteI64KV("gachaTs", pulls.timestamps[idx], true); w.Write(",\n");
            
            if (!pulls.poolNames[idx].empty()) { w.WriteKV("poolName", pulls.poolNames[idx]); w.Write(",\n"); }
            if (!pulls.weaponTypes[idx].empty()) { w.WriteKV("weaponType", pulls.weaponTypes[idx]); w.Write(",\n"); }
            
            w.Write("            \"isNew\": "); w.Write(pulls.isNew[idx] ? "true" : "false"); w.Write(",\n");
            w.Write("            \"isFree\": "); w.Write(pulls.isFree[idx] ? "true" : "false"); w.Write("\n");
            w.Write("        }");
            
            if (i < indices.size() - 1) w.Write(",");
            w.Write("\n");
        }
        
        w.Write("    ]\n}\n");
        w.Flush(); CloseHandle(hOut);
        
        // 核心修复：临时文件落盘后，安全解除老文件锁
        if (mapData) UnmapViewOfFile(mapData);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
        mapData = nullptr; hMap = NULL; hFile = INVALID_HANDLE_VALUE;

        // 最后覆盖替换
        if (MoveFileExA(tempFilename.c_str(), uigfFilename.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            printf("已成功更新记录并保存至: %s\n", uigfFilename.c_str());
        } else {
            printf("文件覆盖失败！请手动将 %s 重命名为 %s\n", tempFilename.c_str(), uigfFilename.c_str());
        }
    } else {
        printf("临时文件创建失败！请检查目录权限。\n");
        if (mapData) UnmapViewOfFile(mapData);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    }

    system("pause");
    return 0;
}
