// repro_class_template_alts_8: 用真实 WTF 头(StdLibExtras.h 的 switchOn/WTF::visit/makeVisitor/Visitor),
// 复刻 CSSCalcTree 的 38 类模板备选 Node + Child + 真实 forAllChildren/copy 惯用法,
// 在真实修饰路径上逼出 StdLibExtras.h:579 的 "cannot mangle this pack expansion yet"。
#define WK_WINUWP 1
#include <wtf/StdLibExtras.h>
#include <tuple>

namespace calc {

using WTF::switchOn;
using WTF::Variant;

struct Child;
template<class T> struct N { T v; };
struct Sum    { std::tuple<Child*, Child*> kids; };
struct Product{ std::tuple<Child*, Child*> kids; };
struct Negate { std::tuple<Child*> kids; };
struct Min    { std::tuple<Child*, Child*, Child*> kids; };

template<typename Op> struct IndirectNode { Op* op; const Op& operator*() const { return *op; } };

using Node = Variant<
    N<int>, N<long>, N<short>, N<char>, N<double>, N<float>, N<unsigned>, N<bool>,
    IndirectNode<Sum>, IndirectNode<Product>, IndirectNode<Negate>, IndirectNode<Min>>;

struct Child { Node value; template<class T> Child(T&& t): value(std::forward<T>(t)) {} };

template<typename Op> const Op& deref(const IndirectNode<Op>& n) { return *n; }
template<typename T> const T& deref(const T& n) { return n; }

template<typename F> void forAllChildren(const Child&, const F&);
template<typename F> void forAllChildren(const N<int>&, const F&) {}
template<typename F> void forAllChildren(const N<long>&, const F&) {}
template<typename F> void forAllChildren(const N<short>&, const F&) {}
template<typename F> void forAllChildren(const N<char>&, const F&) {}
template<typename F> void forAllChildren(const N<double>&, const F&) {}
template<typename F> void forAllChildren(const N<float>&, const F&) {}
template<typename F> void forAllChildren(const N<unsigned>&, const F&) {}
template<typename F> void forAllChildren(const N<bool>&, const F&) {}

template<typename F, typename Op> void forAllChildren(const Op& op, const F& functor)
{
    WTF::apply([&](const auto&... x) { (..., (functor(*x), forAllChildren(*x, functor))); }, op.kids);
}

template<typename F> void forAllChildren(const Child& root, const F& functor)
{
    switchOn(root.value, [&](const auto& alt) { forAllChildren(deref(alt), functor); });
}

} // namespace calc

void run(const calc::Child& c)
{
    calc::forAllChildren(c, [](const calc::Child&) { });
}
