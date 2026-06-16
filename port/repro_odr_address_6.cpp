// repro_odr_address_6: 真实 CSSPrimitiveNumeric 形态 ——
//   类模板 PrimitiveNumeric<Raw> 内含 *变参成员模板* switchOn(F&&... f)（调 makeVisitor）；
//   free HasSwitchOn 概念探测该成员；两个 WTF::switchOn 重载(requires HasSwitchOn / !HasSwitchOn)。
// 角度C：对该成员 switchOn 取地址 / 经函数指针,逼其 out-of-line → 修饰"类模板内变参成员模板 + 闭包"撞墙。
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
template<class V, class... F> requires (HasSwitchOn<V>) [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f) -> decltype(v.switchOn(std::forward<F>(f)...))
{ return v.switchOn(std::forward<F>(f)...); }

// 类模板内的变参成员模板 switchOn —— 关键还原 CSSPrimitiveNumeric::switchOn
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
    // 经 free switchOn → 走 HasSwitchOn 重载 → 调成员 n.switchOn(...)
    return switchOn(n,
        [](NumberRaw r) -> double { return r.v; },
        [](int c) -> double { return double(c); });
}

Number g;
double run() { return useIt(g); }
