// キー順を保つ JSON(project.json の読み書き用)。QJsonObject はキーを並べ替えてしまうので使わない。
// 出力は Python の json.dumps(ensure_ascii=False, indent=2) と同じ形(数値は JS の GUI を経由した時と同じ: 整数値は整数で書く)
#pragma once
#include <QByteArray>
#include <QString>
#include <vector>

namespace vui {

class OVal {
public:
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    OVal() = default;
    static OVal null() { return OVal(); }
    static OVal boolean(bool b) { OVal v; v.t_ = Bool; v.b_ = b; return v; }
    static OVal num(double n) { OVal v; v.t_ = Num; v.n_ = n; return v; }
    static OVal str(const QString& s) { OVal v; v.t_ = Str; v.s_ = s; return v; }
    static OVal array() { OVal v; v.t_ = Arr; return v; }
    static OVal object() { OVal v; v.t_ = Obj; return v; }

    Type type() const { return t_; }
    bool isNull() const { return t_ == Null; }
    bool isNum() const { return t_ == Num; }
    bool isStr() const { return t_ == Str; }
    bool isArr() const { return t_ == Arr; }
    bool isObj() const { return t_ == Obj; }
    bool isBool() const { return t_ == Bool; }
    bool asBool() const { return t_ == Bool ? b_ : (t_ == Num ? n_ != 0 : (t_ == Str ? !s_.isEmpty() : (t_ == Arr || t_ == Obj))); }
    double asNum(double def = 0) const { return t_ == Num ? n_ : def; }
    const QString& asStr() const { return s_; }
    QString strOr(const QString& def) const { return t_ == Str ? s_ : def; }

    // 配列・オブジェクト共通
    int size() const { return (int)vals_.size(); }
    OVal& at(int i) { return vals_[i]; }
    const OVal& at(int i) const { return vals_[i]; }
    void push(const OVal& v) { vals_.push_back(v); if (t_ == Obj) keys_.push_back(QString()); }
    void insert(int i, const OVal& v) { vals_.insert(vals_.begin() + i, v); }
    void erase(int i) { vals_.erase(vals_.begin() + i); }
    void clear() { vals_.clear(); keys_.clear(); }
    std::vector<OVal>& items() { return vals_; }
    const std::vector<OVal>& items() const { return vals_; }

    // オブジェクト
    const OVal* get(const QString& k) const;
    OVal* get(const QString& k);
    bool has(const QString& k) const { return get(k) != nullptr; }
    OVal& set(const QString& k, const OVal& v);   // 在れば同じ位置で置き換え、無ければ末尾に足す(JS のオブジェクトと同じ)
    bool remove(const QString& k);
    const QString& keyAt(int i) const { return keys_[i]; }
    double numOr(const QString& k, double def) const { const OVal* v = get(k); return v && v->isNum() ? v->n_ : def; }
    QString strOr(const QString& k, const QString& def) const { const OVal* v = get(k); return v && v->isStr() ? v->s_ : def; }

    static bool parse(const QByteArray& utf8, OVal* out, QString* err = nullptr);
    QString dump(bool pretty = true) const;      // pretty: indent=2(改行は \n)
    bool operator==(const OVal& o) const;

private:
    void dumpTo(QString& out, bool pretty, int depth) const;
    Type t_ = Null;
    bool b_ = false;
    double n_ = 0;
    QString s_;
    std::vector<QString> keys_;
    std::vector<OVal> vals_;
};

QString fmtNumber(double v);              // JS の GUI を通って Python が書き直した時の数値表記
double toFixedNum(double v, int digits);  // +v.toFixed(digits)

}  // namespace vui
