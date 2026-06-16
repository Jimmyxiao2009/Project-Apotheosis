// repro_variant_return_9: 复刻 StdLibExtras.h:579 真实失败点 ——
// 受 requires(!HasSwitchOn<V>) 约束的变参 switchOn 重载集 + 第三重载尾置 decltype(v.switchOn(F...))
// (pack 在 decltype 里存活成 PackExpansionType) + asVariant 间接 + 变体返回。
#include <variant>
#include <optional>
#include <utility>
#include <type_traits>

#define ALWAYS_INLINE [[gnu::always_inline]] inline
#define WTF_VISITOR_INTERNAL ALWAYS_INLINE

template<class A, class... B> struct Visitor : Visitor<A>, Visitor<B...> {
    Visitor(A a, B... b) : Visitor<A>(a), Visitor<B...>(b...) { }
    using Visitor<A>::operator();
    using Visitor<B...>::operator();
};
template<class A> struct Visitor<A> : A {
    Visitor(A a) : A(a) { }
    using A::operator();
};
template<class... F> WTF_VISITOR_INTERNAL Visitor<F...> makeVisitor(F... f) { return Visitor<F...>(f...); }

template<class... Ts> ALWAYS_INLINE constexpr std::variant<Ts...>& asVariant(std::variant<Ts...>& v) { return v; }
template<class... Ts> ALWAYS_INLINE constexpr const std::variant<Ts...>& asVariant(const std::variant<Ts...>& v) { return v; }
template<class... Ts> ALWAYS_INLINE constexpr std::variant<Ts...>&& asVariant(std::variant<Ts...>&& v) { return std::move(v); }
template<class... Ts> ALWAYS_INLINE constexpr const std::variant<Ts...>&& asVariant(const std::variant<Ts...>&& v) { return std::move(v); }

template<typename T> concept HasSwitchOn = requires(T t) { t.switchOn([](const auto&) {}); };

// 重载1: 非 HasSwitchOn, auto 返回
template<class V, class... F> requires (!HasSwitchOn<V>) WTF_VISITOR_INTERNAL auto switchOn(V&& v, F&&... f)
{
    return std::visit(makeVisitor(std::forward<F>(f)...), asVariant(std::forward<V>(v)));
}
// 重载2: HasSwitchOn, 尾置 decltype(v.switchOn(F...)) —— pack 在 decltype 里
template<class V, class... F> requires (HasSwitchOn<V>) WTF_VISITOR_INTERNAL auto switchOn(V&& v, F&&... f) -> decltype(v.switchOn(std::forward<F>(f)...))
{
    return v.switchOn(std::forward<F>(f)...);
}

// === 真实失败惯用法: replaceSymbol 返回 TypesMinusSymbol<Ts...> ===
struct Symbol { };
template<class T> struct IsSymbol : std::is_same<T, Symbol> {};
// 最小 brigand-ish typelist + remove + wrap
template<class... T> struct TList {};
template<class L> struct First; template<class T, class... U> struct First<TList<T, U...>> { using type = T; };
template<class L, class Acc = TList<>> struct Rm;
template<class Acc> struct Rm<TList<>, Acc> { using type = Acc; };
template<class T, class... U, class... A> struct Rm<TList<T, U...>, TList<A...>>
{ using type = std::conditional_t<IsSymbol<T>::value, typename Rm<TList<U...>, TList<A...>>::type, typename Rm<TList<U...>, TList<A..., T>>::type>; };
template<class L> struct Sz; template<class... T> struct Sz<TList<T...>> { static constexpr unsigned v = sizeof...(T); };
template<class L> struct Wrap; template<class... T> struct Wrap<TList<T...>> { using type = std::variant<T...>; };
template<class L> using VariantOrSingle = std::conditional_t<Sz<L>::v == 1, typename First<L>::type, typename Wrap<L>::type>;
template<class... Ts> using TypesMinusSymbol = VariantOrSingle<typename Rm<TList<Ts...>>::type>;

struct Number { double v{}; };
Number replaceSymbol(Symbol, int) { return {}; }
template<class T> constexpr auto replaceSymbol(T value, int) -> T { return value; }

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
