// vaudio: 順序を保つ最小 JSON。Python の json.dumps / json.load と互換に読み書きする。
// int と float を区別して保持する(3 と 3.0 を書き分ける)ので、触っていないキーはそのまま保存できる。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vj {

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;  // 挿入順を保つ

struct Value {
    enum Type { Null, Bool, Int, Float, String, Arr, Obj } type = Null;
    bool b = false;
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::shared_ptr<Array> a;
    std::shared_ptr<Object> o;

    Value() = default;
    static Value null() { return Value(); }
    static Value boolean(bool v) { Value x; x.type = Bool; x.b = v; return x; }
    static Value integer(int64_t v) { Value x; x.type = Int; x.i = v; return x; }
    static Value number(double v) { Value x; x.type = Float; x.d = v; return x; }
    static Value str(std::string v) { Value x; x.type = String; x.s = std::move(v); return x; }
    static Value array() { Value x; x.type = Arr; x.a = std::make_shared<Array>(); return x; }
    static Value object() { Value x; x.type = Obj; x.o = std::make_shared<Object>(); return x; }

    bool isNull() const { return type == Null; }
    bool isNum() const { return type == Int || type == Float; }
    bool isObj() const { return type == Obj; }
    bool isArr() const { return type == Arr; }
    bool isStr() const { return type == String; }
    double num() const { return type == Int ? (double)i : d; }

    // 値の深いコピー(shared_ptr を共有しない)
    Value clone() const;

    // object アクセス
    const Value* find(const std::string& k) const;
    Value* find(const std::string& k);
    Value& set(const std::string& k, Value v);  // 既存キーは位置を保って置換、無ければ末尾に追加
    void erase(const std::string& k);
    Value get(const std::string& k) const { auto p = find(k); return p ? *p : Value(); }
    std::string getStr(const std::string& k, const std::string& def = "") const;
    double getNum(const std::string& k, double def) const;

    // array アクセス
    size_t size() const { return type == Arr ? a->size() : type == Obj ? o->size() : 0; }
    Value& at(size_t n) { return (*a)[n]; }
    const Value& at(size_t n) const { return (*a)[n]; }
    void push(Value v) { a->push_back(std::move(v)); }
};

// 解析。失敗時は例外(std::runtime_error)。
Value parse(const std::string& text);

// Python の json.dumps 相当。indent<0 は既定の区切り(", " と ": ")、indent>=0 は改行つき。
struct DumpOpt {
    bool sortKeys = false;
    bool ensureAscii = true;
    int indent = -1;
};
std::string dump(const Value& v, const DumpOpt& opt = DumpOpt());

// Python の repr(float) と同じ文字列
std::string pyFloat(double d);

// 構造の一致(int 3 と float 3.0 は等しいとみなす)
bool equal(const Value& a, const Value& b);

}  // namespace vj
