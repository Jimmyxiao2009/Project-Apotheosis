// repro2: 复杂 variant(多备选 + 指针/结构 + 嵌套)+ std::visit,测是否"复杂度"触发 pack-expansion 墙。
#include <variant>
#include <utility>
#include <memory>

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
template<class V, class... F> [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

struct A { }; struct B { }; struct C { }; struct D { }; struct E { };
struct F1 { }; struct G { }; struct H { };
// 仿 WebKit 风格:RefPtr 状的指针 + 多备选 + 一个嵌套 variant 备选
using Inner = std::variant<int, double>;
using Big = std::variant<std::shared_ptr<A>, std::shared_ptr<B>, std::shared_ptr<C>,
                         std::shared_ptr<D>, std::shared_ptr<E>, std::shared_ptr<F1>,
                         std::shared_ptr<G>, std::shared_ptr<H>, Inner, int, double, char>;
Big g;
int run()
{
    return switchOn(g,
        [](const std::shared_ptr<A>&) { return 1; }, [](const std::shared_ptr<B>&) { return 2; },
        [](const std::shared_ptr<C>&) { return 3; }, [](const std::shared_ptr<D>&) { return 4; },
        [](const std::shared_ptr<E>&) { return 5; }, [](const std::shared_ptr<F1>&) { return 6; },
        [](const std::shared_ptr<G>&) { return 7; }, [](const std::shared_ptr<H>&) { return 8; },
        [](const Inner&) { return 9; }, [](int) { return 10; }, [](double) { return 11; }, [](char) { return 12; });
}
