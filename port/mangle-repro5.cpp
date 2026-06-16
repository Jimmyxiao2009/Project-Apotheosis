// repro5: 嵌套变体 + 递归泛型 lambda —— 强制 switchOn 递归 >1 层、被发射成符号,
// 复现 "cannot mangle this pack expansion yet"(repro4 因只递归 1 层被完全内联而未复现)。
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
bool requiresConversionData(const Leaf&) { return false; }
bool requiresConversionData(int) { return false; }
bool requiresConversionData(double) { return false; }

// 依赖变体 + 递归泛型 lambda(CSSUnevaluatedCalc 形态)
template<typename... Ts> bool requiresConversionData(const std::variant<Ts...>& component)
{
    return switchOn(component, [&](auto alternative) -> bool { return requiresConversionData(alternative); });
}
// 嵌套变体:外层 alternative 自身又是变体 → requiresConversionData 递归进内层 → switchOn 跨实例互递归
using Inner = std::variant<int, double>;
using Outer = std::variant<Leaf, Inner>;
Outer g;
bool run() { return requiresConversionData(g); }
