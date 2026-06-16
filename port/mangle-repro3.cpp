// repro3: 泛型 lambda([](const auto&){}) 仿 WebKit CSSCalcTree 风格,测是否触发 pack-expansion 墙。
#include <variant>
#include <utility>
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
template<class V, class... F> [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

struct A { int x; }; struct B { int y; }; struct C { int z; };
using Var = std::variant<A, B, C>;

// 仿 WebKit:在另一个模板函数里、用单个泛型 lambda 调 switchOn
template<class Fn> int forAll(const Var& v, Fn&& fn)
{
    return switchOn(v, [&](const auto& alt) { return fn(&alt); });
}
int run()
{
    Var v { A { 1 } };
    return forAll(v, [](const auto*) { return 7; });
}
