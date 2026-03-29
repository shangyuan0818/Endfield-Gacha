#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

//#include <nlohmann/json.hpp>
#include "json.hpp"

using json = nlohmann::json;

std::wstring Utf8ToWstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

std::string MsToTimeString(long long ms) {
    auto duration = std::chrono::milliseconds(ms);
    std::chrono::system_clock::time_point tp(duration);
    auto t_c = std::chrono::system_clock::to_time_t(tp);
    std::tm tm;
    localtime_s(&tm, &t_c);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
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
    std::string uigf_gacha_type;
    std::string id;         
    std::string item_id;    
    std::string name;       
    std::string item_type;  
    std::string rank_type;  
    std::string time;       
    std::string gachaTs;    
    bool isNew = false;
    bool isFree = false;
    
    std::string poolName;   
    std::string weaponType; 

    // 性能优化：预留纯数字变量
    long long parsed_id = 0;
    long long parsed_ts = 0;
};

void to_json(json& j, const UIGFItem& p) {
    j = json{
        {"uigf_gacha_type", p.uigf_gacha_type}, {"id", p.id}, {"item_id", p.item_id},
        {"name", p.name}, {"item_type", p.item_type}, {"rank_type", p.rank_type},
        {"time", p.time}, {"gachaTs", p.gachaTs}, {"isNew", p.isNew}, {"isFree", p.isFree}
    };
    if (!p.poolName.empty()) j["poolName"] = p.poolName;
    if (!p.weaponType.empty()) j["weaponType"] = p.weaponType;
}

void from_json(const json& j, UIGFItem& p) {
    j.at("uigf_gacha_type").get_to(p.uigf_gacha_type); j.at("id").get_to(p.id);
    j.at("item_id").get_to(p.item_id); j.at("name").get_to(p.name);
    j.at("item_type").get_to(p.item_type); j.at("rank_type").get_to(p.rank_type);
    j.at("time").get_to(p.time);
    if (j.contains("gachaTs")) p.gachaTs = j.at("gachaTs").get<std::string>();
    if (j.contains("isNew")) p.isNew = j.at("isNew").get<bool>();
    if (j.contains("isFree")) p.isFree = j.at("isFree").get<bool>();
    if (j.contains("poolName")) p.poolName = j.at("poolName").get<std::string>();
    if (j.contains("weaponType")) p.weaponType = j.at("weaponType").get<std::string>();
}

// 性能优化：复用 hConnect 句柄
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

struct PoolConfig {
    std::string poolType;
    std::string displayName;
    bool isWeapon;
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    std::string inputUrl;
    std::cout << "请输入您的终末地抽卡记录链接:\n> ";
    std::getline(std::cin, inputUrl);

    std::string token = ExtractToken(inputUrl);
    if (token.empty()) {
        std::cerr << "错误: 无法提取 token。\n"; system("pause"); return 1;
    }
    
    std::string serverId = ExtractServerId(inputUrl);
    std::cout << "\n已自动识别 Server ID: " << serverId << "\n";

    std::vector<PoolConfig> pools = {
        {"E_CharacterGachaPoolType_Special", "角色 - 特许寻访", false},
        {"E_CharacterGachaPoolType_Standard", "角色 - 基础寻访", false},
        {"E_CharacterGachaPoolType_Beginner", "角色 - 启程寻访", false},
        {"", "武器 - 全历史记录", true}
    };
    
    std::string uigfFilename = "uigf_endfield.json";
    
    // 性能优化：使用 long long 纯数字键极速防重
    std::unordered_map<long long, UIGFItem> localRecordsDict;
    std::unordered_map<long long, UIGFItem> sessionRecordsDict;
    json uigfRoot;

    if (std::filesystem::exists(uigfFilename)) {
        try {
            std::ifstream ifs(uigfFilename);
            ifs >> uigfRoot;
            ifs.close();
            for (const auto& item : uigfRoot["list"]) {
                UIGFItem uItem = item.get<UIGFItem>();
                
                uItem.parsed_id = uItem.id.empty() ? 0 : std::stoll(uItem.id);
                uItem.parsed_ts = uItem.gachaTs.empty() ? 0 : std::stoll(uItem.gachaTs);
                
                localRecordsDict[uItem.parsed_id] = uItem;
            }
            std::cout << "成功加载本地存储的 " << localRecordsDict.size() << " 条抽卡记录。\n";
        } catch (...) { std::cerr << "本地文件读取警告，将重新创建。\n"; }
    }

    std::cout << "\n========================================\n";
    std::cout << "        开始向服务器拉取抽卡数据\n";
    std::cout << "========================================\n";

    HINTERNET hSession = WinHttpOpen(L"Endfield Gacha Tool/2.3", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = hSession ? WinHttpConnect(hSession, L"ef-webview.gryphline.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;

    if (!hConnect) {
        std::cerr << "网络初始化失败！\n"; system("pause"); return 1;
    }

    for (const auto& pool : pools) {
        std::cout << "\n>>> 正在抓取 [" << pool.displayName << "] ...\n";
        
        bool hasMore = true;
        bool reachedExisting = false;
        long long nextSeqIdCursor = 0; 
        int page = 1;
        int poolFetchedCount = 0;

        while (hasMore && !reachedExisting) {
            std::string currentPath;
            if (pool.isWeapon) {
                currentPath = "/api/record/weapon?lang=zh-cn&token=" + token + "&server_id=" + serverId;
            } else {
                currentPath = "/api/record/char?lang=zh-cn&pool_type=" + pool.poolType + "&token=" + token + "&server_id=" + serverId;
            }
            if (page > 1 && nextSeqIdCursor > 0) {
                currentPath += "&seq_id=" + std::to_string(nextSeqIdCursor);
            }

            std::string resStr = FetchPath(hConnect, Utf8ToWstring(currentPath));
            if (resStr.empty()) {
                std::cerr << "  [错误] 网络请求失败或 Token 已失效。\n";
                break;
            }

            json resObj;
            try { resObj = json::parse(resStr); } catch (...) { 
                std::cerr << "  [错误] 接口返回了非 JSON 数据。\n"; break; 
            }
            if (resObj["code"].get<int>() != 0) {
                std::cout << "  [提示] 接口返回信息: " << resObj["msg"].get<std::string>() << "\n";
                break;
            }

            auto& listObj = resObj["data"]["list"];
            if (listObj.empty()) break; 

            for (auto& item : listObj) {
                std::string rawSeqIdStr = item["seqId"].get<std::string>();
                long long rawSeqIdNum = std::stoll(rawSeqIdStr);

                long long safeUniqueId = pool.isWeapon ? -rawSeqIdNum : rawSeqIdNum;
                std::string safeUniqueIdStr = std::to_string(safeUniqueId);
                
                if (localRecordsDict.contains(safeUniqueId)) {
                    reachedExisting = true;
                    std::cout << "  * 触达本地老记录 (ID: " << rawSeqIdStr << ")，停止追溯。\n";
                    break;
                }

                if (sessionRecordsDict.contains(safeUniqueId)) {
                    std::cerr << "\n  [警告] 遇到重复数据 (ID: " << rawSeqIdStr << ")，防死循环中止。\n";
                    hasMore = false; 
                    break;
                }

                UIGFItem uItem;
                uItem.uigf_gacha_type = item["poolId"].get<std::string>();
                uItem.id = safeUniqueIdStr; 
                uItem.parsed_id = safeUniqueId; 
                uItem.rank_type = std::to_string(item["rarity"].get<int>());
                
                if (item.contains("poolName")) {
                    uItem.poolName = item["poolName"].get<std::string>();
                }
                
                if (pool.isWeapon) {
                    uItem.item_id = item["weaponId"].get<std::string>();
                    uItem.name = item["weaponName"].get<std::string>();
                    uItem.item_type = "Weapon";
                    if (item.contains("weaponType")) {
                        uItem.weaponType = item["weaponType"].get<std::string>();
                    }
                } else {
                    uItem.item_id = item["charId"].get<std::string>();
                    uItem.name = item["charName"].get<std::string>();
                    uItem.item_type = "Character";
                }
                
                std::string tsStr = item["gachaTs"].get<std::string>();
                uItem.time = MsToTimeString(std::stoll(tsStr)); 
                uItem.gachaTs = tsStr;
                uItem.parsed_ts = std::stoll(tsStr); 
                
                uItem.isNew = item["isNew"].get<bool>();
                if (item.contains("isFree")) uItem.isFree = item["isFree"].get<bool>();

                sessionRecordsDict[safeUniqueId] = uItem;
                poolFetchedCount++;
                
                std::cout << "  获取到: " << uItem.name << " (" << uItem.rank_type << " 星) [" << uItem.poolName << "] - " << uItem.time << "\n";
            }

            if (reachedExisting || !hasMore) break;

            std::string lastRawSeqIdStr = listObj.back()["seqId"].get<std::string>();
            nextSeqIdCursor = std::stoll(lastRawSeqIdStr);

            hasMore = resObj["data"]["hasMore"].get<bool>();
            page++;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        
        std::cout << ">>> [" << pool.displayName << "] 抓取完成，本次新增拉取: " << poolFetchedCount << " 条。\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    std::cout << "\n========================================\n";
    std::cout << "已完成全部抓取！总计新增拉取了 " << sessionRecordsDict.size() << " 条记录。\n";

    std::vector<UIGFItem> mergedList;
    mergedList.reserve(localRecordsDict.size() + sessionRecordsDict.size());
    for (auto& [id, record] : localRecordsDict) mergedList.push_back(std::move(record));
    for (auto& [id, record] : sessionRecordsDict) mergedList.push_back(std::move(record));

    std::ranges::sort(mergedList, [](const UIGFItem& a, const UIGFItem& b) {
        bool isWeaponA = a.parsed_id < 0;
        bool isWeaponB = b.parsed_id < 0;
        
        if (isWeaponA != isWeaponB) {
            return isWeaponA < isWeaponB; 
        }
        if (a.parsed_ts != b.parsed_ts) {
            return a.parsed_ts < b.parsed_ts;
        } 
        return std::abs(a.parsed_id) < std::abs(b.parsed_id);
    });

    auto now = std::chrono::system_clock::now();
    uigfRoot["info"] = {
        {"uid", "0"}, {"lang", "zh-cn"},
        {"export_time", MsToTimeString(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count())},
        {"export_timestamp", std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()},
        {"export_app", "Endfield Exporter C++20"}, {"export_app_version", "v2.3.0"}, {"uigf_version", "v3.0"}
    };
    uigfRoot["list"] = mergedList;

    std::ofstream ofs(uigfFilename);
    if (ofs.is_open()) {
        ofs << uigfRoot.dump(4);
        ofs.close();
        std::cout << "已成功更新记录并保存至: " << uigfFilename << std::endl;
    } else {
        std::cerr << "文件写入失败！请检查目录权限。" << std::endl;
    }

    system("pause");
    return 0;
}
