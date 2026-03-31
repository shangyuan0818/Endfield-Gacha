#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <ctime>
#include <windows.h>
#include <winhttp.h>
#include <string_view>
#include <charconv>
#include <ranges>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "User32.lib")

// ---------------------------------------------------------
// [极简 JSON 解析模块 - 零分配纯净版]
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
    
    int depth = 0;
    size_t objStart = 0;
    for (size_t i = pos; i < source.length(); ++i) {
        char c = source[i];
        // FIX: 跳过字符串内容，避免值中的 { } 干扰层级计数
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

std::wstring Utf8ToWstring(std::string_view str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

// Win32 wsprintfA 替代 snprintf (减少 CRT 依赖)
std::string MsToTimeString(long long ms) {
    time_t t = ms / 1000;
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    char buf[64];
    wsprintfA(buf, "%04d-%02d-%02d %02d:%02d:%02d",
        tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    return std::string(buf);
}

// charconv 替代 std::to_string (零分配, 栈上完成)
char* I64ToStr(long long val, char* buf) {
    auto [ptr, ec] = std::to_chars(buf, buf + 20, val);
    *ptr = '\0';
    return buf;
}

std::string_view ExtractUrlParam(std::string_view url, std::string_view key) {
    size_t pos = url.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.length();
    size_t end = url.find('&', pos);
    return (end == std::string_view::npos) ? url.substr(pos) : url.substr(pos, end - pos);
}

struct UIGFItem {
    std::string uigf_gacha_type, id, item_id, name, item_type, rank_type, time, gachaTs, poolName, weaponType;
    bool isNew = false, isFree = false;
    long long parsed_id = 0, parsed_ts = 0;
};

std::string FetchPath(HINTERNET hConnect, const std::wstring& path) {
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    std::string response;
    if (hRequest) {
        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD dwSize = 0, dwDownloaded = 0;
            char stackBuf[8192]; // 栈上复用 buffer，避免每次循环 heap 分配
            do {
                WinHttpQueryDataAvailable(hRequest, &dwSize);
                if (dwSize == 0) break;
                if (dwSize <= sizeof(stackBuf)) {
                    if (WinHttpReadData(hRequest, stackBuf, dwSize, &dwDownloaded))
                        response.append(stackBuf, dwDownloaded);
                } else {
                    // 极端情况: 数据块超大，fallback 到堆分配
                    std::vector<char> heapBuf(dwSize);
                    if (WinHttpReadData(hRequest, heapBuf.data(), dwSize, &dwDownloaded))
                        response.append(heapBuf.data(), dwDownloaded);
                }
            } while (dwSize > 0);
        }
        WinHttpCloseHandle(hRequest);
    }
    return response;
}

struct PoolConfig { std::string poolType, displayName; bool isWeapon; };

// ---------------------------------------------------------
// 缓冲写入器: 攒满 buffer 再一次性 WriteFile，减少系统调用
// ---------------------------------------------------------
struct BufferedWriter {
    HANDLE hFile;
    char buf[65536];
    DWORD pos = 0;
    
    void Flush() {
        if (pos > 0) {
            DWORD written;
            WriteFile(hFile, buf, pos, &written, NULL);
            pos = 0;
        }
    }
    void Write(const char* data, DWORD len) {
        while (len > 0) {
            DWORD space = sizeof(buf) - pos;
            DWORD chunk = (len < space) ? len : space;
            CopyMemory(buf + pos, data, chunk);
            pos += chunk; data += chunk; len -= chunk;
            if (pos == sizeof(buf)) Flush();
        }
    }
    // 只需要保留 string_view 的重载即可
    void Write(std::string_view sv) { Write(sv.data(), (DWORD)sv.size()); }
    // void Write(const std::string& s) { Write(s.data(), (DWORD)s.size()); }
    
    // 直接写入 JSON 转义字符串 (避免 EscapeJsonStr 的临时 string 分配)
    void WriteEscaped(std::string_view s) {
        for (char c : s) {
            if (c == '"')       { Write("\\\"", 2); }
            else if (c == '\\') { Write("\\\\", 2); }
            else                { Write(&c, 1); }
        }
    }
    
    // 写入 "key": "escaped_value" 格式
    void WriteKV(std::string_view key, std::string_view val) {
        Write("            \"", 13);
        Write(key);
        Write("\": \"", 4);
        WriteEscaped(val);
        Write("\"", 1);
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    char urlBuffer[1024];
    printf("请输入您的终末地抽卡记录完整链接 (https://ef-webview.gryphline.com/api/record/<参数>):\n> ");
    if (!fgets(urlBuffer, sizeof(urlBuffer), stdin)) return 1;
    
    std::string_view inputUrl(urlBuffer);
    // trim trailing whitespace
    while (!inputUrl.empty() && (inputUrl.back() == ' ' || inputUrl.back() == '\n' || inputUrl.back() == '\r' || inputUrl.back() == '\t'))
        inputUrl.remove_suffix(1);

    auto token = ExtractUrlParam(inputUrl, "token=");
    if (token.empty()) {
        printf("错误: 无法提取 token。\n"); system("pause"); return 1;
    }
    
    auto serverId = ExtractUrlParam(inputUrl, "server_id=");
    if (serverId.empty()) serverId = "1";
    printf("\n已自动识别 Server ID: %.*s\n", (int)serverId.size(), serverId.data());

    std::vector<PoolConfig> pools = {
        {"E_CharacterGachaPoolType_Special", "角色 - 特许寻访", false},
        {"E_CharacterGachaPoolType_Standard", "角色 - 基础寻访", false},
        {"E_CharacterGachaPoolType_Beginner", "角色 - 启程寻访", false},
        {"", "武器 - 全历史记录", true}
    };
    
    std::string uigfFilename = "uigf_endfield.json";
    
    // FIX: 使用 unordered_set 替代 sorted vector + binary_search
    // 查重从 O(logN) 降为 O(1)，且省去 O(NlogN) 排序
    std::vector<UIGFItem> allRecords;
    std::unordered_set<long long> localIds;
    
    HANDLE hFile = CreateFileA(uigfFilename.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD fileSize = GetFileSize(hFile, NULL);
        if (fileSize != INVALID_FILE_SIZE && fileSize > 0) {
            HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
            if (hMap) {
                const char* mapData = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                if (mapData) {
                    std::string_view bufferView(mapData, fileSize);
                    
                    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
                        UIGFItem uItem;
                        uItem.uigf_gacha_type = ExtractJsonValue(itemStr, "uigf_gacha_type", true);
                        uItem.id = ExtractJsonValue(itemStr, "id", true);
                        uItem.item_id = ExtractJsonValue(itemStr, "item_id", true);
                        uItem.name = ExtractJsonValue(itemStr, "name", true);
                        uItem.item_type = ExtractJsonValue(itemStr, "item_type", true);
                        uItem.rank_type = ExtractJsonValue(itemStr, "rank_type", true);
                        uItem.time = ExtractJsonValue(itemStr, "time", true);
                        uItem.gachaTs = ExtractJsonValue(itemStr, "gachaTs", true);
                        uItem.poolName = ExtractJsonValue(itemStr, "poolName", true);
                        uItem.weaponType = ExtractJsonValue(itemStr, "weaponType", true);
                        
                        uItem.isNew = (ExtractJsonValue(itemStr, "isNew", false) == "true");
                        uItem.isFree = (ExtractJsonValue(itemStr, "isFree", false) == "true");
                        
                        if (!uItem.id.empty()) std::from_chars(uItem.id.data(), uItem.id.data() + uItem.id.size(), uItem.parsed_id);
                        if (!uItem.gachaTs.empty()) std::from_chars(uItem.gachaTs.data(), uItem.gachaTs.data() + uItem.gachaTs.size(), uItem.parsed_ts);
                        
                        localIds.insert(uItem.parsed_id);
                        allRecords.push_back(std::move(uItem));
                    });
                    UnmapViewOfFile(mapData);
                }
                CloseHandle(hMap);
            }
        }
        CloseHandle(hFile);
        
        printf("成功加载本地存储的 %zu 条抽卡记录。\n", allRecords.size());
    } else {
        printf("未发现本地记录，将创建新文件。\n");
    }

    // 极简判断：只要 URL 中包含 "hypergryph" 即为国服，否则全部视为国际服
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
    if (!hConnect) {
        printf("网络初始化失败！\n"); system("pause"); return 1;
    }

    std::unordered_set<long long> sessionIds;
    
    // 预构建 token/serverId 的 string 以供路径拼接
    std::string tokenStr(token);
    std::string serverIdStr(serverId);

    for (const auto& pool : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", pool.displayName.c_str());
        bool hasMore = true, reachedExisting = false;
        long long nextSeqIdCursor = 0; 
        int page = 1, poolFetchedCount = 0;
        char seqIdBuf[24];

        while (hasMore && !reachedExisting) {
            std::string currentPath = pool.isWeapon 
                ? "/api/record/weapon?lang=zh-cn&token=" + tokenStr + "&server_id=" + serverIdStr
                : "/api/record/char?lang=zh-cn&pool_type=" + pool.poolType + "&token=" + tokenStr + "&server_id=" + serverIdStr;
            if (page > 1 && nextSeqIdCursor > 0) {
                currentPath += "&seq_id=";
                currentPath += I64ToStr(nextSeqIdCursor, seqIdBuf);
            }

            std::string resStr = FetchPath(hConnect, Utf8ToWstring(currentPath));
            if (resStr.empty()) { printf("  [错误] 网络请求失败或 Token 已失效。\n"); break; }

            std::string_view resView(resStr);
            std::string_view codeStr = ExtractJsonValue(resView, "code", false);
            if (codeStr.empty()) { printf("  [错误] 接口返回了非 JSON 数据或格式异常。\n"); break; }
            if (codeStr != "0") {
                printf("  [提示] 接口返回信息: %.*s\n", (int)ExtractJsonValue(resView, "msg", true).size(), ExtractJsonValue(resView, "msg", true).data());
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
                long long safeUniqueId = pool.isWeapon ? -rawSeqId : rawSeqId;
                
                // O(1) 哈希查重 (替代原 O(logN) 二分)
                if (localIds.contains(safeUniqueId)) {
                    reachedExisting = true;
                    printf("  * 触达本地老记录 (ID: %lld)，停止追溯。\n", rawSeqId);
                    return;
                }
                if (sessionIds.contains(safeUniqueId)) {
                    printf("\n  [警告] 遇到重复数据 (ID: %lld)，防死循环中止。\n", rawSeqId);
                    hasMore = false; return;
                }

                UIGFItem uItem;
                uItem.uigf_gacha_type = ExtractJsonValue(itemStr, "poolId", true);
                
                char idBuf[24];
                uItem.id = I64ToStr(safeUniqueId, idBuf);
                uItem.parsed_id = safeUniqueId; 
                uItem.rank_type = ExtractJsonValue(itemStr, "rarity", false);
                uItem.poolName = ExtractJsonValue(itemStr, "poolName", true);
                
                if (pool.isWeapon) {
                    uItem.item_id = ExtractJsonValue(itemStr, "weaponId", true);
                    uItem.name = ExtractJsonValue(itemStr, "weaponName", true);
                    uItem.item_type = "Weapon";
                    uItem.weaponType = ExtractJsonValue(itemStr, "weaponType", true);
                } else {
                    uItem.item_id = ExtractJsonValue(itemStr, "charId", true);
                    uItem.name = ExtractJsonValue(itemStr, "charName", true);
                    uItem.item_type = "Character";
                }
                
                std::string_view tsStr = ExtractJsonValue(itemStr, "gachaTs", true);
                if (!tsStr.empty()) std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), uItem.parsed_ts);
                
                uItem.time = MsToTimeString(uItem.parsed_ts); 
                uItem.gachaTs = tsStr;
                uItem.isNew = (ExtractJsonValue(itemStr, "isNew", false) == "true");
                uItem.isFree = (ExtractJsonValue(itemStr, "isFree", false) == "true");

                sessionIds.insert(safeUniqueId);
                allRecords.push_back(std::move(uItem));
                poolFetchedCount++;
                printf("  获取到: %s (%s 星) [%s] - %s\n", allRecords.back().name.c_str(), allRecords.back().rank_type.c_str(), allRecords.back().poolName.c_str(), allRecords.back().time.c_str());
            });

            if (reachedExisting || !hasMore) break;
            
            nextSeqIdCursor = lastSeqParsed;
            hasMore = (ExtractJsonValue(resView, "hasMore", false) == "true");
            page++;
            Sleep(300); 
        }
        printf(">>> [%s] 抓取完成，本次新增拉取: %d 条。\n", pool.displayName.c_str(), poolFetchedCount);
        Sleep(500);
    }

    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    printf("\n========================================\n");
    printf("已完成全部抓取！总计新增拉取了 %zu 条记录。\n", sessionIds.size());

    // 统一排序: 角色在前武器在后，同类按时间戳+ID排序
    std::ranges::sort(allRecords, [](const UIGFItem& a, const UIGFItem& b) {
        bool isWeaponA = a.parsed_id < 0, isWeaponB = b.parsed_id < 0;
        if (isWeaponA != isWeaponB) return isWeaponA < isWeaponB; 
        if (a.parsed_ts != b.parsed_ts) return a.parsed_ts < b.parsed_ts;
        return std::abs(a.parsed_id) < std::abs(b.parsed_id);
    });

    time_t rawtime;
    time(&rawtime);
    long long export_ts = (long long)rawtime;
    std::string export_time = MsToTimeString(export_ts * 1000);

    // 缓冲写入器: 攒满64KB再WriteFile，大幅减少系统调用
    HANDLE hOut = CreateFileA(uigfFilename.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut != INVALID_HANDLE_VALUE) {
        BufferedWriter w{hOut};
        char numBuf[24];
        
        w.Write("{\n    \"info\": {\n");
        w.Write("        \"uid\": \"0\",\n        \"lang\": \"zh-cn\",\n");
        w.Write("        \"export_time\": \""); w.Write(export_time); w.Write("\",\n");
        w.Write("        \"export_timestamp\": "); w.Write(I64ToStr(export_ts, numBuf)); w.Write(",\n");
        w.Write("        \"export_app\": \"Endfield Exporter\",\n        \"export_app_version\": \"v2.3.0\",\n        \"uigf_version\": \"v3.0\"\n    },\n");
 w.Write("    \"list\": [\n");

        for (size_t i = 0; i < allRecords.size(); ++i) {
            const auto& p = allRecords[i];
            
            w.Write("        {\n");
            
            // 直接流式写入，零临时 string 分配
            w.WriteKV("uigf_gacha_type", p.uigf_gacha_type); w.Write(",\n");
            w.WriteKV("id", p.id); w.Write(",\n");
            w.WriteKV("item_id", p.item_id); w.Write(",\n");
            w.WriteKV("name", p.name); w.Write(",\n");
            w.WriteKV("item_type", p.item_type); w.Write(",\n");
            w.WriteKV("rank_type", p.rank_type); w.Write(",\n");
            w.WriteKV("time", p.time); w.Write(",\n");
            w.WriteKV("gachaTs", p.gachaTs); w.Write(",\n");
            if (!p.poolName.empty()) { w.WriteKV("poolName", p.poolName); w.Write(",\n"); }
            if (!p.weaponType.empty()) { w.WriteKV("weaponType", p.weaponType); w.Write(",\n"); }
            w.Write("            \"isNew\": "); w.Write(p.isNew ? "true" : "false"); w.Write(",\n");
            w.Write("            \"isFree\": "); w.Write(p.isFree ? "true" : "false"); w.Write("\n");
            w.Write("        }");
            
            if (i < allRecords.size() - 1) w.Write(",");
            w.Write("\n");
        }
        
        w.Write("    ]\n}\n");
        w.Flush();
        CloseHandle(hOut);
        printf("已成功更新记录并保存至: %s\n", uigfFilename.c_str());
    } else {
        printf("文件写入失败！请检查目录权限。\n");
    }

    system("pause");
    return 0;
}
