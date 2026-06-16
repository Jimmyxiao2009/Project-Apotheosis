// repro_odr_address_3: 隔离变量 —— 在 repro4/repro5(未复现)基础上,只新增 WebKit 真实结构的
// requires(!HasSwitchOn<V>) 约束 + 第二个 HasSwitchOn 重载 + asVariant。无 std::function(避免 ARM 后端 EH 崩)。
// 假设:墙的触发因子是"带 requires 约束子句的变参 switchOn 模板",clang 修饰约束时撞未展开 F...。
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

template<class... Ts> [[gnu::always_inline]] constexpr const std::variant<Ts...>& asVariant(const std::variant<Ts...>& v) { return v; }
template<class... Ts> [[gnu::always_inline]] constexpr std::variant<Ts...>& asVariant(std::variant<Ts...>& v) { return v; }

template<typename T> concept HasSwitchOn = requires(T t) { t.switchOn([](const auto&) {}); };

template<class V, class... F> requires (!HasSwitchOn<V>) [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), asVariant(std::forward<V>(v))); }

template<class V, class... F> requires (HasSwitchOn<V>) [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f) -> decltype(v.switchOn(std::forward<F>(f)...))
{ return v.switchOn(std::forward<F>(f)...); }

struct Leaf { };
bool isCalc(const Leaf&) { return false; }
bool isCalc(int) { return false; }

template<typename... Ts> bool isCalc(const std::variant<Ts...>& component)
{
    return switchOn(component, [&](auto alternative) -> bool { return isCalc(alternative); });
}

using Inner = std::variant<int, double>;
using Outer = std::variant<Leaf, Inner>;
Outer g;
bool run() { return isCalc(g); }
