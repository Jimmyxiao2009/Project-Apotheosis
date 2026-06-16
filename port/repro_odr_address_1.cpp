// repro_odr_address_1: 角度C — 强制发射 switchOn(对其取地址→必须 out-of-line)。
// always_inline 的 switchOn 一旦被 ODR-used by address,链接器需要其 out-of-line 定义,
// 编译器必须修饰 switchOn<...> 及其内含的"定义在变参模板内、修饰名引用未展开 Ts... 的 lambda"。
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

// 依赖变体 + 递归泛型 lambda(CSSUnevaluatedCalc 形态)。
// 关键:这里我们要拿到 switchOn 的具体实例化指针,逼迫它 out-of-line。
template<typename... Ts> bool isCalc(const std::variant<Ts...>& component)
{
    return switchOn(component, [&](auto alternative) -> bool { return isCalc(alternative); });
}

using Var = std::variant<Leaf, int>;
Var g;

// 显式取 switchOn 的地址:用一个与该实例签名匹配的函数指针。
// switchOn<const Var&, lambda&> —— lambda 是在 isCalc<Leaf,int> 内定义的、修饰名含 Ts... 的那个。
// 通过 isCalc 内部把 &switchOn 暴露出来。
template<typename... Ts>
auto* addrOfSwitchOn(const std::variant<Ts...>& component)
{
    auto lam = [&](auto alternative) -> bool { return isCalc(alternative); };
    // 取 switchOn 实例的地址 —— ODR-use by address，强制 out-of-line 发射
    return static_cast<bool(*)(const std::variant<Ts...>&, decltype(lam)&)>(
        &switchOn<const std::variant<Ts...>&, decltype(lam)&>);
}

bool run()
{
    auto* p = addrOfSwitchOn(g);
    return p != nullptr;
}
