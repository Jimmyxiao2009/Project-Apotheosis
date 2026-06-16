// repro_variant_return_2: 角度A + 强制 switchOn 不被内联(去 always_inline)+ 变体返回类型。
// switchOn 一旦被发射成真符号, 其 auto 推导的 std::variant<Ts...> 返回类型要被 MSVC 修饰,
// 该返回类型来自 Ts... 包 -> 撞墙。
#include <variant>
#include <optional>
#include <utility>

template<class A, class... B> struct Visitor : Visitor<A>, Visitor<B...> {
    Visitor(A a, B... b) : Visitor<A>(a), Visitor<B...>(b...) { }
    using Visitor<A>::operator();
    using Visitor<B...>::operator();
};
template<class A> struct Visitor<A> : A {
    Visitor(A a) : A(a) { }
    using A::operator();
};
template<class... F> Visitor<F...> makeVisitor(F... f) { return Visitor<F...>(f...); }
// 关键: 去掉 always_inline/inline, 让 switchOn 作为可发射符号 -> 其变体返回类型被修饰
template<class V, class... F> auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

struct Leaf { };
Leaf simplify(const Leaf& l) { return l; }
int  simplify(int v) { return v; }
double simplify(double v) { return v; }

template<typename... Ts>
std::variant<Ts...> simplify(const std::variant<Ts...>& component)
{
    return switchOn(component,
        [&](const auto& alternative) -> std::variant<Ts...> { return simplify(alternative); });
}

using Inner = std::variant<int, double>;
using Outer = std::variant<Leaf, Inner>;
Outer g;
Outer run() { return simplify(g); }
