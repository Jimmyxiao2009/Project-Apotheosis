// repro_variant_return_10_fix: 把 repro10 的"尾置 decltype(v.switchOn(F...))"换成
// auto 返回推导(WebKit 实际采用的解法), 看墙是否消失。
// 另外把递归 lambda 换成命名空间作用域具名仿函数(operator() 模板按非包 alt 参数化, 返回具体变体)。
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

template<class V, class... F> requires (!HasSwitchOn<V>) WTF_VISITOR_INTERNAL auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), asVariant(std::forward<V>(v))); }
// 解法: 去掉尾置 decltype(v.switchOn(F...)), 用 auto 返回推导(pack 不再进 decltype)
template<class V, class... F> requires (HasSwitchOn<V>) WTF_VISITOR_INTERNAL auto switchOn(V&& v, F&&... f)
{ return v.switchOn(std::forward<F>(f)...); }

template<class... Ts> struct VariantSubclass {
    std::variant<Ts...> inner;
    template<class... F> auto switchOn(F&&... f) const
    { return ::switchOn(inner, std::forward<F>(f)...); }   // 同样去 decltype
};

struct Leaf {};
Leaf   id(const Leaf&) { return {}; }
int    id(int v) { return v; }
double id(double v) { return v; }

// 具名仿函数: 按非包 alt 参数化, 返回具体变体 Ret
template<class Ret> struct SimplifyVisitor {
    template<class Alt> Ret operator()(Alt alt) const;
};

template<class T> auto simplify(const T& x) { return id(x); }
template<class... Ts> auto simplify(const VariantSubclass<Ts...>& v) -> std::variant<Ts...>
{ return switchOn(v, SimplifyVisitor<std::variant<Ts...>>{}); }

template<class Ret> template<class Alt> Ret SimplifyVisitor<Ret>::operator()(Alt alt) const { return simplify(alt); }

VariantSubclass<Leaf, int, double> g;
auto run() { return simplify(g); }
