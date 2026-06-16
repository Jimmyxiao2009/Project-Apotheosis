// repro_class_template_alts_6: 强制发射 switchOn 本体并修饰其名字。
// switchOn 设 noinline(模拟真实里因递归环无法折叠而被发射的情形),且取其地址。
// 多 lambda(F... 多元包)+ 38 类模板备选 + 变参包装层。若 switchOn<V,F...> 的修饰名
// 含不可修饰包展开,这里就撞墙。
#include <variant>
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
template<typename Vis, typename... Variants> constexpr auto wtf_visit(Vis&& v, Variants&&... values)
{ return std::visit(std::forward<Vis>(v), std::forward<Variants>(values)...); }

// switchOn 强制 noinline -> 必被发射为独立符号 -> 必修饰 switchOn<V, F...> 名字
template<class V, class... F> [[gnu::noinline]] auto switchOn(V&& v, F&&... f)
{ return wtf_visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

template<class T> struct N { T v; };
struct R0 {}; struct R1 {}; struct R2 {}; struct R3 {}; struct R4 {};
struct R5 {}; struct R6 {}; struct R7 {}; struct R8 {}; struct R9 {};
using Big = std::variant<
    N<R0>, N<R1>, N<R2>, N<R3>, N<R4>, N<R5>, N<R6>, N<R7>, N<R8>, N<R9>,
    N<int>, N<long>, N<short>, N<char>, N<double>, N<float>, N<unsigned>, N<signed>,
    N<R0*>, N<R1*>, N<R2*>, N<R3*>, N<R4*>, N<R5*>, N<R6*>, N<R7*>, N<R8*>, N<R9*>,
    N<int*>, N<long*>, N<short*>, N<char*>, N<double*>, N<float*>, N<unsigned*>, N<signed*>,
    N<void*>, N<bool>>;

Big gv;
int run()
{
    return switchOn(gv,
        [](const N<R0>&) -> int { return 0; },
        [](const N<R1>&) -> int { return 1; },
        [](const auto&)  -> int { return -1; });
}
