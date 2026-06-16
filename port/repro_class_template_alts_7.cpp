// repro_class_template_alts_7: 真实触发点 —— 变参泛型 lambda [](const auto&...x){ Op{ f(x)... } }
// (CSSCalcTree+Copy.cpp:91)。其闭包类型作为模板实参传入变参模板(apply/switchOn),
// MS-ABI 修饰该闭包的 operator()(模板)时撞内部包展开 -> "cannot mangle this pack expansion yet"。
#include <variant>
#include <utility>
#include <tuple>

// apply 形态(StdLibExtras.h:1345)
template<class F, class T, size_t... I>
constexpr decltype(auto) apply_impl(F&& functor, T&& t, std::index_sequence<I...>)
{ using std::get; return std::invoke(std::forward<F>(functor), get<I>(std::forward<T>(t))...); }
template<class F, class T>
constexpr decltype(auto) apply(F&& functor, T&& t)
{ return apply_impl(std::forward<F>(functor), std::forward<T>(t),
                    std::make_index_sequence<std::tuple_size_v<std::remove_reference_t<T>>>{}); }

template<class A, class... B> struct Visitor : Visitor<A>, Visitor<B...> {
    Visitor(A a, B... b) : Visitor<A>(a), Visitor<B...>(b...) { }
    using Visitor<A>::operator(); using Visitor<B...>::operator();
};
template<class A> struct Visitor<A> : A { Visitor(A a) : A(a) { } using A::operator(); };
template<class... F> [[gnu::always_inline]] inline Visitor<F...> makeVisitor(F... f) { return Visitor<F...>(f...); }
template<typename Vis, typename... Vs> constexpr auto wtf_visit(Vis&& v, Vs&&... vs)
{ return std::visit(std::forward<Vis>(v), std::forward<Vs>(vs)...); }
template<class V, class... F> [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return wtf_visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

// 类模板运算节点,各以 tuple<Child*...> 持子(指针 -> 完整类型,apply 可展开)
struct Child;
template<class T> struct N { T v; };
struct Sum    { std::tuple<Child*, Child*> kids; };
struct Product{ std::tuple<Child*, Child*> kids; };
struct Negate { std::tuple<Child*> kids; };
struct Min    { std::tuple<Child*, Child*, Child*> kids; };

template<typename Op> struct IndirectNode { Op* op; const Op& operator*() const { return *op; } };

using Node = std::variant<
    N<int>, N<long>, N<short>, N<char>, N<double>, N<float>, N<unsigned>, N<bool>,
    IndirectNode<Sum>, IndirectNode<Product>, IndirectNode<Negate>, IndirectNode<Min>>;

struct Child { Node value; template<class T> Child(T&& t): value(std::forward<T>(t)) {} };

// copy 重载簇
inline Child copy(const Child& root);
inline Child* copy(Child* p) { return p; }   // 让 copy(x) 对 Child* 有定义
template<class T> Child copy(const N<T>& n) { return Child { n }; }

// 关键:变参泛型 lambda,内部 copy(x)... 包展开,构造 Op,再包成 IndirectNode<Op>
template<typename Op> Child copy(const IndirectNode<Op>& root)
{
    Op newOp = apply([](const auto&... x) { return Op { { copy(x)... } }; }, (*root).kids);
    return Child { IndirectNode<Op> { new Op(newOp) } };
}

inline Child copy(const Child& root)
{
    return switchOn(root.value, [&](const auto& alt) -> Child { return copy(alt); });
}

Child run(const Child& c) { return copy(c); }
