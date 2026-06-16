// repro_class_template_alts_9: 真实 WTF 头 + 38 类模板备选 Node + 跨多变体深互递归
// (Node / AnchorSide=Variant<int,Child> / ChildOrNone=Variant<Child,None>),
// copy() 返回 Child(auto 返回类型推导),变参泛型 lambda + copy(x)... 包展开。
// 目标:多变体递归环 -> switchOn<Node, lambda> 无法全内联 -> 发射 -> StdLibExtras.h:579 撞墙。
#define WK_WINUWP 1
#include <wtf/StdLibExtras.h>
#include <tuple>
#include <vector>

namespace calc {

using WTF::switchOn;
using WTF::Variant;

struct Child;

// leaf
struct Number { double v; };
struct Percentage { double v; };
struct CanonicalDimension { double v; };
struct NonCanonicalDimension { double v; };
struct Symbol { int v; };
struct SiblingCount { int v; };
struct SiblingIndex { int v; };

// 跨变体:AnchorSide / ChildOrNone 自身是变体且含 Child -> 制造递归环
struct None {};
struct AnchorSide { Variant<int, Child>* value; };       // 指针避免不完整类型
struct ChildOrNone { Variant<Child, None>* value; };

// 类模板运算节点(各含 children: vector<Child*>)
struct Sum     { std::vector<Child*> children; };
struct Product { std::vector<Child*> children; };
struct Negate  { std::vector<Child*> children; };
struct Min     { std::vector<Child*> children; };
struct Max     { std::vector<Child*> children; };
struct Clamp   { std::vector<Child*> children; };
struct Hypot   { std::vector<Child*> children; };
struct Anchor    { AnchorSide* side; };
struct AnchorSize{ ChildOrNone* fallback; };

template<typename Op> struct IndirectNode { Op* op; const Op& operator*() const { return *op; } };

using Node = Variant<
    Number, Percentage, CanonicalDimension, NonCanonicalDimension, Symbol, SiblingCount, SiblingIndex,
    IndirectNode<Sum>, IndirectNode<Product>, IndirectNode<Negate>, IndirectNode<Min>,
    IndirectNode<Max>, IndirectNode<Clamp>, IndirectNode<Hypot>,
    IndirectNode<Anchor>, IndirectNode<AnchorSize>>;

struct Child { Node value; template<class T> Child(T&& t): value(std::forward<T>(t)) {} };

template<typename Op> const Op& deref(const IndirectNode<Op>& n) { return *n; }
template<typename T> const T& deref(const T& n) { return n; }

// ---- copy 重载簇(返回 Child,触发 auto 推导)----
inline Child copy(const Child&);
inline Child copy(const Number& n) { return Child { n }; }
inline Child copy(const Percentage& n) { return Child { n }; }
inline Child copy(const CanonicalDimension& n) { return Child { n }; }
inline Child copy(const NonCanonicalDimension& n) { return Child { n }; }
inline Child copy(const Symbol& n) { return Child { n }; }
inline Child copy(const SiblingCount& n) { return Child { n }; }
inline Child copy(const SiblingIndex& n) { return Child { n }; }

// Op(含 vector<Child*>):变参不适用,直接遍历递归
template<typename Op>
inline auto copyChildrenOp(const Op& op) -> Op
{
    Op out;
    for (auto* c : op.children)
        out.children.push_back(new Child(copy(*c)));
    return out;
}
inline Child copy(const Sum& o)   { return Child { IndirectNode<Sum>   { new Sum(copyChildrenOp(o)) } }; }
inline Child copy(const Product& o){ return Child { IndirectNode<Product>{ new Product(copyChildrenOp(o)) } }; }
inline Child copy(const Negate& o){ return Child { IndirectNode<Negate>{ new Negate(copyChildrenOp(o)) } }; }
inline Child copy(const Min& o)   { return Child { IndirectNode<Min>   { new Min(copyChildrenOp(o)) } }; }
inline Child copy(const Max& o)   { return Child { IndirectNode<Max>   { new Max(copyChildrenOp(o)) } }; }
inline Child copy(const Clamp& o) { return Child { IndirectNode<Clamp> { new Clamp(copyChildrenOp(o)) } }; }
inline Child copy(const Hypot& o) { return Child { IndirectNode<Hypot> { new Hypot(copyChildrenOp(o)) } }; }

// Anchor -> AnchorSide(嵌套变体)-> switchOn 再入
inline AnchorSide copyAnchorSide(const AnchorSide& s)
{
    auto* nv = new Variant<int, Child>(switchOn(*s.value, [&](const auto& alt) -> Variant<int, Child> {
        if constexpr (std::is_same_v<std::decay_t<decltype(alt)>, Child>)
            return Variant<int, Child> { copy(alt) };
        else
            return Variant<int, Child> { alt };
    }));
    return AnchorSide { nv };
}
inline Child copy(const Anchor& a) { return Child { IndirectNode<Anchor> { new Anchor { new AnchorSide(copyAnchorSide(*a.side)) } } }; }

// AnchorSize -> ChildOrNone(嵌套变体)-> switchOn 再入
inline ChildOrNone copyChildOrNone(const ChildOrNone& c)
{
    auto* nv = new Variant<Child, None>(switchOn(*c.value, [&](const auto& alt) -> Variant<Child, None> {
        if constexpr (std::is_same_v<std::decay_t<decltype(alt)>, Child>)
            return Variant<Child, None> { copy(alt) };
        else
            return Variant<Child, None> { alt };
    }));
    return ChildOrNone { nv };
}
inline Child copy(const AnchorSize& a) { return Child { IndirectNode<AnchorSize> { new AnchorSize { new ChildOrNone(copyChildOrNone(*a.fallback)) } } }; }

// Child:switchOn 解出 Node 备选,泛型 lambda 递归回 copy(返回 Child)
inline Child copy(const Child& root)
{
    return switchOn(root.value, [&](const auto& alt) -> Child { return copy(deref(alt)); });
}

} // namespace calc

calc::Child run(const calc::Child& c) { return calc::copy(c); }
