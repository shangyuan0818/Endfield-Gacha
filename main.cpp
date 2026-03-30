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
        if (source[pos] != '"') return {};
        pos++; 
        auto endPos = pos;
        while (endPos < source.length() && source[endPos] != '"') {
            if (source[endPos] == '\\') endPos++; 
            endPos++;
        }
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
    
    int depth = 0;
    size_t objStart = 0;
    for (size_t i = pos; i < source.length(); ++i) {
        if (source[i] == '{') {
            if (depth == 0) objStart = i;
            depth++;
        } else if (source[i] == '}') {
            depth--;
            if (depth == 0) cb(source.substr(objStart, i - objStart + 1));
        } else if (source[i] == ']' && depth == 0) break;
    }
}

std::string EscapeJsonStr(std::string_view s) {
    std::string res;
    res.reserve(s.size() + 10);
    for (char c : s) {
        if (c == '"') res += "\\\"";
        else if (c == '\\') res += "\\\\";
        else res += c;
    }
    return res;
}
// ---------------------------------------------------------

std::wstring Utf8ToWstring(std::string_view str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

std::string MsToTimeString(long long ms) {
    time_t t = ms / 1000;
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
        tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    return std::string(buf);
}

std::string ExtractToken(const std::string& url) {
    std::string key = "token=";
    auto pos = url.find(key);
    if (pos == std::string::npos) return "";
    pos += key.length();
    auto endPos = url.find("&", pos);
    if (endPos == std::string::npos) return url.substr(pos);
    return url.substr(pos, endPos - pos);
}

std::string ExtractServerId(const std::string& url) {
    std::string key = "server_id=";
    auto pos = url.find(key);
    if (pos == std::string::npos) return "1"; 
    pos += key.length();
    auto endPos = url.find("&", pos);
    if (endPos == std::string::npos) return url.substr(pos);
    return url.substr(pos, endPos - pos);
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
            do {
                WinHttpQueryDataAvailable(hRequest, &dwSize);
                if (dwSize == 0) break;
                std::vector<char> buffer(dwSize + 1, 0);
                if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
                    response.append(buffer.data(), dwDownloaded);
                }
            } while (dwSize > 0);
        }
        WinHttpCloseHandle(hRequest);
    }
    return response;
}

struct PoolConfig { std::string poolType, displayName; bool isWeapon; };

int main() {
    SetConsoleOutputCP(CP_UTF8);

    char urlBuffer[1024];
    printf("请输入您的终末地抽卡记录链接:\n> ");
    if (!fgets(urlBuffer, sizeof(urlBuffer), stdin)) return 1;
    
    std::string inputUrl(urlBuffer);
    inputUrl.erase(inputUrl.find_last_not_of(" \n\r\t") + 1);

    std::string token = ExtractToken(inputUrl);
    if (token.empty()) {
        printf("错误: 无法提取 token。\n"); system("pause"); return 1;
    }
    
    std::string serverId = ExtractServerId(inputUrl);
    printf("\n已自动识别 Server ID: %s\n", serverId.c_str());

    std::vector<PoolConfig> pools = {
        {"E_CharacterGachaPoolType_Special", "角色 - 特许寻访", false},
        {"E_CharacterGachaPoolType_Standard", "角色 - 基础寻访", false},
        {"E_CharacterGachaPoolType_Beginner", "角色 - 启程寻访", false},
        {"", "武器 - 全历史记录", true}
    };
    
    std::string uigfFilename = "uigf_endfield.json";
    
    // 摒弃 unordered_map，使用 vector 存储，并在查重时使用二分查找
    std::vector<UIGFItem> allRecords;
    std::vector<long long> localIds; 
    
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
                        
                        std::string_view isNewStr = ExtractJsonValue(itemStr, "isNew", false);
                        uItem.isNew = (isNewStr == "true");
                        
                        std::string_view isFreeStr = ExtractJsonValue(itemStr, "isFree", false);
                        uItem.isFree = (isFreeStr == "true");
                        
                        if (!uItem.id.empty()) std::from_chars(uItem.id.data(), uItem.id.data() + uItem.id.size(), uItem.parsed_id);
                        if (!uItem.gachaTs.empty()) std::from_chars(uItem.gachaTs.data(), uItem.gachaTs.data() + uItem.gachaTs.size(), uItem.parsed_ts);
                        
                        localIds.push_back(uItem.parsed_id);
                        allRecords.push_back(std::move(uItem));
                    });
                    UnmapViewOfFile(mapData);
                }
                CloseHandle(hMap);
            }
        }
        CloseHandle(hFile);
        
        // 对本地 ID 排序，以便后续进行 O(log N) 的极速查重
        std::ranges::sort(localIds);
        printf("成功加载本地存储的 %zu 条抽卡记录。\n", allRecords.size());
    } else {
        printf("未发现本地记录，将创建新文件。\n");
    }

    printf("\n========================================\n");
    printf("        开始向服务器拉取抽卡数据\n");
    printf("========================================\n");

    HINTERNET hSession = WinHttpOpen(L"Endfield Gacha Tool", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = hSession ? WinHttpConnect(hSession, L"ef-webview.gryphline.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;

    if (!hConnect) {
        printf("网络初始化失败！\n"); system("pause"); return 1;
    }

    std::unordered_set<long long> sessionIds; // 当前会话的防重击机制，数据量小，保留 unordered_set

    for (const auto& pool : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", pool.displayName.c_str());
        bool hasMore = true, reachedExisting = false;
        long long nextSeqIdCursor = 0; 
        int page = 1, poolFetchedCount = 0;

        while (hasMore && !reachedExisting) {
            std::string currentPath = pool.isWeapon 
                ? "/api/record/weapon?lang=zh-cn&token=" + token + "&server_id=" + serverId
                : "/api/record/char?lang=zh-cn&pool_type=" + pool.poolType + "&token=" + token + "&server_id=" + serverId;
            if (page > 1 && nextSeqIdCursor > 0) currentPath += "&seq_id=" + std::to_string(nextSeqIdCursor);

            std::string resStr = FetchPath(hConnect, Utf8ToWstring(currentPath));
            if (resStr.empty()) { printf("  [错误] 网络请求失败或 Token 已失效。\n"); break; }

            std::string_view resView(resStr);
            std::string_view codeStr = ExtractJsonValue(resView, "code", false);
            if (codeStr.empty()) { printf("  [错误] 接口返回了非 JSON 数据或格式异常。\n"); break; }
            if (codeStr != "0") {
                printf("  [提示] 接口返回信息: %s\n", std::string(ExtractJsonValue(resView, "msg", true)).c_str());
                break;
            }

            long long lastSeqParsed = 0;
            ForEachJsonObject(resView, "list", [&](std::string_view itemStr) {
                if (reachedExisting) return; // 回调中如果已触达底线则跳过剩余解析

                std::string_view rawSeqIdStr = ExtractJsonValue(itemStr, "seqId", true);
                if (rawSeqIdStr.empty()) return;

                long long rawSeqId = 0;
                std::from_chars(rawSeqIdStr.data(), rawSeqIdStr.data() + rawSeqIdStr.size(), rawSeqId);
                lastSeqParsed = rawSeqId;
                long long safeUniqueId = pool.isWeapon ? -rawSeqId : rawSeqId;
                
                // 二分极速查重
                if (std::ranges::binary_search(localIds, safeUniqueId)) {
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
                uItem.id = std::to_string(safeUniqueId); 
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

    // 统一排序
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

    // 流式切片写入 (完美化解写文件内存膨胀)
    HANDLE hOut = CreateFileA(uigfFilename.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut != INVALID_HANDLE_VALUE) {
        auto WriteString = [&](const std::string& s) {
            DWORD bytesWritten;
            WriteFile(hOut, s.data(), (DWORD)s.size(), &bytesWritten, NULL);
        };

        std::string header = "{\n    \"info\": {\n";
        header += "        \"uid\": \"0\",\n        \"lang\": \"zh-cn\",\n";
        header += "        \"export_time\": \"" + export_time + "\",\n";
        header += "        \"export_timestamp\": " + std::to_string(export_ts) + ",\n";
        header += "        \"export_app\": \"Endfield Exporter\",\n        \"export_app_version\": \"v2.3.0\",\n        \"uigf_version\": \"v3.0\"\n    },\n";
        header += "    \"list\": [\n";
        WriteString(header);

        for (size_t i = 0; i < allRecords.size(); ++i) {
            const auto& p = allRecords[i];
            std::string itemStr = "        {\n            \"uigf_gacha_type\": \"" + EscapeJsonStr(p.uigf_gacha_type) + "\",\n";
            itemStr += "            \"id\": \"" + EscapeJsonStr(p.id) + "\",\n            \"item_id\": \"" + EscapeJsonStr(p.item_id) + "\",\n";
            itemStr += "            \"name\": \"" + EscapeJsonStr(p.name) + "\",\n            \"item_type\": \"" + EscapeJsonStr(p.item_type) + "\",\n";
            itemStr += "            \"rank_type\": \"" + EscapeJsonStr(p.rank_type) + "\",\n            \"time\": \"" + EscapeJsonStr(p.time) + "\",\n";
            itemStr += "            \"gachaTs\": \"" + EscapeJsonStr(p.gachaTs) + "\",\n";
            if (!p.poolName.empty()) itemStr += "            \"poolName\": \"" + EscapeJsonStr(p.poolName) + "\",\n";
            if (!p.weaponType.empty()) itemStr += "            \"weaponType\": \"" + EscapeJsonStr(p.weaponType) + "\",\n";
            itemStr += "            \"isNew\": " + std::string(p.isNew ? "true" : "false") + ",\n";
            itemStr += "            \"isFree\": " + std::string(p.isFree ? "true" : "false") + "\n        }";
            
            if (i < allRecords.size() - 1) itemStr += ",";
            itemStr += "\n";
            
            WriteString(itemStr);
        }
        
        WriteString("    ]\n}\n");
        CloseHandle(hOut);
        printf("已成功更新记录并保存至: %s\n", uigfFilename.c_str());
    } else {
        printf("文件写入失败！请检查目录权限。\n");
    }

    system("pause");
    return 0;
}
