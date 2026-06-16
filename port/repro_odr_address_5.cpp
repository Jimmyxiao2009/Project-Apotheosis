// repro_odr_address_5: 角度C 变体 —— 逼"变参模板内的泛型 lambda"自身 out-of-line 发射。
// 关键洞见:MS-ABI 闭包修饰名编码其词法上下文(外围函数签名)。若外围是变参模板 isCalc<Ts...>
// 或 switchOn(F&&...),闭包修饰名内嵌 pack expansion → 撞墙。对 lambda::operator() 取地址逼其出 line。
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
bool isCalc(const Leaf&) { return false; }
bool isCalc(int) { return false; }
bool isCalc(double) { return false; }

void* volatile g_sink;

// 变参模板内定义泛型 lambda，且对它的某个具体 operator() 取地址 → 闭包必须 out-of-line。
// 闭包修饰名内嵌外围 isCalc<Ts...> 上下文(含 Ts... pack)→ 期望撞墙。
template<typename... Ts> bool isCalc(const std::variant<Ts...>& component)
{
    auto alt = [&](auto alternative) -> bool { return isCalc(alternative); };
    // 强制实例化并取 lambda::operator()<Leaf> 的地址
    using LamT = decltype(alt);
    auto mp = static_cast<bool (LamT::*)(Leaf) const>(&LamT::template operator()<Leaf>);
    (void)mp;
    g_sink = *reinterpret_cast<void**>(&mp);
    return switchOn(component, alt);
}

using Inner = std::variant<int, double>;
using Outer = std::variant<Leaf, Inner>;
Outer g;
bool run() { return isCalc(g); }
