// repro_odr_address_13: 探针 —— 概念里用 *非泛型* lambda(参数具体类型,无 auto)。
// 验证墙是否依赖"泛型 lambda(invented template param)"而非任意 lambda。
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

struct NumberRaw { double v; };

// 非泛型 lambda(具体参数类型)。注意:为让成员 switchOn(ProbeLambda) 可调,成员需能接受任意可调用。
template<typename T> concept HasSwitchOn = requires(T t) { t.switchOn([](const NumberRaw&) {}); };

template<class V, class... F> requires (!HasSwitchOn<V>) [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return makeVisitor(std::forward<F>(f)...)(v); }
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

using Number = PrimitiveNumeric<NumberRaw>;

double useIt(const Number& n)
{
    return switchOn(n,
        [](NumberRaw r) -> double { return r.v; },
        [](int c) -> double { return double(c); });
}

Number g;
double run() { return useIt(g); }
