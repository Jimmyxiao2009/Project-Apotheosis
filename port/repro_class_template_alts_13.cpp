// repro_class_template_alts_13: 命中真实残留点 —— CSSCalcTree+Simplification.cpp:switchTogether<F...>。
// switchOn 内的 lambda 词法上嵌在变参函数模板 switchTogether<F...> 里(F... 为未展开包),
// lambda 返回类型 ResultType 由 F... 推导。switchTogether<F...> 被发射时,内层 switchOn 的闭包
// MS-ABI 修饰名需编码其词法父 switchTogether<F...> 的包 F... -> "cannot mangle this pack expansion yet"。
#define WK_WINUWP 1
#include <wtf/StdLibExtras.h>
#include <optional>

using WTF::switchOn; using WTF::makeVisitor; using WTF::Variant;

template<class T> struct IndirectNode { T* op; const T& operator*() const { return *op; } };
struct Sin{}; struct Cos{}; struct Tan{}; struct Asin{}; struct Acos{};
struct Atan{}; struct Log{}; struct Exp{}; struct Abs{}; struct Sign{};
struct Number { double v; };
struct Child;
using Node = Variant<
    Number,
    IndirectNode<Sin>, IndirectNode<Cos>, IndirectNode<Tan>, IndirectNode<Asin>, IndirectNode<Acos>,
    IndirectNode<Atan>, IndirectNode<Log>, IndirectNode<Exp>, IndirectNode<Abs>, IndirectNode<Sign>>;
struct Child {
    Node value;
    template<class T> Child(T&& t): value(std::forward<T>(t)) {}
    size_t index() const { return value.index(); }
    const Node& asVariant() const { return value; }
};
template<class... Ts> const Variant<Ts...>& asVariant(const Child& c) { return c.value; }

// 让 WTF::switchOn 能解出 Child(它对 Variant 工作;Child 提供 asVariant)
// 这里直接对 Child.value 调 switchOn。

// === 真实 switchTogether<F...> 形态 ===
template<typename T> const T& get_b(const Child& b) { return std::get<T>(b.value); }

template<typename... F> [[gnu::noinline]] static decltype(auto) switchTogether(const Child& a, const Child& b, F&&... f)
{
    auto visitor = makeVisitor(std::forward<F>(f)...);
    using ResultType = decltype(visitor(std::declval<Number>(), std::declval<Number>()));

    if (a.index() != b.index())
        return visitor(std::nullopt, std::nullopt);

    // lambda 词法嵌在 switchTogether<F...> 内,返回 ResultType(依赖 F...),带显式模板参 <typename T>
    return switchOn(a.value,
        [&]<typename T>(const T& aT) -> ResultType {
            return visitor(aT, get_b<T>(b));
        });
}

// 命名 lambda 类型,便于取 switchTogether<F...> 实例地址,逼其独立发射
using L0 = std::optional<Child>(*)(const Number&, const Number&);
using L1 = std::optional<Child>(*)(std::nullopt_t, std::nullopt_t);

struct F0 { std::optional<Child> operator()(const Number& x, const Number& y) const { return Child { Number { x.v + y.v } }; } };
struct F1 { std::optional<Child> operator()(std::nullopt_t, std::nullopt_t) const { return std::nullopt; } };
struct F2 { template<class A, class B> std::optional<Child> operator()(const A&, const B&) const { return std::nullopt; } };

// 取 switchTogether<F0,F1,F2> 的地址 -> 必发射该变参模板实例 -> 内层 lambda 须修饰(含 F... 包)
[[gnu::noinline]] std::optional<Child> mergePair(const Child& a, const Child& b)
{
    return switchTogether(a, b, F0{}, F1{}, F2{});
}

std::optional<Child> (*volatile gp)(const Child&, const Child&) = &mergePair;
