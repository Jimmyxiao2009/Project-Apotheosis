// repro_class_template_alts_10: 最大努力组合 —— 真实 WTF 头,38 类模板备选,
// switchOn 返回变体本身(Node),通过函数指针表阻止内联并逼发射,深互递归。
#define WK_WINUWP 1
#include <wtf/StdLibExtras.h>

namespace calc {
using WTF::switchOn; using WTF::Variant;

template<class T> struct IndirectNode { T* op; const T& operator*() const { return *op; } };
struct R00{};struct R01{};struct R02{};struct R03{};struct R04{};struct R05{};struct R06{};struct R07{};
struct R08{};struct R09{};struct R10{};struct R11{};struct R12{};struct R13{};struct R14{};struct R15{};
struct R16{};struct R17{};struct R18{};struct R19{};struct R20{};struct R21{};struct R22{};struct R23{};
struct R24{};struct R25{};struct R26{};struct R27{};struct R28{};struct R29{};struct R30{};struct R31{};
struct R32{};struct R33{};struct R34{};struct R35{};struct R36{};struct R37{};

using Node = Variant<
    IndirectNode<R00>,IndirectNode<R01>,IndirectNode<R02>,IndirectNode<R03>,IndirectNode<R04>,
    IndirectNode<R05>,IndirectNode<R06>,IndirectNode<R07>,IndirectNode<R08>,IndirectNode<R09>,
    IndirectNode<R10>,IndirectNode<R11>,IndirectNode<R12>,IndirectNode<R13>,IndirectNode<R14>,
    IndirectNode<R15>,IndirectNode<R16>,IndirectNode<R17>,IndirectNode<R18>,IndirectNode<R19>,
    IndirectNode<R20>,IndirectNode<R21>,IndirectNode<R22>,IndirectNode<R23>,IndirectNode<R24>,
    IndirectNode<R25>,IndirectNode<R26>,IndirectNode<R27>,IndirectNode<R28>,IndirectNode<R29>,
    IndirectNode<R30>,IndirectNode<R31>,IndirectNode<R32>,IndirectNode<R33>,IndirectNode<R34>,
    IndirectNode<R35>,IndirectNode<R36>,IndirectNode<R37>>;

// 返回 Node 的 switchOn(auto 推导成 Node) + 多 lambda 互递归
[[gnu::noinline]] Node clone(const Node& n);

template<class T> Node make(const IndirectNode<T>&) { return Node { IndirectNode<T>{ new T } }; }

[[gnu::noinline]] Node clone(const Node& n)
{
    return switchOn(n, [&](const auto& alt) -> Node { return make(alt); });
}

} // namespace calc

// 函数指针,阻止内联折叠 + 逼发射 switchOn 实例
calc::Node (*volatile gpfn)(const calc::Node&) = &calc::clone;
calc::Node run(const calc::Node& n) { return gpfn(n); }
