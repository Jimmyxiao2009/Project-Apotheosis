// repro_variant_return_10: 触发 HasSwitchOn 重载(line586)—— 类型自带变参成员 .switchOn(F...),
// 使受约束的 switchOn 重载尾置 decltype(v.switchOn(F...)) 里 pack 真正存活到修饰。
#include <variant>
#include <optional>
#include <utility>
#include <type_traits>

#define ALWAYS_INLINE [[gnu::always_inline]] inline
#define WTF_VISITOR_INTERNAL ALWAYS_INLINE

template<class A, class... B> struct Visitor : Visitor<A>, Visitor<B...> {
    Visitor(A a, B... b) : Visitor<A>(a), Visitor<B...>(b...) { }
    using Visitor<A>::operator(); using Visitor<B...>::operator();
};
template<class A> struct Visitor<A> : A { Visitor(A a) : A(a) { } using A::operator(); };
template<class... F> WTF_VISITOR_INTERNAL Visitor<F...> makeVisitor(F... f) { return Visitor<F...>(f...); }

template<class... Ts> ALWAYS_INLINE constexpr std::variant<Ts...>& asVariant(std::variant<Ts...>& v) { return v; }
template<class... Ts> ALWAYS_INLINE constexpr const std::variant<Ts...>& asVariant(const std::variant<Ts...>& v) { return v; }

template<typename T> concept HasSwitchOn = requires(T t) { t.switchOn([](const auto&) {}); };

// 重载1: !HasSwitchOn
template<class V, class... F> requires (!HasSwitchOn<V>) WTF_VISITOR_INTERNAL auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), asVariant(std::forward<V>(v))); }
// 重载2: HasSwitchOn, 尾置 decltype(v.switchOn(F...)) —— pack 在 decltype
template<class V, class... F> requires (HasSwitchOn<V>) WTF_VISITOR_INTERNAL auto switchOn(V&& v, F&&... f) -> decltype(v.switchOn(std::forward<F>(f)...))
{ return v.switchOn(std::forward<F>(f)...); }

// 一个带变参成员 switchOn 的"变体子类"型, 命中重载2
template<class... Ts> struct VariantSubclass {
    std::variant<Ts...> inner;
    template<class... F> auto switchOn(F&&... f) const -> decltype(::switchOn(inner, std::forward<F>(f)...))
    { return ::switchOn(inner, std::forward<F>(f)...); }
};

struct Leaf {};
Leaf   id(const Leaf&) { return {}; }
int    id(int v) { return v; }
double id(double v) { return v; }

// 真实惯用法: 递归 + 变体返回, 经由 VariantSubclass 命中 HasSwitchOn 重载
template<class T> auto simplify(const T& x) { return id(x); }
template<class... Ts> auto simplify(const VariantSubclass<Ts...>& v) -> std::variant<Ts...>
{ return switchOn(v, [&](auto alt) -> std::variant<Ts...> { return simplify(alt); }); }

using Inner = std::variant<int, double>;
VariantSubclass<Leaf, int, double> g;
auto run() { return simplify(g); }
