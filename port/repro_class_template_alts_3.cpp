// repro_class_template_alts_3: 忠实复刻 CSSCalcTree+Traversal.h 的真实惯用法。
//   Node = Variant<7 个 leaf + ~31 个 IndirectNode<Op> 类模板备选>
//   Child { Node value; }  +  IndirectNode<Op>::operator*  转发到 Op
//   forAllChildren<F>(const Child&, const F&) 内用 switchOn + 泛型 lambda 递归回 forAllChildren
//   forAllChildren<F, Op>(const Op&, const F&) 遍历 Op 的 Children(每个是 Child)再入
// 目标:模板函数内 switchOn(变参) + 泛型 lambda + 38 类模板备选 + 深互递归 → 发射 →
//       撞 "cannot mangle this pack expansion yet"。
#include <variant>
#include <optional>
#include <utility>
#include <vector>
#include <memory>

template<class A, class... B> struct Visitor : Visitor<A>, Visitor<B...> {
    Visitor(A a, B... b) : Visitor<A>(a), Visitor<B...>(b...) { }
    using Visitor<A>::operator();
    using Visitor<B...>::operator();
};
template<class A> struct Visitor<A> : A {
    Visitor(A a) : A(a) { }
    using A::operator();
};
template<class... F> [[gnu::always_inline]] inline Visitor<F...> makeVisitor(F... f) { return Visitor<F...>(f...); }

// 真实 WK_WINUWP:switchOn -> WTF::visit(变参包装层) -> std::visit。
// 这一层 wrapper 是 std::variant 残留墙的关键:它带 Variants&&... 包,被发射时其修饰名含包展开。
template<typename Vis, typename... Variants> constexpr auto wtf_visit(Vis&& v, Variants&&... values)
{ return std::visit(std::forward<Vis>(v), std::forward<Variants>(values)...); }

template<class V, class... F> [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return wtf_visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

// ---- leaf 类型(7 个,仿 Number/Percentage/CanonicalDimension/... ) ----
struct Number { double v; };
struct Percentage { double v; };
struct CanonicalDimension { double v; };
struct NonCanonicalDimension { double v; };
struct Symbol { int v; };
struct SiblingCount { int v; };
struct SiblingIndex { int v; };

struct Child;  // 前置

// ---- 31 个 Op 运算结构,各含 Children(子节点列表) ----
struct Children;  // 前置
#define DECL_OP(name) struct name { Children* kids; };
DECL_OP(Sum) DECL_OP(Product) DECL_OP(Negate) DECL_OP(Invert)
DECL_OP(Min) DECL_OP(Max) DECL_OP(Clamp) DECL_OP(RoundNearest)
DECL_OP(RoundUp) DECL_OP(RoundDown) DECL_OP(RoundToZero) DECL_OP(Mod)
DECL_OP(Rem) DECL_OP(Sin) DECL_OP(Cos) DECL_OP(Tan)
DECL_OP(Asin) DECL_OP(Acos) DECL_OP(Atan) DECL_OP(Atan2)
DECL_OP(Pow) DECL_OP(Sqrt) DECL_OP(Hypot) DECL_OP(Log)
DECL_OP(Exp) DECL_OP(Abs) DECL_OP(Sign) DECL_OP(Random)
DECL_OP(Progress) DECL_OP(Anchor) DECL_OP(AnchorSize)
#undef DECL_OP

// ---- IndirectNode<Op>(真实形态:含 UniqueRef<Op>,operator* 转发)----
template<typename Op> struct IndirectNode {
    std::unique_ptr<Op> op;
    const Op& operator*() const { return *op; }
    Op& operator*() { return *op; }
};

using Node = std::variant<
    Number, Percentage, CanonicalDimension, NonCanonicalDimension, Symbol, SiblingCount, SiblingIndex,
    IndirectNode<Sum>, IndirectNode<Product>, IndirectNode<Negate>, IndirectNode<Invert>,
    IndirectNode<Min>, IndirectNode<Max>, IndirectNode<Clamp>, IndirectNode<RoundNearest>,
    IndirectNode<RoundUp>, IndirectNode<RoundDown>, IndirectNode<RoundToZero>, IndirectNode<Mod>,
    IndirectNode<Rem>, IndirectNode<Sin>, IndirectNode<Cos>, IndirectNode<Tan>,
    IndirectNode<Asin>, IndirectNode<Acos>, IndirectNode<Atan>, IndirectNode<Atan2>,
    IndirectNode<Pow>, IndirectNode<Sqrt>, IndirectNode<Hypot>, IndirectNode<Log>,
    IndirectNode<Exp>, IndirectNode<Abs>, IndirectNode<Sign>, IndirectNode<Random>,
    IndirectNode<Progress>, IndirectNode<Anchor>, IndirectNode<AnchorSize>>;

struct Child {
    Node value;
    template<typename T> Child(T&& t) : value(std::forward<T>(t)) { }
    Node& asVariant() { return value; }
    const Node& asVariant() const { return value; }
};

struct Children { std::vector<Child> value; };

// asVariant 自由函数(switchOn 对 Child 解出 Node)
inline const Node& asVariant(const Child& c) { return c.value; }

// ---- 真实 Traversal 形态 ----
// leaf 不递归
template<typename F> void forAllChildren(const Number&, const F&) { }
template<typename F> void forAllChildren(const Percentage&, const F&) { }
template<typename F> void forAllChildren(const CanonicalDimension&, const F&) { }
template<typename F> void forAllChildren(const NonCanonicalDimension&, const F&) { }
template<typename F> void forAllChildren(const Symbol&, const F&) { }
template<typename F> void forAllChildren(const SiblingCount&, const F&) { }
template<typename F> void forAllChildren(const SiblingIndex&, const F&) { }

// Op:遍历其 Children,每个 child 调 functor 并递归
template<typename F, typename Op> void forAllChildren(const Op& root, const F& functor)
{
    if (root.kids)
        for (auto& child : root.kids->value) {
            functor(child);
            forAllChildren(child, functor);  // 递归到 Child 重载
        }
}

// deref:IndirectNode<Op> 解引用得 Op;leaf 原样返回。
template<typename Op> const Op& deref(const IndirectNode<Op>& n) { return *n; }
template<typename T> const T& deref(const T& n) { return n; }

// Child:switchOn 解出 Node 的具体备选,泛型 lambda 递归回 forAllChildren
template<typename F> void forAllChildren(const Child& root, const F& functor)
{
    switchOn(root.value, [&](const auto& alt) { forAllChildren(deref(alt), functor); });
}

// run:实际触发实例化与发射
void run(const Child& root)
{
    forAllChildren(root, [](const Child&) { });
}
