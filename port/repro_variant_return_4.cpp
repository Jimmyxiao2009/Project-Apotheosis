// repro_variant_return_4: 显式实例化 switchOn(变参 + 变体返回) -> 强制其修饰全签名。
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
template<class V, class... F> auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

using Inner = std::variant<int, double>;

struct L1 { std::variant<int,double> operator()(const int&) const { return 0; } };
struct L2 { std::variant<int,double> operator()(const double&) const { return 0.0; } };

// 显式实例化: 强制 switchOn<Inner, L1, L2> 这个变参实例被完整修饰发射
template std::variant<int,double> switchOn<Inner&, L1, L2>(Inner&, L1&&, L2&&);

Inner g;
std::variant<int,double> run() { return switchOn(g, L1{}, L2{}); }
