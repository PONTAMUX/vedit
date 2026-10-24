#include "json.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace vj {

Value Value::clone() const {
    Value r = *this;
    if (type == Arr) {
        r.a = std::make_shared<Array>();
        for (auto& e : *a) r.a->push_back(e.clone());
    } else if (type == Obj) {
        r.o = std::make_shared<Object>();
        for (auto& e : *o) r.o->emplace_back(e.first, e.second.clone());
    }
    return r;
}

const Value* Value::find(const std::string& k) const {
    if (type != Obj) return nullptr;
    for (auto& e : *o) if (e.first == k) return &e.second;
    return nullptr;
}
Value* Value::find(const std::string& k) {
    if (type != Obj) return nullptr;
    for (auto& e : *o) if (e.first == k) return &e.second;
    return nullptr;
}
Value& Value::set(const std::string& k, Value v) {
    if (auto p = find(k)) { *p = std::move(v); return *p; }
    o->emplace_back(k, std::move(v));
    return o->back().second;
}
void Value::erase(const std::string& k) {
    if (type != Obj) return;
    o->erase(std::remove_if(o->begin(), o->end(), [&](auto& e) { return e.first == k; }), o->end());
}
std::string Value::getStr(const std::string& k, const std::string& def) const {
    auto p = find(k);
    return p && p->type == String ? p->s : def;
}
double Value::getNum(const std::string& k, double def) const {
    auto p = find(k);
    return p && p->isNum() ? p->num() : def;
}

// ---------------- parse ----------------
namespace {
struct Parser {
    const std::string& t;
    size_t p = 0;
    explicit Parser(const std::string& s) : t(s) {}
    [[noreturn]] void fail(const char* m) { throw std::runtime_error(std::string("JSON: ") + m + " @" + std::to_string(p)); }
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) p++; }
    char cur() { if (p >= t.size()) fail("unexpected end"); return t[p]; }
    static void putUtf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    uint32_t hex4() {
        if (p + 4 > t.size()) fail("bad \\u");
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            char c = t[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else fail("bad hex");
        }
        return v;
    }
    std::string string() {
        if (cur() != '"') fail("string expected");
        p++;
        std::string o;
        while (true) {
            if (p >= t.size()) fail("unterminated string");
            char c = t[p++];
            if (c == '"') break;
            if (c != '\\') { o += c; continue; }
            if (p >= t.size()) fail("bad escape");
            char e = t[p++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    uint32_t u = hex4();
                    if (u >= 0xD800 && u < 0xDC00 && p + 1 < t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                        p += 2;
                        uint32_t lo = hex4();
                        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(o, u);
                    break;
                }
                default: fail("bad escape");
            }
        }
        return o;
    }
    Value value() {
        ws();
        char c = cur();
        if (c == '{') {
            p++;
            Value v = Value::object();
            ws();
            if (cur() == '}') { p++; return v; }
            while (true) {
                ws();
                std::string k = string();
                ws();
                if (cur() != ':') fail("':' expected");
                p++;
                Value e = value();
                v.set(k, std::move(e));
                ws();
                if (cur() == ',') { p++; continue; }
                if (cur() == '}') { p++; break; }
                fail("',' or '}' expected");
            }
            return v;
        }
        if (c == '[') {
            p++;
            Value v = Value::array();
            ws();
            if (cur() == ']') { p++; return v; }
            while (true) {
                v.push(value());
                ws();
                if (cur() == ',') { p++; continue; }
                if (cur() == ']') { p++; break; }
                fail("',' or ']' expected");
            }
            return v;
        }
        if (c == '"') return Value::str(string());
        if (t.compare(p, 4, "true") == 0) { p += 4; return Value::boolean(true); }
        if (t.compare(p, 5, "false") == 0) { p += 5; return Value::boolean(false); }
        if (t.compare(p, 4, "null") == 0) { p += 4; return Value(); }
        if (t.compare(p, 3, "NaN") == 0) { p += 3; return Value::number(NAN); }
        if (t.compare(p, 8, "Infinity") == 0) { p += 8; return Value::number(INFINITY); }
        if (t.compare(p, 9, "-Infinity") == 0) { p += 9; return Value::number(-INFINITY); }
        size_t s = p;
        bool isFloat = false;
        if (t[p] == '-') p++;
        while (p < t.size() && ((t[p] >= '0' && t[p] <= '9') || t[p] == '.' || t[p] == 'e' || t[p] == 'E' || t[p] == '+' || t[p] == '-')) {
            if (t[p] == '.' || t[p] == 'e' || t[p] == 'E') isFloat = true;
            p++;
        }
        if (p == s) fail("unexpected character");
        std::string n = t.substr(s, p - s);
        if (!isFloat) {
            errno = 0;
            long long ll = strtoll(n.c_str(), nullptr, 10);
            if (errno == 0) return Value::integer(ll);
        }
        return Value::number(strtod(n.c_str(), nullptr));
    }
};
}  // namespace

Value parse(const std::string& text) {
    std::string s = text;
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    Parser ps(s);
    Value v = ps.value();
    ps.ws();
    if (ps.p != s.size()) ps.fail("trailing data");
    return v;
}

// ---------------- dump ----------------
std::string pyFloat(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == 0) return std::signbit(d) ? "-0.0" : "0.0";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);  // 最短往復表現 例 "1.2345e+02"
    std::string sci(buf, r.ptr);
    bool neg = sci[0] == '-';
    if (neg) sci.erase(0, 1);
    size_t e = sci.find('e');
    std::string mant = sci.substr(0, e);
    int exp10 = atoi(sci.c_str() + e + 1);
    std::string digits;
    for (char c : mant) if (c != '.') digits += c;
    int nd = (int)digits.size();
    std::string out;
    // Python repr: 10^-4 <= |x| < 10^16 は固定小数、それ以外は指数表記
    if (exp10 >= -4 && exp10 < 16) {
        if (exp10 >= 0) {
            if (nd <= exp10 + 1) out = digits + std::string(exp10 + 1 - nd, '0') + ".0";
            else out = digits.substr(0, exp10 + 1) + "." + digits.substr(exp10 + 1);
        } else {
            out = "0." + std::string(-exp10 - 1, '0') + digits;
        }
    } else {
        out = digits.substr(0, 1);
        if (nd > 1) out += "." + digits.substr(1);
        char eb[16];
        snprintf(eb, sizeof eb, "e%c%02d", exp10 < 0 ? '-' : '+', std::abs(exp10));
        out += eb;
    }
    return neg ? "-" + out : out;
}

namespace {
void escape(std::string& o, const std::string& s, bool ascii) {
    o += '"';
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { o += "\\\""; i++; }
        else if (c == '\\') { o += "\\\\"; i++; }
        else if (c == '\n') { o += "\\n"; i++; }
        else if (c == '\r') { o += "\\r"; i++; }
        else if (c == '\t') { o += "\\t"; i++; }
        else if (c == '\b') { o += "\\b"; i++; }
        else if (c == '\f') { o += "\\f"; i++; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; i++; }
        else if (c < 0x80 || !ascii) { o += (char)c; i++; }
        else {
            int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            uint32_t cp = c & (0xFF >> (n + 1));
            for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
            i += n;
            char b[24];
            if (cp >= 0x10000) {
                cp -= 0x10000;
                snprintf(b, sizeof b, "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
            } else {
                snprintf(b, sizeof b, "\\u%04x", cp);
            }
            o += b;
        }
    }
    o += '"';
}

void dumpRec(std::string& o, const Value& v, const DumpOpt& opt, int depth) {
    auto nl = [&](int d) {
        if (opt.indent >= 0) { o += '\n'; o.append((size_t)d * opt.indent, ' '); }
    };
    switch (v.type) {
        case Value::Null: o += "null"; break;
        case Value::Bool: o += v.b ? "true" : "false"; break;
        case Value::Int: o += std::to_string(v.i); break;
        case Value::Float: o += pyFloat(v.d); break;
        case Value::String: escape(o, v.s, opt.ensureAscii); break;
        case Value::Arr:
            if (v.a->empty()) { o += "[]"; break; }
            o += '[';
            for (size_t k = 0; k < v.a->size(); k++) {
                if (k) o += opt.indent >= 0 ? "," : ", ";
                nl(depth + 1);
                dumpRec(o, (*v.a)[k], opt, depth + 1);
            }
            nl(depth);
            o += ']';
            break;
        case Value::Obj: {
            if (v.o->empty()) { o += "{}"; break; }
            std::vector<const std::pair<std::string, Value>*> items;
            for (auto& e : *v.o) items.push_back(&e);
            // UTF-8 のバイト順 = コードポイント順(Python の sort_keys と同じ)
            if (opt.sortKeys) std::stable_sort(items.begin(), items.end(), [](auto a, auto b) { return a->first < b->first; });
            o += '{';
            for (size_t k = 0; k < items.size(); k++) {
                if (k) o += opt.indent >= 0 ? "," : ", ";
                nl(depth + 1);
                escape(o, items[k]->first, opt.ensureAscii);
                o += ": ";
                dumpRec(o, items[k]->second, opt, depth + 1);
            }
            nl(depth);
            o += '}';
            break;
        }
    }
}
}  // namespace

std::string dump(const Value& v, const DumpOpt& opt) {
    std::string o;
    dumpRec(o, v, opt, 0);
    return o;
}

bool equal(const Value& a, const Value& b) {
    if (a.isNum() && b.isNum()) return a.num() == b.num();
    if (a.type != b.type) return false;
    switch (a.type) {
        case Value::Null: return true;
        case Value::Bool: return a.b == b.b;
        case Value::String: return a.s == b.s;
        case Value::Arr:
            if (a.size() != b.size()) return false;
            for (size_t k = 0; k < a.size(); k++) if (!equal(a.at(k), b.at(k))) return false;
            return true;
        case Value::Obj:
            if (a.size() != b.size()) return false;
            for (auto& e : *a.o) {
                auto p = b.find(e.first);
                if (!p || !equal(e.second, *p)) return false;
            }
            return true;
        default: return true;
    }
}

}  // namespace vj
