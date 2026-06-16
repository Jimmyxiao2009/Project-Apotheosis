// repro_class_template_alts_5: 真实触发点 —— 多 lambda 的 switchOn(F... 为 2+ 个不同闭包类型),
// 经变参包装层 WTF::visit 发射 -> switchOn<V, F0, F1, ...> 的修饰名里 F... 包展开 -> 撞墙。
// 仿 CSSCalcTree+Traversal.h:106 的 WTF::switchOn(root, [](const Child&){...}, [](const None&){...})。
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

// 真实 WK_WINUWP WTF::visit:变参包装层
template<typename Vis, typename... Variants> constexpr auto wtf_visit(Vis&& v, Variants&&... values)
{ return std::visit(std::forward<Vis>(v), std::forward<Variants>(values)...); }

template<class V, class... F> [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return wtf_visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

// 38 类模板备选大变体
template<class T> struct N { T v; };
struct R0 {}; struct R1 {}; struct R2 {}; struct R3 {}; struct R4 {};
struct R5 {}; struct R6 {}; struct R7 {}; struct R8 {}; struct R9 {};
using Big = std::variant<
    N<R0>, N<R1>, N<R2>, N<R3>, N<R4>, N<R5>, N<R6>, N<R7>, N<R8>, N<R9>,
    N<int>, N<long>, N<short>, N<char>, N<double>, N<float>, N<unsigned>, N<signed>,
    N<R0*>, N<R1*>, N<R2*>, N<R3*>, N<R4*>, N<R5*>, N<R6*>, N<R7*>, N<R8*>, N<R9*>,
    N<int*>, N<long*>, N<short*>, N<char*>, N<double*>, N<float*>, N<unsigned*>, N<signed*>,
    N<void*>, N<bool>>;

// 模板函数内,多 lambda(F... = 多个不同闭包),递归互调 -> 阻止全内联
template<typename G> int dispatch(const Big& v, const G& g)
{
    return switchOn(v,
        [&](const N<R0>&) -> int { return g(0); },
        [&](const N<R1>&) -> int { return g(1); },
        [&](const N<R2>&) -> int { return g(2); },
        [&](const N<R3>&) -> int { return g(3); },
        [&](const N<R4>&) -> int { return g(4); },
        [&](const N<R5>&) -> int { return g(5); },
        [&](const N<R6>&) -> int { return g(6); },
        [&](const N<R7>&) -> int { return g(7); },
        [&](const N<R8>&) -> int { return g(8); },
        [&](const N<R9>&) -> int { return g(9); },
        [&](const auto&)  -> int { return g(-1); });
}

Big gv;
int run() { return dispatch(gv, [](int x) { return x + 1; }); }
