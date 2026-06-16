// repro_odr_address_12: 最小化 —— 删掉 !HasSwitchOn 重载,只留 HasSwitchOn 重载 + 成员 switchOn 类。
// 并把概念里的 lambda 保持为泛型 [](const auto&){}。验证墙仍在(确认 !HasSwitchOn 重载非必需)。
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

// 仅此一个 free switchOn 重载
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
