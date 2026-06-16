// repro_variant_return_3: 取 switchOn / makeVisitor 的地址 -> 强制发射带完整签名修饰的符号,
// 其变参签名(F...)+变体返回 -> 撞包展开。同时返回变体。
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

struct Leaf { };
Leaf   simplify(const Leaf& l) { return l; }
int    simplify(int v) { return v; }
double simplify(double v) { return v; }

template<typename... Ts>
std::variant<Ts...> simplify(const std::variant<Ts...>& component)
{
    return switchOn(component,
        [&](const auto& alternative) -> std::variant<Ts...> { return simplify(alternative); });
}

using Inner = std::variant<int, double>;
using Outer = std::variant<Leaf, Inner>;

// 取地址: 把 switchOn 的特定实例钉成必须修饰的真符号
using LamT = decltype([](const int&) -> std::variant<int, double> { return 0; });
auto* p1 = static_cast<std::variant<int,double>(*)(Inner&&, LamT&&)>(&switchOn<Inner, LamT>);

Outer g;
Outer run() { return simplify(g); }
