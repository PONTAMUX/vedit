#include "expr.h"
#include <QMutex>
#include <QMutexLocker>
#include <cmath>
#include <functional>
#include <vector>
#include "pyrand.h"

namespace vn {

struct Expr::Node {
    std::function<double(const ExprCtx&)> f;
};

namespace {

using Fn = std::function<double(const ExprCtx&)>;

struct Tok { char kind; double num; QString s; };   // kind: n v o e

bool lex(const QString& s, std::vector<Tok>* out, QString* err) {
    int i = 0;
    const int n = s.size();
    while (i < n) {
        while (i < n && s[i].isSpace()) ++i;
        if (i >= n) break;
        const QChar c = s[i];
        if (c.isDigit() || (c == '.' && i + 1 < n && s[i + 1].isDigit())) {
            int j = i;
            while (j < n && s[j].isDigit()) ++j;
            if (j < n && s[j] == '.') { ++j; while (j < n && s[j].isDigit()) ++j; }
            out->push_back({'n', s.mid(i, j - i).toDouble(), {}});
            i = j;
        } else if (c.isLetter() || c == '_') {
            int j = i;
            while (j < n && (s[j].isLetterOrNumber() || s[j] == '_')) ++j;
            out->push_back({'v', 0, s.mid(i, j - i)});
            i = j;
        } else {
            QString op;
            if (i + 1 < n) {
                const QString two = s.mid(i, 2);
                if (two == "**") op = "^";
                else if (two == "<=" || two == ">=" || two == "==" || two == "!=") op = two;
            }
            if (!op.isEmpty()) i += 2;
            else if (QString("-+*/%^(),<>").contains(c)) { op = QString(c); ++i; }
            else { *err = QString("式が読めません(%1文字目): %2").arg(i + 1).arg(s); return false; }
            out->push_back({'o', 0, op});
        }
    }
    out->push_back({'e', 0, {}});
    return true;
}

double pmod(double a, double b) {
    double r = std::fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

double noise1(long long seed, double x) {
    const double fl = std::floor(x);
    const long long i = (long long)fl;
    const double f = x - fl;
    const double a = PyRandom(seed * 1000003 + i).uniform(-1, 1);
    const double b = PyRandom(seed * 1000003 + i + 1).uniform(-1, 1);
    const double u = f * f * (3 - 2 * f);
    return a + (b - a) * u;
}

double wiggle(long long seed, double t, double freq, double amp, double octaves, double mult) {
    double v = 0, a = 1, f = freq, tot = 0;
    const int oc = std::max(1, (int)octaves);
    for (int o = 0; o < oc; ++o) {
        v += noise1(seed + o * 7919, t * f) * a;
        tot += a;
        a *= mult;
        f *= 2;
    }
    return v / tot * amp;
}

double clamp01(double u) { return std::min(1.0, std::max(0.0, u)); }

struct Parser {
    std::vector<Tok> t;
    size_t i = 0;
    QString src, err;
    QStringList names;

    const Tok& peek() const { return t[i]; }
    bool isOp(const char* o) const { return t[i].kind == 'o' && t[i].s == o; }
    bool take(const char* o) {
        if (!isOp(o)) { err = QString("式の書き方が違います(%1 が必要): %2").arg(o).arg(src); return false; }
        ++i;
        return true;
    }
    Fn fail(const QString& m) { if (err.isEmpty()) err = m; return [](const ExprCtx&) { return 0.0; }; }

    Fn cmp() {
        Fn a = add();
        while (isOp("<") || isOp(">") || isOp("<=") || isOp(">=") || isOp("==") || isOp("!=")) {
            const QString op = t[i++].s;
            Fn b = add();
            a = [a, b, op](const ExprCtx& c) {
                const double x = a(c), y = b(c);
                bool r = op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : op == ">=" ? x >= y : op == "==" ? x == y : x != y;
                return r ? 1.0 : 0.0;
            };
        }
        return a;
    }
    Fn add() {
        Fn a = mul();
        while (isOp("+") || isOp("-")) {
            const bool plus = t[i++].s == "+";
            Fn b = mul();
            a = plus ? Fn([a, b](const ExprCtx& c) { return a(c) + b(c); }) : Fn([a, b](const ExprCtx& c) { return a(c) - b(c); });
        }
        return a;
    }
    Fn mul() {
        Fn a = unary();
        while (isOp("*") || isOp("/") || isOp("%")) {
            const QString op = t[i++].s;
            Fn b = unary();
            if (op == "*") a = [a, b](const ExprCtx& c) { return a(c) * b(c); };
            else if (op == "/") a = [a, b](const ExprCtx& c) { const double y = b(c); return y != 0 ? a(c) / y : 0.0; };
            else a = [a, b](const ExprCtx& c) { const double y = b(c); return y != 0 ? pmod(a(c), y) : 0.0; };
        }
        return a;
    }
    Fn unary() {
        if (isOp("-")) { ++i; Fn a = unary(); return [a](const ExprCtx& c) { return -a(c); }; }
        if (isOp("+")) { ++i; return unary(); }
        return power();
    }
    Fn power() {
        Fn a = atom();
        if (isOp("^")) {
            ++i;
            Fn b = unary();
            return [a, b](const ExprCtx& c) {
                const double x = a(c), y = b(c);
                return (x >= 0 || y == std::floor(y)) ? std::pow(x, y) : 0.0;
            };
        }
        return a;
    }
    Fn atom() {
        const Tok k = peek();
        if (k.kind == 'n') { ++i; const double v = k.num; return [v](const ExprCtx&) { return v; }; }
        if (k.kind == 'o' && k.s == "(") {
            ++i;
            Fn a = cmp();
            take(")");
            return a;
        }
        if (k.kind == 'v') {
            ++i;
            const QString v = k.s;
            if (isOp("(")) {
                ++i;
                std::vector<Fn> args;
                if (!isOp(")")) {
                    args.push_back(cmp());
                    while (isOp(",")) { ++i; args.push_back(cmp()); }
                }
                take(")");
                return call(v, args);
            }
            if (v == "pi") return [](const ExprCtx&) { return M_PI; };
            if (v == "e") return [](const ExprCtx&) { return M_E; };
            static const QStringList vars{"t", "dur", "value", "fps", "frame"};
            if (!vars.contains(v) && !names.contains(v) && !v.startsWith("p_")) return fail(QString("知らない変数です: %1").arg(v));
            return [v](const ExprCtx& c) { return c.vars.value(v, 0.0); };
        }
        return fail(QString("式の書き方が違います: %1").arg(src));
    }
    Fn call(const QString& v, const std::vector<Fn>& a) {
        const int n = (int)a.size();
        auto need = [&](int lo, int hi) {
            if (n < lo || n > hi) { fail(QString("%1() の引数の数が違います: %2").arg(v).arg(src)); return false; }
            return true;
        };
        auto A = [&](int k) { return a[k]; };
        if (v == "sin" || v == "cos" || v == "tan" || v == "abs" || v == "floor" || v == "ceil" || v == "sqrt" || v == "exp" || v == "log" ||
            v == "smooth") {
            if (!need(1, 1)) return fail("");
            Fn x = A(0);
            if (v == "sin") return [x](const ExprCtx& c) { return std::sin(x(c)); };
            if (v == "cos") return [x](const ExprCtx& c) { return std::cos(x(c)); };
            if (v == "tan") return [x](const ExprCtx& c) { return std::tan(x(c)); };
            if (v == "abs") return [x](const ExprCtx& c) { return std::fabs(x(c)); };
            if (v == "floor") return [x](const ExprCtx& c) { return std::floor(x(c)); };
            if (v == "ceil") return [x](const ExprCtx& c) { return std::ceil(x(c)); };
            if (v == "sqrt") return [x](const ExprCtx& c) { return std::sqrt(std::max(0.0, x(c))); };
            if (v == "exp") return [x](const ExprCtx& c) { return std::exp(std::min(700.0, x(c))); };
            if (v == "log") return [x](const ExprCtx& c) { return std::log(std::max(1e-300, x(c))); };
            return [x](const ExprCtx& c) { const double u = clamp01(x(c)); return 3 * u * u - 2 * u * u * u; };
        }
        if (v == "round") {
            if (!need(1, 2)) return fail("");
            Fn x = A(0), d = n > 1 ? A(1) : Fn([](const ExprCtx&) { return 0.0; });
            return [x, d](const ExprCtx& c) {
                const int nd = (int)d(c);
                const double p = std::pow(10.0, nd);
                return std::nearbyint(x(c) * p) / p;
            };
        }
        if (v == "pow") { if (!need(2, 2)) return fail(""); Fn x = A(0), y = A(1); return [x, y](const ExprCtx& c) { return std::pow(x(c), y(c)); }; }
        if (v == "min" || v == "max") {
            if (!need(2, 99)) return fail("");
            const bool mn = v == "min";
            std::vector<Fn> as = a;
            return [as, mn](const ExprCtx& c) {
                double r = as[0](c);
                for (size_t k = 1; k < as.size(); ++k) { const double y = as[k](c); r = mn ? (y < r ? y : r) : (y > r ? y : r); }
                return r;
            };
        }
        if (v == "clamp") { if (!need(3, 3)) return fail(""); Fn x = A(0), lo = A(1), hi = A(2); return [x, lo, hi](const ExprCtx& c) { return std::min(std::max(x(c), lo(c)), hi(c)); }; }
        if (v == "lerp") { if (!need(3, 3)) return fail(""); Fn x = A(0), y = A(1), u = A(2); return [x, y, u](const ExprCtx& c) { const double p = x(c); return p + (y(c) - p) * u(c); }; }
        if (v == "step") { if (!need(2, 2)) return fail(""); Fn e = A(0), x = A(1); return [e, x](const ExprCtx& c) { return x(c) >= e(c) ? 1.0 : 0.0; }; }
        if (v == "wiggle") {
            if (!need(2, 4)) return fail("");
            Fn f = A(0), am = A(1), o = n > 2 ? A(2) : Fn([](const ExprCtx&) { return 1.0; }), m = n > 3 ? A(3) : Fn([](const ExprCtx&) { return 0.5; });
            return [f, am, o, m](const ExprCtx& c) { return wiggle(c.seed, c.vars.value("t", 0.0), f(c), am(c), o(c), m(c)); };
        }
        if (v == "noise") { if (!need(1, 1)) return fail(""); Fn x = A(0); return [x](const ExprCtx& c) { return noise1(c.seed, x(c)); }; }
        if (v == "loop") {
            if (!need(1, 1)) return fail("");
            Fn p = A(0);
            return [p](const ExprCtx& c) { const double pp = p(c), t = c.vars.value("t", 0.0); return pp > 0 ? pmod(t, pp) : t; };
        }
        if (v == "rand") {
            if (!need(0, 2)) return fail("");
            Fn x = n > 0 ? A(0) : Fn(), y = n > 1 ? A(1) : Fn();
            return [x, y, n](const ExprCtx& c) {
                const double r = PyRandom(c.seed * 31 + (long long)c.frame()).random();
                if (n == 0) return r;
                if (n == 1) return r * x(c);
                const double lo = x(c);
                return lo + (y(c) - lo) * r;
            };
        }
        return fail(QString("知らない関数です: %1()").arg(v));
    }
};

}  // namespace

Expr::Expr(const QString& src, const QStringList& names) {
    Parser p;
    p.src = src;
    p.names = names;
    if (!lex(src, &p.t, &error)) return;
    Fn f = p.cmp();
    if (p.err.isEmpty() && p.peek().kind != 'e') p.err = QString("式の終わりに余分なものがあります: %1").arg(src);
    if (!p.err.isEmpty()) { error = p.err; return; }
    root_ = std::make_shared<Node>();
    root_->f = f;
    ok_ = true;
}

Expr::~Expr() = default;

double Expr::eval(const ExprCtx& c) const { return ok_ ? root_->f(c) : 0.0; }

double evalExpr(const QString& src, const ExprCtx& c, const QStringList& names) {
    static QMutex mu;
    static QHash<QString, std::shared_ptr<Expr>> cache;
    const QString key = src + '\x01' + names.join(',');
    std::shared_ptr<Expr> e;
    {
        QMutexLocker l(&mu);
        auto it = cache.find(key);
        if (it == cache.end()) it = cache.insert(key, std::make_shared<Expr>(src, names));
        e = it.value();
    }
    return e->eval(c);
}

}  // namespace vn
