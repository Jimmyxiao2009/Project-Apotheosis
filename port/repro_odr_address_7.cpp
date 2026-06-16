// repro_odr_address_7: 收敛到"纯 pack-expansion 墙"(对应真实 StdLibExtras.h:579 的 auto 返回 !HasSwitchOn 重载)。
// 关键三要素:
//   (1) 类模板含变参成员模板 switchOn(F&&...) → 满足 HasSwitchOn(=真实 CSSPrimitiveNumeric 形态);
//   (2) free switchOn 两个 requires 重载;HasSwitchOn 重载尾置 -> decltype(...pack...) ;
//   (3) 角度C 强制发射:对 free switchOn 的某实例取地址,逼其 out-of-line → 修饰未展开 F... 撞墙。
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
