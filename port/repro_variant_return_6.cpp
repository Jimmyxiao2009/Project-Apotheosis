// repro_variant_return_6: 角度A + 类模板备选(仿 UnevaluatedCalc<RawType>/PrimitiveNumeric<...>)。
// 备选自身是类模板特化, 其中一个由 pack 参数化, 配合变体返回 + switchOn + 递归泛型 lambda。
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

// 类模板备选(仿 UnevaluatedCalc<Raw> / PrimitiveNumeric<Range, V>)
template<class Raw> struct Numeric { Raw v{}; Numeric simplify() const { return *this; } };
template<class... Components> struct Composite { std::variant<Components...> child; };

template<class T> constexpr bool isCalc = false;
template<class Raw> constexpr bool isCalc<Numeric<Raw>> = true;

template<class T> auto simplify(const T& v) -> T requires(isCalc<T>) { return v.simplify(); }
template<class T> auto simplify(const T& v) -> T requires(!isCalc<T> && !requires { typename T::is_composite; }) { return v; }

// Variant<Ts...> -> Variant<Ts...> : 核心墙候选
template<typename... Ts>
auto simplify(const std::variant<Ts...>& component) -> std::variant<Ts...>
{
    return switchOn(component,
        [&](auto alternative) -> std::variant<Ts...> { return simplify(alternative); });
}

struct RawA { int a{}; }; struct RawB { double b{}; };
using Leaf1 = Numeric<RawA>;
using Leaf2 = Numeric<RawB>;
using V1 = std::variant<Leaf1, Leaf2, int>;
using V2 = std::variant<V1, Leaf1, double>;
using V3 = std::variant<V2, V1, Leaf2>;
V3 g;
V3 run() { return simplify(g); }
