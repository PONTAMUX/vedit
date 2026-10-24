#include "ojson.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <charconv>

namespace vui {

const OVal* OVal::get(const QString& k) const {
    if (t_ != Obj) return nullptr;
    for (size_t i = 0; i < keys_.size(); ++i) if (keys_[i] == k) return &vals_[i];
    return nullptr;
}
OVal* OVal::get(const QString& k) {
    return const_cast<OVal*>(static_cast<const OVal*>(this)->get(k));
}
OVal& OVal::set(const QString& k, const OVal& v) {
    if (t_ != Obj) { *this = object(); }
    for (size_t i = 0; i < keys_.size(); ++i) if (keys_[i] == k) { vals_[i] = v; return vals_[i]; }
    keys_.push_back(k);
    vals_.push_back(v);
    return vals_.back();
}
bool OVal::remove(const QString& k) {
    if (t_ != Obj) return false;
    for (size_t i = 0; i < keys_.size(); ++i) if (keys_[i] == k) { keys_.erase(keys_.begin() + i); vals_.erase(vals_.begin() + i); return true; }
    return false;
}
bool OVal::operator==(const OVal& o) const {
    if (t_ != o.t_) return false;
    switch (t_) {
    case Null: return true;
    case Bool: return b_ == o.b_;
    case Num: return n_ == o.n_;
    case Str: return s_ == o.s_;
    case Arr: return vals_ == o.vals_;
    case Obj: return keys_ == o.keys_ && vals_ == o.vals_;
    }
    return false;
}

// 一番短く元の値に戻る桁数で書く(Python の repr / JS の String と同じ形)。to_chars は snprintf を桁数ぶん回すより速い
static int fmtNumberTo(char* o, double v) {
    if (!std::isfinite(v)) { std::memcpy(o, "null", 4); return 4; }
    if (v == 0) { o[0] = '0'; return 1; }
    char buf[64];
    if (v == std::floor(v) && std::fabs(v) < 1e16) return int(std::to_chars(o, o + 40, (long long)v).ptr - o);
    char* end = std::to_chars(buf, buf + sizeof buf - 1, v, std::chars_format::scientific).ptr;
    *end = '\0';
    int n = 0;
    const char* p = buf;
    if (*p == '-') { o[n++] = '-'; ++p; }
    const char* ep = std::find(p, (const char*)end, 'e');
    char digits[32]; int nd = 0;
    for (const char* q = p; q < ep; ++q) if (*q != '.') digits[nd++] = *q;
    while (nd > 1 && digits[nd - 1] == '0') --nd;
    const int e = std::atoi(ep + 1);
    if (e < -4 || e >= 16) {
        o[n++] = digits[0];
        if (nd > 1) { o[n++] = '.'; for (int i = 1; i < nd; ++i) o[n++] = digits[i]; }
        n += std::snprintf(o + n, 16, "e%c%02d", e < 0 ? '-' : '+', std::abs(e));
    } else if (e >= 0) {
        if (nd <= e + 1) { for (int i = 0; i < nd; ++i) o[n++] = digits[i]; for (int i = nd; i < e + 1; ++i) o[n++] = '0'; }
        else { for (int i = 0; i <= e; ++i) o[n++] = digits[i]; o[n++] = '.'; for (int i = e + 1; i < nd; ++i) o[n++] = digits[i]; }
    } else {
        o[n++] = '0'; o[n++] = '.';
        for (int i = 0; i < -e - 1; ++i) o[n++] = '0';
        for (int i = 0; i < nd; ++i) o[n++] = digits[i];
    }
    return n;
}

QString fmtNumber(double v) {
    char o[64];
    return QString::fromLatin1(o, fmtNumberTo(o, v));
}

double toFixedNum(double v, int digits) {
    // JS の toFixed と同じ: 厳密な値で四捨五入(ちょうど半分は絶対値の大きい方へ)。long double なら v*10^digits は誤差なし
    if (!std::isfinite(v)) return v;
    long double p = 1;
    for (int i = 0; i < digits; ++i) p *= 10;
    const bool neg = v < 0;
    const long double s = (long double)std::fabs(v) * p;
    const long double n = std::floor(s + 0.5L);
    const double r = (double)n / (double)p;
    return neg ? -r : r;
}

static void escapeTo(QString& out, const QString& s) {
    out += '"';
    for (QChar c : s) {
        const ushort u = c.unicode();
        switch (u) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (u < 0x20) out += QString("\\u%1").arg(u, 4, 16, QChar('0'));
            else out += c;
        }
    }
    out += '"';
}

void OVal::dumpTo(QString& out, bool pretty, int depth) const {
    switch (t_) {
    case Null: out += "null"; break;
    case Bool: out += b_ ? "true" : "false"; break;
    case Num: { char o[64]; out += QLatin1StringView(o, fmtNumberTo(o, n_)); break; }
    case Str: escapeTo(out, s_); break;
    case Arr:
        if (vals_.empty()) { out += "[]"; break; }
        out += '[';
        for (size_t i = 0; i < vals_.size(); ++i) {
            if (i) out += ',';
            if (pretty) { out += '\n'; out += QString((depth + 1) * 2, ' '); }
            vals_[i].dumpTo(out, pretty, depth + 1);
        }
        if (pretty) { out += '\n'; out += QString(depth * 2, ' '); }
        out += ']';
        break;
    case Obj:
        if (vals_.empty()) { out += "{}"; break; }
        out += '{';
        for (size_t i = 0; i < vals_.size(); ++i) {
            if (i) out += ',';
            if (pretty) { out += '\n'; out += QString((depth + 1) * 2, ' '); }
            escapeTo(out, keys_[i]);
            out += pretty ? ": " : ":";
            vals_[i].dumpTo(out, pretty, depth + 1);
        }
        if (pretty) { out += '\n'; out += QString(depth * 2, ' '); }
        out += '}';
        break;
    }
}

QString OVal::dump(bool pretty) const {
    QString out;
    dumpTo(out, pretty, 0);
    return out;
}

namespace {
struct Parser {
    const QString& s;
    int i = 0;
    QString err;
    explicit Parser(const QString& str) : s(str) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; }
    bool fail(const QString& m) { if (err.isEmpty()) err = QString("%1 (pos %2)").arg(m).arg(i); return false; }
    bool lit(const char* w) {
        const int n = (int)strlen(w);
        if (s.mid(i, n) == QLatin1String(w)) { i += n; return true; }
        return false;
    }
    bool str(QString* out) {
        if (i >= s.size() || s[i] != '"') return fail("string expected");
        ++i;
        out->clear();
        while (i < s.size()) {
            const QChar c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { *out += c; continue; }
            if (i >= s.size()) break;
            const QChar e = s[i++];
            switch (e.unicode()) {
            case 'n': *out += '\n'; break;
            case 'r': *out += '\r'; break;
            case 't': *out += '\t'; break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case '/': *out += '/'; break;
            case '\\': *out += '\\'; break;
            case '"': *out += '"'; break;
            case 'u': {
                if (i + 4 > s.size()) return fail("short \\u");
                bool ok = false;
                const ushort u = s.mid(i, 4).toUShort(&ok, 16);
                if (!ok) return fail("bad \\u");
                *out += QChar(u);
                i += 4;
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool val(OVal* v, int depth) {
        if (depth > 200) return fail("too deep");
        ws();
        if (i >= s.size()) return fail("value expected");
        const QChar c = s[i];
        if (c == '{') {
            ++i;
            *v = OVal::object();
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                QString k;
                if (!str(&k)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("':' expected");
                ++i;
                OVal x;
                if (!val(&x, depth + 1)) return false;
                v->set(k, x);
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            ++i;
            *v = OVal::array();
            ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                OVal x;
                if (!val(&x, depth + 1)) return false;
                v->push(x);
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return fail("',' or ']' expected");
            }
        }
        if (c == '"') { QString x; if (!str(&x)) return false; *v = OVal::str(x); return true; }
        if (lit("true")) { *v = OVal::boolean(true); return true; }
        if (lit("false")) { *v = OVal::boolean(false); return true; }
        if (lit("null")) { *v = OVal::null(); return true; }
        const int st = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && (s[i].isDigit() || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
        bool ok = false;
        const double d = s.mid(st, i - st).toDouble(&ok);
        if (!ok) { i = st; return fail("bad value"); }
        *v = OVal::num(d);
        return true;
    }
};
}  // namespace

bool OVal::parse(const QByteArray& utf8, OVal* out, QString* err) {
    QByteArray b = utf8;
    if (b.startsWith("\xEF\xBB\xBF")) b.remove(0, 3);
    const QString text = QString::fromUtf8(b);
    Parser p(text);
    OVal v;
    if (!p.val(&v, 0)) { if (err) *err = p.err; return false; }
    p.ws();
    if (p.i != text.size()) { if (err) *err = "trailing characters"; return false; }
    *out = v;
    return true;
}

}  // namespace vui
