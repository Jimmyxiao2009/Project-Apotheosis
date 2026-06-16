// repro4: 仿 CSSUnevaluatedCalc 的"依赖变体 Variant<Ts...> + 递归泛型 lambda"模式。
#include <variant>
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
template<class... F> [[gnu::always_inline]] inline Visitor<F...> makeVisitor(F... f) { return Visitor<F...>(f...); }
template<class V, class... F> [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

struct Leaf { };
bool isCalc(const Leaf&) { return false; }
bool isCalc(int) { return false; }
// 依赖变体 + 递归泛型 lambda(回调自身),正是 CSSUnevaluatedCalc 的形态
template<typename... Ts> bool isCalc(const std::variant<Ts...>& component)
{
    return switchOn(component, [&](auto alternative) -> bool { return isCalc(alternative); });
}

using Var = std::variant<Leaf, int>;
Var g;
bool run() { return isCalc(g); }
