// repro_odr_address_8: 探针 —— 把 HasSwitchOn 重载的尾置 decltype 去掉(改 auto 推导)。
// 目的:判定墙到底来自"尾置 decltype 内的 pack expansion"还是来自"成员 switchOn(F&&...) 变参模板本身的修饰"。
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

template<typename T> concept HasSwitchOn = requires(T t) { t.switchOn([](const auto&) {}); };

template<class V, class... F> requires (!HasSwitchOn<V>) [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }
// 去掉尾置 decltype，改纯 auto 推导
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

double useIt(const Number& n)
{
    return switchOn(n,
        [](NumberRaw r) -> double { return r.v; },
        [](int c) -> double { return double(c); });
}

Number g;
double run() { return useIt(g); }
