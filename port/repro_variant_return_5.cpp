// repro_variant_return_5: 1:1 复刻 CSSUnevaluatedCalc 的 simplifyUnevaluatedCalc 三/四重载
// (Variant<Ts...> 返回 + std::optional<T> 递归 + 概念约束式叶子 + 泛型 [&](auto alt) lambda),
// 但用 switchOn(变参模板)而非 std::visit -> 逼出墙。
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

// === 叶子类型: 仿 Calc<T> 概念 ===
struct CalcLeaf { CalcLeaf simplify() const { return *this; } };
template<class T> constexpr bool isCalc = false;
template<> constexpr bool isCalc<CalcLeaf> = true;

// Calc 叶子 (返回 T)
template<class T> auto simplifyUnevaluatedCalc(const T& v) -> T requires(isCalc<T>)
{ return v.simplify(); }
// 非 Calc 叶子 (返回 T)
template<class T> auto simplifyUnevaluatedCalc(const T& v) -> T requires(!isCalc<T>)
{ return v; }

// Variant<Ts...> -> Variant<Ts...> (核心: 泛型 lambda + switchOn + 变体返回)
template<typename... Ts>
auto simplifyUnevaluatedCalc(const std::variant<Ts...>& component) -> std::variant<Ts...>
{
    return switchOn(component,
        [&](auto alternative) -> std::variant<Ts...> { return simplifyUnevaluatedCalc(alternative); });
}

// std::optional<T> 递归 (制造跨实例深递归, 阻止完全内联)
template<typename T>
decltype(auto) simplifyUnevaluatedCalc(const std::optional<T>& component)
{
    return component ? std::make_optional(simplifyUnevaluatedCalc(*component)) : std::nullopt;
}

using Inner = std::variant<CalcLeaf, int>;
using Mid   = std::variant<Inner, double, std::optional<Inner>>;
using Outer = std::variant<Mid, CalcLeaf, std::optional<Mid>>;
Outer g;
Outer run() { return simplifyUnevaluatedCalc(g); }
