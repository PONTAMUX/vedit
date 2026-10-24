// エフェクトの値に書く式(expr.py の移植)。t・dur・value・fps・frame・pi・e と関数(sin cos … wiggle noise loop rand)
#pragma once
#include <QHash>
#include <QString>
#include <memory>

namespace vn {

struct ExprCtx {
    QHash<QString, double> vars;   // t dur value fps frame ほか
    long long seed = 0;
    double frame() const { return vars.value("frame", 0.0); }
};

class Expr {
public:
    // 式を読む。失敗したら ok()=false(eval は 0)
    explicit Expr(const QString& src, const QStringList& names = {});
    ~Expr();
    bool ok() const { return ok_; }
    QString error;
    double eval(const ExprCtx& c) const;

private:
    struct Node;
    std::shared_ptr<Node> root_;
    bool ok_ = false;
};

// 式の文字列から値を出す(同じ式は読み直さない)。読めない式は 0
double evalExpr(const QString& src, const ExprCtx& c, const QStringList& names = {});

}  // namespace vn
