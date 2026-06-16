// repro_class_template_alts_14: 命中 CSSCalcTree+Traversal.h 的真实残留点 ——
// forAllChildNodes<F,Op> 内 WTF::apply([&](const auto& ...x){ (..., caller(x)); }, op)
// 的"变参泛型 lambda [&](const auto& ...x)"。其闭包 operator()<Xs...> 带参数包,
// 作为 apply<F,T> 的模板实参被发射修饰时 -> "cannot mangle this pack expansion yet"。
// 叠加:38 个类模板 IndirectNode<Op> 备选 + switchOn 泛型 lambda + 深互递归。
#define WK_WINUWP 1
#include <wtf/StdLibExtras.h>
#include <tuple>

using WTF::switchOn; using WTF::apply; using WTF::Variant;

struct Child;
struct Number { double v; };
struct Percentage { double v; };
struct Symbol { int v; };

// Op 含 tuple<Child*...>(让 apply 在其上展开 ...x)
struct Sum     { std::tuple<Child*, Child*> kids; };
struct Product { std::tuple<Child*, Child*> kids; };
struct Negate  { std::tuple<Child*> kids; };
struct Min     { std::tuple<Child*, Child*, Child*> kids; };
struct Max     { std::tuple<Child*, Child*, Child*> kids; };
struct Clamp   { std::tuple<Child*, Child*, Child*> kids; };
struct Hypot   { std::tuple<Child*, Child*> kids; };
struct Pow     { std::tuple<Child*, Child*> kids; };

template<typename Op> struct IndirectNode { Op* op; const Op& operator*() const { return *op; } };

using Node = Variant<
    Number, Percentage, Symbol,
    IndirectNode<Sum>, IndirectNode<Product>, IndirectNode<Negate>, IndirectNode<Min>,
    IndirectNode<Max>, IndirectNode<Clamp>, IndirectNode<Hypot>, IndirectNode<Pow>>;

struct Child {
    Node value;
    template<class T> Child(T&& t): value(std::forward<T>(t)) {}
    const Node& asVariant() const { return value; }
};

template<typename Op> const Op& deref(const IndirectNode<Op>& n) { return *n; }
template<typename T> const T& deref(const T& n) { return n; }

// ---- 真实 forAllChildNodes 形态 ----
template<typename F> void forAllChildNodes(const Child& root, const F& functor);

// leaf:无子
template<typename F> void forAllChildNodes(const Number&, const F&) { }
template<typename F> void forAllChildNodes(const Percentage&, const F&) { }
template<typename F> void forAllChildNodes(const Symbol&, const F&) { }

// Op:用 Caller 结构 + WTF::apply([&](const auto& ...x){ (..., caller(x)); }, op.kids)
template<typename F, typename Op> void forAllChildNodes(const Op& op, const F& functor)
{
    struct Caller {
        const F& functor;
        void operator()(Child* child) { if (child) functor(*child); }
    };
    auto caller = Caller { functor };
    // 关键:变参泛型 lambda [&](const auto& ...x) —— 闭包 operator()<Xs...> 带参数包
    apply([&](const auto& ...x) { (..., caller(x)); }, op.kids);
}

// Child:switchOn 泛型 lambda 递归回 forAllChildNodes
template<typename F> void forAllChildNodes(const Child& root, const F& functor)
{
    switchOn(root.value, [&](const auto& alt) { forAllChildNodes(deref(alt), functor); });
}

// 顶层:仿 collectComputedStyleDependencies —— switchOn 大变体 + catch-all 调 forAllChildNodes
[[gnu::noinline]] void collect(const Child& root, int& acc)
{
    switchOn(root.value,
        [&](const Number& n)    { acc += (int)n.v; },
        [&](const Percentage& p){ acc += (int)p.v; },
        [&](const Symbol& s)    { acc += s.v; },
        [&](const auto& alt) {
            forAllChildNodes(deref(alt), [&](const auto& child) { collect(child, acc); });
        });
}

void run(const Child& root, int& acc) { collect(root, acc); }
