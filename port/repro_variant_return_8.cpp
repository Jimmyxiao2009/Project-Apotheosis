// repro_variant_return_8: 1:1 复刻 CSS::replaceSymbol —— 返回类型不是 Variant<Ts...>,
// 而是 TypesMinusSymbol<Ts...> = VariantOrSingle<MinusSymbol<brigand::list<Ts...>>>。
// 即一个对 pack 做 remove_if + wrap 的元函数链作为 switchOn/泛型 lambda 的返回类型。
// 这是真实失败函数的精确惯用法。最小复刻 brigand 的 list/size/front/wrap/remove_if。
#include <variant>
#include <optional>
#include <utility>
#include <type_traits>

// ---- 脚手架: switchOn / makeVisitor / Visitor ----
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

// ---- 最小 brigand ----
namespace brigand {
template<class... T> struct list {};
// size
template<class L> struct size_impl;
template<template<class...> class L, class... T> struct size_impl<L<T...>> : std::integral_constant<unsigned, sizeof...(T)> {};
template<class L> using size = size_impl<L>;
// front
template<class L> struct front_impl;
template<template<class...> class L, class T, class... U> struct front_impl<L<T, U...>> { using type = T; };
template<class L> using front = typename front_impl<L>::type;
// wrap: A<T...> + B  -> B<T...>
template<class A, template<class...> class B> struct wrap_impl;
template<template<class...> class A, class... T, template<class...> class B> struct wrap_impl<A<T...>, B> { using type = B<T...>; };
template<class A, template<class...> class B> using wrap = typename wrap_impl<A, B>::type;
// remove_if: 从 list<Ts...> 去掉满足谓词的元素
template<class L, template<class> class Pred> struct remove_if_impl;
template<template<class...> class L, template<class> class Pred> struct remove_if_impl<L<>, Pred> { using type = L<>; };
template<template<class...> class L, class T, class... Ts, template<class> class Pred>
struct remove_if_impl<L<T, Ts...>, Pred> {
    using rest = typename remove_if_impl<L<Ts...>, Pred>::type;
    template<class Head, class R> struct prepend;
    template<class Head, template<class...> class LL, class... Rs> struct prepend<Head, LL<Rs...>> { using type = LL<Head, Rs...>; };
    using type = std::conditional_t<Pred<T>::value, rest, typename prepend<T, rest>::type>;
};
template<class L, template<class> class Pred> using remove_if = typename remove_if_impl<L, Pred>::type;
} // namespace brigand

// ---- 复刻 WTF::VariantOrSingle ----
template<class... Ts> using VariantWrapper = std::variant<Ts...>;
template<class TypeList> using VariantOrSingle = std::conditional_t<
    brigand::size<TypeList>::value == 1,
    brigand::front<TypeList>,
    brigand::wrap<TypeList, VariantWrapper>
>;

// ---- 复刻 Symbol / MinusSymbol / TypesMinusSymbol ----
struct Symbol { };
template<class T> struct IsSymbolPred : std::is_same<T, Symbol> {};
template<class TypeList> using MinusSymbol = brigand::remove_if<TypeList, IsSymbolPred>;
template<class... Ts> using TypesMinusSymbol = VariantOrSingle<MinusSymbol<brigand::list<Ts...>>>;

// ---- 复刻 replaceSymbol 三/四重载 ----
struct Number { double v{}; };
Number replaceSymbol(Symbol, int) { return {}; }
template<class T> constexpr auto replaceSymbol(T value, int) -> T { return value; }

// 核心: 返回 TypesMinusSymbol<Ts...>(对 pack 的元函数链), lambda 同返回类型
template<typename... Ts>
constexpr auto replaceSymbol(const std::variant<Ts...>& component, int table) -> TypesMinusSymbol<Ts...>
{
    return switchOn(component, [&](auto part) -> TypesMinusSymbol<Ts...> { return replaceSymbol(part, table); });
}
template<typename T>
constexpr decltype(auto) replaceSymbol(const std::optional<T>& component, int table)
{
    return component ? std::make_optional(replaceSymbol(*component, table)) : std::nullopt;
}

using V = std::variant<Number, Symbol, int>;
auto run() { V g; return replaceSymbol(g, 0); }
