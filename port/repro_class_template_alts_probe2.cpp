// probe2:逼修饰"变参泛型 lambda 闭包"(operator() 为 auto... 变参模板)作为模板实参。
#include <variant>
#include <utility>
#include <tuple>

template<class... F> struct Visitor : F... { using F::operator()...; };
template<class... F> Visitor(F...) -> Visitor<F...>;

// 变参函数模板,把闭包当模板实参 F... 传入,noinline 逼发射
template<class V, class... F> [[gnu::noinline]] auto switchOn(V&& v, F&&... f)
{ return std::visit(Visitor{ std::forward<F>(f)... }, std::forward<V>(v)); }

using Var = std::variant<int, double, char>;
Var g;

// 关键:lambda 自身带变参 auto 包 [](const auto&... xs)
int run()
{
    return switchOn(g, [](const auto&... xs) -> int { return (0 + ... + (int)sizeof(xs)); });
}
