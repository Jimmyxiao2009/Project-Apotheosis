// Apotheosis: 最小复现 clang thumbv7-windows-msvc 的 "cannot mangle this pack expansion yet"。
// 仿 WTF::switchOn / makeVisitor(递归 Visitor)+ std::visit 的变参访问模式。
// 用法:clang-cl --target=thumbv7-unknown-windows-msvc -c mangle-repro.cpp
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
{
    return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v));
}

using Var = std::variant<int, double, char>;
Var g;
int run()
{
    return switchOn(g, [](int) { return 1; }, [](double) { return 2; }, [](char) { return 3; });
}
