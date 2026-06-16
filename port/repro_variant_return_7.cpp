// repro_variant_return_7: 让生成的闭包/Visitor 成为必须修饰的真符号。
// 关键: 把变参模板里的泛型 lambda 经由一个"返回 std::variant<Ts...> 的函数指针"取地址,
// 使闭包 operator() 模板 + 其 std::variant<Ts...>(enclosing pack) 返回类型必须被修饰 -> PackExpansion。
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
template<class V, class... F> [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

struct Leaf { };
Leaf   simplify(const Leaf&) { return {}; }
int    simplify(int v) { return v; }
double simplify(double v) { return v; }

// 把泛型 lambda 通过 std::function<std::variant<Ts...>(...)> 做类型擦除,
// 强制实例化并修饰闭包的 operator()<Alt> 及其 std::variant<Ts...>(enclosing pack) 返回。
template<typename... Ts>
auto simplify(const std::variant<Ts...>& component) -> std::variant<Ts...>
{
    auto lam = [&](auto alternative) -> std::variant<Ts...> { return simplify(alternative); };
    // 类型擦除每一个 alternative 上的实例: 迫使闭包 operator() 被发射、修饰
    std::variant<Ts...> result = component;
    (void)std::initializer_list<int>{
        ( [&]{ std::function<std::variant<Ts...>(const Ts&)> f = lam; result = f(std::get<Ts>(component)); }(), 0 )...
    };
    return switchOn(component, lam);
}

using Inner = std::variant<int, double>;
using Outer = std::variant<Leaf, Inner>;
Outer g;
Outer run() { return simplify(g); }
