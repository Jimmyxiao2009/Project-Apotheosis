// repro_variant_return_1: 角度A —— switchOn 的 lambda 返回类型 = std::variant<Ts...>。
// 真实失败函数 simplifyUnevaluatedCalc 形态: 递归把一个变体映射成同型变体,
// lambda -> Variant<Ts...>, switchOn 的 auto 推导出变体返回类型 -> MSVC 修饰函数模板返回类型
// -> 该返回类型由 Ts... 包推导 -> 撞 "cannot mangle this pack expansion yet"。
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
template<class... F> [[gnu::always_inline]] inline Visitor<F...> makeVisitor(F... f) { return Visitor<F...>(f...); }
template<class V, class... F> [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

struct Leaf { };
Leaf simplify(const Leaf& l) { return l; }
int  simplify(int v) { return v; }
double simplify(double v) { return v; }

// simplifyUnevaluatedCalc 形态: 返回同型变体, lambda 返回 Variant<Ts...>
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
