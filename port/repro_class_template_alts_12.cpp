// repro_class_template_alts_12: 仿 CSSCalcTree+Simplification.cpp:simplifyForTrig<Op>。
// switchOn 在函数模板 simplifyFor<Op> 内,对 root.a(类型依赖 Op)操作,多 lambda 返回 optional<Child>。
// 多个 Op(8+ 类模板)互递归 + 该函数模板被发射 -> switchOn<decltype(root.a), F...> 修饰 -> 撞墙。
#define WK_WINUWP 1
#include <wtf/StdLibExtras.h>
#include <optional>

using WTF::switchOn; using WTF::Variant;

struct Number { double value; };
struct CanonicalDimension { double value; int dim; };
struct Symbol { int id; };
struct Child;

// root.a 的类型:一个含 Child 的数值变体(依赖递归)
template<class T> struct IndirectNode { T* op; const T& operator*() const { return *op; } };

using NumericRoot = Variant<Number, CanonicalDimension, Symbol>;

// 8+ 个 Op 类模板,各有成员 a(类型 = NumericRoot 或 Child)
struct Sin   { NumericRoot a; }; struct Cos   { NumericRoot a; };
struct Tan   { NumericRoot a; }; struct Asin  { NumericRoot a; };
struct Acos  { NumericRoot a; }; struct Atan  { NumericRoot a; };
struct Log   { NumericRoot a; }; struct Exp   { NumericRoot a; };
struct Abs   { NumericRoot a; }; struct Sign  { NumericRoot a; };

using Node = Variant<
    Number, CanonicalDimension, Symbol,
    IndirectNode<Sin>, IndirectNode<Cos>, IndirectNode<Tan>, IndirectNode<Asin>,
    IndirectNode<Acos>, IndirectNode<Atan>, IndirectNode<Log>, IndirectNode<Exp>,
    IndirectNode<Abs>, IndirectNode<Sign>>;

struct Child { Node value; template<class T> Child(T&& t): value(std::forward<T>(t)) {} };

inline std::optional<Child> makeChild(Number n) { return Child { n }; }
inline std::optional<Child> makeChild(CanonicalDimension n) { return Child { n }; }

// 仿 simplifyForTrig<Op>:switchOn(root.a, 多 lambda),root.a 类型依赖 Op
template<typename Op> [[gnu::noinline]] std::optional<Child> simplifyForTrig(Op& root)
{
    return switchOn(root.a,
        [&](const Number& a) -> std::optional<Child> { return makeChild(Number { a.value + 1 }); },
        [&](const CanonicalDimension& a) -> std::optional<Child> { return makeChild(CanonicalDimension { a.value, a.dim }); },
        [](const auto&) -> std::optional<Child> { return { }; });
}

// 显式实例化 + 取地址,强制发射全部 simplifyForTrig<Op> -> switchOn<NumericRoot, F0,F1,F2>
#define INST(Op) template std::optional<Child> simplifyForTrig<Op>(Op&);
INST(Sin) INST(Cos) INST(Tan) INST(Asin) INST(Acos) INST(Atan) INST(Log) INST(Exp) INST(Abs) INST(Sign)
#undef INST

using FP = std::optional<Child>(*)(Sin&);
FP volatile gp = &simplifyForTrig<Sin>;
