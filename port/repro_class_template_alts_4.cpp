// repro_class_template_alts_4: 决定性测试 —— 把变参包装层 wtf_visit 强制 noinline 并取地址,
// 逼 clang 发射 wtf_visit<Visitor<lambda>, const BigVariant&> 实例并修饰其名字。
// 若残留墙是"变参包装层被发射时其修饰名含包展开",这里就会撞墙。
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

// 变参包装层(强制 noinline + 外部可见 -> 必被发射并修饰)
template<typename Vis, typename... Variants> [[gnu::noinline]] auto wtf_visit(Vis&& v, Variants&&... values)
{ return std::visit(std::forward<Vis>(v), std::forward<Variants>(values)...); }

template<class V, class... F> [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return wtf_visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

// 38 类模板备选
template<class T> struct N { T v; };
struct R0 {}; struct R1 {}; struct R2 {}; struct R3 {}; struct R4 {};
struct R5 {}; struct R6 {}; struct R7 {}; struct R8 {}; struct R9 {};
using Big = std::variant<
    N<R0>, N<R1>, N<R2>, N<R3>, N<R4>, N<R5>, N<R6>, N<R7>, N<R8>, N<R9>,
    N<int>, N<long>, N<short>, N<char>, N<double>, N<float>, N<unsigned>, N<signed>,
    N<R0*>, N<R1*>, N<R2*>, N<R3*>, N<R4*>, N<R5*>, N<R6*>, N<R7*>, N<R8*>, N<R9*>,
    N<int*>, N<long*>, N<short*>, N<char*>, N<double*>, N<float*>, N<unsigned*>, N<signed*>,
    N<void*>, N<bool>>;

Big g;
int run()
{
    return switchOn(g, [](const auto&) -> int { return 0; });
}
