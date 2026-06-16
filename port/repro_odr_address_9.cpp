// repro_odr_address_9: 修复探针 —— 把传给 switchOn 的"匿名 lambda"换成
// 命名空间作用域的具名仿函数(其 operator() 为模板,但仿函数类型本身不含未展开包)。
// 同时 HasSwitchOn 概念探测里的 lambda 也换成具名仿函数。看 pack-expansion 墙是否消失。
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

// 命名空间作用域具名仿函数(概念探测用,替代 [](const auto&){})
struct ProbeFunctor { template<typename T> void operator()(const T&) const { } };
template<typename T> concept HasSwitchOn = requires(T t) { t.switchOn(ProbeFunctor{}); };

template<class V, class... F> requires (!HasSwitchOn<V>) [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }
template<class V, class... F> requires (HasSwitchOn<V>) [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return v.switchOn(std::forward<F>(f)...); }

template<typename Raw>
struct PrimitiveNumeric {
    Raw raw {};
    int calcv {};
    bool isCalc() const { return false; }
    template<typename... F> decltype(auto) switchOn(F&&... f) const
    {
        auto visitor = makeVisitor(std::forward<F>(f)...);
        if (isCalc())
            return visitor(calcv);
        return visitor(raw);
    }
};

struct NumberRaw { double v; };
using Number = PrimitiveNumeric<NumberRaw>;

// 命名空间作用域具名仿函数,替代传给 switchOn 的两个 lambda
struct OnRaw { double operator()(NumberRaw r) const { return r.v; } };
struct OnCalc { double operator()(int c) const { return double(c); } };

double useIt(const Number& n)
{
    return switchOn(n, OnRaw{}, OnCalc{});
}

Number g;
double run() { return useIt(g); }
