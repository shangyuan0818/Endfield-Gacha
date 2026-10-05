//
//  JsonScan.h
//  Endfield-Gacha
//
//  轻量 JSON 局部扫描器 (header-only, 无依赖, 无堆分配)。
//
//  为什么使用视图扫描而不是构建完整 JSON 对象树:
//    抽卡存档动辄几万条记录, 全量建对象树会产生几十万次分配; 本工具全程用
//    std::string_view 指向已经在内存里的原始字节 (映射的存档 / 网络响应),
//    字段零额外拷贝。代价是【它不是完整的 JSON 语法校验器】—— 见下方"能力边界"。
//
//  能力边界 (务必先读):
//    * SkipValue 做的是【严格的括号/引号配对】, 能可靠识别"被截断"的文本, 但不校验
//      被跳过的那段内部的语法 (例如 {"a":1 "b":2} 这种成员间缺逗号的对象, 作为
//      【被跳过的值】时会被接受)。
//    * FindMember / ForEachObjectIn 会【逐个成员/元素】前进, 因此它们直接遍历的那一层
//      是严格的: 成员/元素之间必须有逗号、不允许尾逗号、键必须是字符串、数组元素必须
//      是对象。也就是说"我们真正读进来的那些层"是校验过的, 更深的层只保证配对完整。
//    * IsCompleteObjectDocument 用来判断"整段正文是不是一个完整闭合的 JSON 对象,
//      后面只剩空白" —— 这是识别【传输被截断】最可靠的一道闸门。
//
//  与 Apple 分支共享同一套扫描语义。main.cpp / gui.cpp 共用本头文件, 避免导出器
//  按结构路径读取存档、分析器却全文找第一个 "list" 时因成员顺序产生不同结果。
//
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

namespace efjson {

// MARK: - 基本类型

enum class ValueKind : uint8_t { None = 0, String, Number, Object, Array, Bool, Null };

struct ValueRef {
    ValueKind kind = ValueKind::None;
    // 所在对象本身读不下去 (不是对象 / 键没闭合 / 少冒号 / 成员间缺逗号 / 值解析不了)。
    // 必须与"没有这个键"分开: 前者是"这段坏了", 后者是"本来就没有" ——
    // 在存档场景里一个要中止、一个要放行。
    bool malformed = false;
    std::string_view text;   // String: 去掉两端引号的原文(转义未还原); 其余: 值的原文
};

// 遍历一个数组的结果。三态而非 bool: "没有这个键"与"键在但这段坏了"必须分开处理。
enum class ArrayScan : uint8_t {
    NotFound = 0,   // 没有这个键 (或值为 null) —— 旧格式文件 / 空页, 正常
    Ok,             // 值是正常闭合的数组, 元素全是对象 (可以是 0 个)
    Malformed       // 值不是数组 / 数组被截断 / 元素不是对象 / 元素间缺逗号
};

// 只定位不遍历时的结果。
enum class LocateResult : uint8_t { NotFound = 0, Located, Malformed };

// MARK: - 全文找键 (宽松路径)

// 找到【两侧带引号】的 key, 返回起始引号的位置。只做字符串匹配, 不区分嵌套层级 ——
// 只适合一次性的服务器报文; 读本地存档请用 FindMember 走结构路径。
inline size_t FindKeyToken(std::string_view src, std::string_view key, size_t pos = 0) {
    while (true) {
        pos = src.find(key, pos);
        if (pos == std::string_view::npos) return pos;
        if (pos > 0 && src[pos - 1] == '"' &&
            pos + key.size() < src.size() && src[pos + key.size()] == '"')
            return pos - 1;
        pos += key.size();
    }
}

// 全文取一个标量字段的原文。isStr=true 取引号之间的内容 (转义不还原),
// false 取到下一个分隔符为止的字面量。同样只适合服务器报文。
inline std::string_view ExtractValue(std::string_view src, std::string_view key, bool isStr) {
    size_t pos = FindKeyToken(src, key);
    if (pos == std::string_view::npos) return {};
    pos = src.find(':', pos + key.size() + 2);
    if (pos == std::string_view::npos) return {};
    ++pos;
    while (pos < src.size() && (src[pos]==' '||src[pos]=='\t'||src[pos]=='\n'||src[pos]=='\r')) ++pos;
    if (isStr) {
        if (pos >= src.size() || src[pos] != '"') return {};
        ++pos;
        size_t e = pos;
        while (e < src.size() && src[e] != '"') { if (src[e]=='\\' && e+1 < src.size()) e += 2; else ++e; }
        return e < src.size() ? src.substr(pos, e - pos) : std::string_view{};
    }
    size_t e = pos;
    while (e < src.size() && src[e]!=',' && src[e]!='}' && src[e]!=']'
           && src[e]!=' ' && src[e]!='\n' && src[e]!='\r') ++e;
    return src.substr(pos, e - pos);
}

// MARK: - 值扫描

// 从 s[i] 处解析一个 JSON 值, 返回其结束位置(末字符的下一位); 结构不合法返回 npos。
// 括号用位栈严格配对 —— '[' 记 1、'{' 记 0, 闭合时比对, 交叉括号(如 {..])直接判非法。
inline size_t SkipValue(std::string_view s, size_t i, ValueKind& kind) {
    const size_t n = s.size();
    while (i < n && (unsigned char)s[i] <= ' ') ++i;
    if (i >= n) return std::string_view::npos;
    const char c = s[i];
    if (c == '"') {
        for (size_t k = i + 1; k < n; ++k) {
            if (s[k] == '\\') { ++k; continue; }
            if (s[k] == '"') { kind = ValueKind::String; return k + 1; }
        }
        return std::string_view::npos;          // 字符串没闭合
    }
    if (c == '{' || c == '[') {
        uint64_t isArr = 0;                     // bit d: 第 d 层是 '[' 吗
        int depth = 0;
        for (size_t k = i; k < n; ++k) {
            const char d = s[k];
            if (d == '"') {
                size_t q = k + 1;
                for (; q < n; ++q) {
                    if (s[q] == '\\') { ++q; continue; }
                    if (s[q] == '"') break;
                }
                if (q >= n) return std::string_view::npos;
                k = q;
                continue;
            }
            if (d == '{' || d == '[') {
                if (depth >= 64) return std::string_view::npos;   // 嵌套过深, 不冒险
                if (d == '[') isArr |= (1ull << depth); else isArr &= ~(1ull << depth);
                ++depth;
            } else if (d == '}' || d == ']') {
                if (depth == 0) return std::string_view::npos;
                --depth;
                const bool wantArr = ((isArr >> depth) & 1ull) != 0;
                if (wantArr != (d == ']')) return std::string_view::npos;   // 括号交叉
                if (depth == 0) {
                    kind = wantArr ? ValueKind::Array : ValueKind::Object;
                    return k + 1;
                }
            }
        }
        return std::string_view::npos;          // 没闭合 = 被截断
    }
    size_t k = i;
    while (k < n && s[k] != ',' && s[k] != '}' && s[k] != ']' && (unsigned char)s[k] > ' ') ++k;
    if (k == i) return std::string_view::npos;
    const std::string_view lit = s.substr(i, k - i);
    kind = (lit == "true" || lit == "false") ? ValueKind::Bool
         : (lit == "null")                   ? ValueKind::Null
                                             : ValueKind::Number;
    return k;
}

// 整段文本是不是【一个完整闭合的 JSON 对象, 后面只剩空白】。
//
// 这是识别"传输被截断"最可靠的一道闸门: 任何在对象/数组/字符串中途断掉的正文,
// 括号或引号都配不上, 一定返回 false。它【不】保证内部语法完全合法 (见文件头的能力边界),
// 但"半截正文"这一类是它的强项, 而那正是会造成静默记录缺口的那一类。
inline bool IsCompleteObjectDocument(std::string_view s) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n && (unsigned char)s[i] <= ' ') ++i;
    ValueKind k = ValueKind::None;
    const size_t e = SkipValue(s, i, k);
    if (e == std::string_view::npos || k != ValueKind::Object) return false;
    size_t j = e;
    while (j < n && (unsigned char)s[j] <= ' ') ++j;
    return j == n;
}

// MARK: - 结构化读取 (存档必须走这条路径)

// 在【对象 obj 的本层】查找 key。obj 必须是以 '{' 开头的完整对象。
// 三种结果: 命中 (kind 为具体类型) / 没有这个键 (kind==None, malformed==false) /
// 对象结构读不下去 (malformed==true)。后两者必须分开 —— 把"读不出来"当成"没有",
// 正是会造成静默丢数据的那一类默认。
//
// 本层是严格校验的: 成员之间必须有逗号, 不允许尾逗号, 键必须是字符串, 必须有冒号。
inline ValueRef FindMember(std::string_view obj, std::string_view key) {
    ValueRef out;
    const size_t n = obj.size();
    size_t i = 0;
    while (i < n && (unsigned char)obj[i] <= ' ') ++i;
    if (i >= n || obj[i] != '{') { out.malformed = true; return out; }
    ++i;
    bool first = true;          // 还没读到任何成员
    bool expectMember = true;   // 下一个非空白必须是成员 (或首次时可以是 '}')
    while (true) {
        while (i < n && (unsigned char)obj[i] <= ' ') ++i;
        if (i >= n) { out.malformed = true; return out; }   // 对象没闭合
        if (obj[i] == '}') {
            if (!first && expectMember) out.malformed = true;   // 尾逗号
            return out;                                         // 读完了, 没有这个键
        }
        if (!expectMember) {
            if (obj[i] != ',') { out.malformed = true; return out; }   // 成员之间缺逗号
            ++i;
            expectMember = true;
            continue;
        }
        if (obj[i] != '"') { out.malformed = true; return out; }       // 键必须是字符串
        const size_t ks = i + 1;
        size_t ke = ks;
        bool escaped = false;
        while (ke < n) {
            if (obj[ke] == '\\') { ke += 2; escaped = true; continue; }
            if (obj[ke] == '"') break;
            ++ke;
        }
        if (ke >= n) { out.malformed = true; return out; }
        const std::string_view thisKey = obj.substr(ks, ke - ks);
        i = ke + 1;
        while (i < n && (unsigned char)obj[i] <= ' ') ++i;
        if (i >= n || obj[i] != ':') { out.malformed = true; return out; }
        ++i;
        while (i < n && (unsigned char)obj[i] <= ' ') ++i;
        const size_t vs = i;
        ValueKind vk = ValueKind::None;
        const size_t ve = SkipValue(obj, i, vk);
        if (ve == std::string_view::npos) { out.malformed = true; return out; }
        // escaped 的键名不做还原, 直接跳过 —— 我们要找的键都是纯 ASCII
        if (!escaped && thisKey == key) {
            out.kind = vk;
            out.text = (vk == ValueKind::String) ? obj.substr(vs + 1, (ve - 1) - (vs + 1))
                                                 : obj.substr(vs, ve - vs);
            return out;
        }
        i = ve;
        first = false;
        expectMember = false;
    }
}

// 取数组文本里的第 0 个元素 (arrayText 是 FindMember 交回的 Array 原文, 以 '[' 开头)。
// 空数组返回 kind==None 且 malformed==false。
inline ValueRef FirstElement(std::string_view arrayText) {
    ValueRef out;
    const size_t n = arrayText.size();
    size_t i = 0;
    while (i < n && (unsigned char)arrayText[i] <= ' ') ++i;
    if (i >= n || arrayText[i] != '[') { out.malformed = true; return out; }
    ++i;
    while (i < n && (unsigned char)arrayText[i] <= ' ') ++i;
    if (i >= n) { out.malformed = true; return out; }
    if (arrayText[i] == ']') return out;                    // 空数组
    ValueKind vk = ValueKind::None;
    const size_t ve = SkipValue(arrayText, i, vk);
    if (ve == std::string_view::npos) { out.malformed = true; return out; }
    out.kind = vk;
    out.text = (vk == ValueKind::String) ? arrayText.substr(i + 1, (ve - 1) - (i + 1))
                                         : arrayText.substr(i, ve - i);
    return out;
}

// 整串必须是一个完整的十进制整数 —— from_chars 只报"读到了几个字符", 不检查就会把
// "bad-id" 读成 0、"1006oops" 读成 1006, 于是坏数据被当成好数据落盘。
inline bool ParseFullInt64(std::string_view s, long long& out) {
    if (s.empty()) return false;
    const char* const first = s.data();
    const char* const last  = s.data() + s.size();
    long long v = 0;
    const auto res = std::from_chars(first, last, v);
    if (res.ec != std::errc{} || res.ptr != last) return false;
    out = v;
    return true;
}

// MARK: - 数组遍历

// 遍历【一个已经定位好的数组】, 对每个元素对象回调。arrayText 以 '[' 开头 (前面允许空白)。
// 本层严格校验: 元素必须是对象, 元素之间必须有逗号, 不允许尾逗号, 必须扫到收尾的 ']'。
template<typename Cb>
[[nodiscard]] ArrayScan ForEachObjectIn(std::string_view arrayText, Cb&& cb) {
    const size_t n = arrayText.size();
    size_t i = 0;
    while (i < n && (unsigned char)arrayText[i] <= ' ') ++i;
    if (i >= n || arrayText[i] != '[') return ArrayScan::Malformed;
    ++i;
    bool first = true;           // 还没读到任何元素
    bool expectElement = true;   // 下一个非空白必须是元素 (或首次时可以是 ']')
    while (true) {
        while (i < n && (unsigned char)arrayText[i] <= ' ') ++i;
        if (i >= n) return ArrayScan::Malformed;            // 没闭合 = 被截断
        if (arrayText[i] == ']')
            return (first || !expectElement) ? ArrayScan::Ok : ArrayScan::Malformed;  // 尾逗号 -> Malformed
        if (!expectElement) {
            if (arrayText[i] != ',') return ArrayScan::Malformed;   // 元素之间缺逗号
            ++i;
            expectElement = true;
            continue;
        }
        if (arrayText[i] != '{') return ArrayScan::Malformed;       // 元素必须是对象
        ValueKind vk = ValueKind::None;
        const size_t ve = SkipValue(arrayText, i, vk);
        if (ve == std::string_view::npos || vk != ValueKind::Object) return ArrayScan::Malformed;
        cb(arrayText.substr(i, ve - i));
        i = ve;
        first = false;
        expectElement = false;
    }
}

// 全文找 "key": [ ... ] 并把数组原文交回来 (不遍历)。只给【服务器临时报文】用 ——
// 报文的嵌套层级随接口版本会变, 按路径写死反而更脆; 而它是一次性的, 读串了下次重拉即可。
// 本地存档【不要】用这个: 存档里的 non_pull_events[].raw 是服务器原样透传的对象, 完全
// 可能自带 "list": [...], 一旦它排在 endfield 前面, 全文首个匹配就落到那上面。
inline std::pair<LocateResult, std::string_view>
LocateArrayFullText(std::string_view src, std::string_view key) {
    const size_t len = src.length();
    for (size_t search = 0; ; ) {
        const size_t hit = FindKeyToken(src, key, search);
        if (hit == std::string_view::npos) return {LocateResult::NotFound, {}};
        size_t p = hit + key.length() + 2;   // FindKeyToken 返回起始引号, 跳过 "key"
        search = p;
        while (p < len && (unsigned char)src[p] <= ' ') ++p;
        // FindKeyToken 只认"两侧带引号", 值恰好等于键名时 ("foo":"list") 也会命中。
        // 后面不是 ':' 就说明这不是个键, 换下一处继续找, 而不是判文件损坏。
        if (p >= len || src[p] != ':') continue;
        ++p;
        while (p < len && (unsigned char)src[p] <= ' ') ++p;
        ValueKind vk = ValueKind::None;
        const size_t ve = SkipValue(src, p, vk);
        if (ve == std::string_view::npos) return {LocateResult::Malformed, {}};   // 值读不完 = 被截断
        if (vk == ValueKind::Null)  return {LocateResult::NotFound, {}};          // 显式 null 视同空
        if (vk != ValueKind::Array) return {LocateResult::Malformed, {}};
        return {LocateResult::Located, src.substr(p, ve - p)};
    }
}

// LocateArrayFullText + ForEachObjectIn 的组合。
template<typename Cb>
[[nodiscard]] ArrayScan ForEachObjectByKey(std::string_view src, std::string_view key, Cb&& cb) {
    const auto [st, text] = LocateArrayFullText(src, key);
    if (st == LocateResult::NotFound)  return ArrayScan::NotFound;
    if (st == LocateResult::Malformed) return ArrayScan::Malformed;
    return ForEachObjectIn(text, std::forward<Cb>(cb));
}

} // namespace efjson
