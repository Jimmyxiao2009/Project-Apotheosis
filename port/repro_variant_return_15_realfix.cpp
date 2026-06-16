// repro_variant_return_15: 真正解法 —— 去掉 switchOn 上的 requires 约束(改用普通重载/tag 派发),
// 其余结构(变参成员 switchOn + 变体返回 + 递归)不变。验证墙消失。
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

// 解法: 无 requires —— 直接对 std::variant<Ts...> 与 VariantSubclass<Ts...> 各给一个非约束重载
template<class... Ts, class... F> WTF_VISITOR_INTERNAL auto switchOn(const std::variant<Ts...>& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), v); }

template<class... Ts> struct VariantSubclass {
    std::variant<Ts...> inner;
    template<class... F> auto switchOn(F&&... f) const { return ::switchOn(inner, std::forward<F>(f)...); }
};
// VariantSubclass 重载: 直接转调成员, 无 requires
template<class... Ts, class... F> WTF_VISITOR_INTERNAL auto switchOn(const VariantSubclass<Ts...>& v, F&&... f)
{ return v.switchOn(std::forward<F>(f)...); }

struct Leaf {};
Leaf   id(const Leaf&) { return {}; }
int    id(int v) { return v; }
double id(double v) { return v; }

template<class T> auto simplify(const T& x) { return id(x); }
template<class... Ts> auto simplify(const VariantSubclass<Ts...>& v) -> std::variant<Ts...>
{ return switchOn(v, [&](auto alt) -> std::variant<Ts...> { return simplify(alt); }); }

VariantSubclass<Leaf, int, double> g;
auto run() { return simplify(g); }
