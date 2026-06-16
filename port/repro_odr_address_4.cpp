// repro_odr_address_4: requires 约束结构(repro3) + 角度C 强制发射:
// 在变参模板 isCalc<Ts...> 内对 switchOn 的"含未展开 F... 约束"的实例取地址，
// 逼 clang 修饰 switchOn 模板实例的 out-of-line 符号 → 修饰约束/闭包时撞墙。
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

// volatile sink，防优化把取址常量折叠掉
void* volatile g_sink;

template<typename... Ts> bool isCalc(const std::variant<Ts...>& component)
{
    // 在变参上下文里定义闭包，并显式取 switchOn 该实例的地址（ODR-use by address）
    auto lam = [&](auto alternative) -> bool { return isCalc(alternative); };
    using V = const std::variant<Ts...>&;
    auto fp = &switchOn<V, decltype(lam)&>;   // 取地址 → 必须 out-of-line 发射 switchOn<...>
    g_sink = reinterpret_cast<void*>(fp);
    return switchOn(component, lam);
}

using Inner = std::variant<int, double>;
using Outer = std::variant<Leaf, Inner>;
Outer g;
bool run() { return isCalc(g); }
