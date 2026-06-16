// repro_odr_address_2: 完整还原 WTF::switchOn 形态 —— 带 requires(!HasSwitchOn<V>) 约束、
// 两个重载、asVariant 包装。WebKit 真实墙就发在 switchOn 声明行 (StdLibExtras.h:579),
// clang 试图修饰带未展开 F... 的函数模板(约束子句)→ "cannot mangle this pack expansion yet"。
// 角度C:再叠加取地址/类型擦除逼迫 out-of-line 发射。
#include <variant>
#include <optional>
#include <utility>
#include <functional>

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

// 关键:HasSwitchOn 概念 + 两个带 requires 的 switchOn 重载(WebKit 真实结构)
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

using Var = std::variant<Leaf, int>;
Var g;

// 角度C：经 std::function 类型擦除调用 switchOn → 逼出 out-of-line 闭包/符号
std::function<bool(const Var&)> erased = [](const Var& v) -> bool { return isCalc(v); };

bool run() { return erased(g); }
