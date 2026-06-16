// repro_class_template_alts_11: 假说 —— 触发点是 WTF::Variant(别名模板 using Variant=std::variant<Ts...>)
// 以"依赖包 Ts..."形态出现在被发射函数的修饰名里。alias-template-id Variant<Ts...> 含包展开,
// MS-ABI 修饰未实现 -> "cannot mangle this pack expansion yet"。
// 形态仿 CSSUnevaluatedCalc:template<class... Ts> RET f(const Variant<Ts...>&){ switchOn(v, [](auto){...}); }
#define WK_WINUWP 1
#include <wtf/StdLibExtras.h>

using WTF::switchOn;
using WTF::Variant;

template<class T> struct N { T v; };
struct R0{};struct R1{};struct R2{};struct R3{};struct R4{};struct R5{};struct R6{};struct R7{};

// 叶子重载
template<class T> bool f(const N<T>&) { return true; }

// 关键:依赖包 Variant<Ts...> 形态(别名模板 + Ts... 包)+ switchOn + 泛型 lambda 自递归。
// 该函数模板被实例化并(因 ODR-use / 取地址)发射时,其修饰名含 Variant<Ts...> 包展开。
template<class... Ts> [[gnu::noinline]] bool f(const Variant<Ts...>& v)
{
    return switchOn(v, [&](const auto& alt) -> bool { return f(alt); });
}

using Big = Variant<
    N<R0>, N<R1>, N<R2>, N<R3>, N<R4>, N<R5>, N<R6>, N<R7>,
    N<int>, N<long>, N<short>, N<char>>;

// 取该实例地址,强制修饰 f<N<R0>,...,N<char>>(const Variant<...>&)
bool (*volatile gpf)(const Big&) = &f<N<R0>,N<R1>,N<R2>,N<R3>,N<R4>,N<R5>,N<R6>,N<R7>,
                                      N<int>,N<long>,N<short>,N<char>>;

Big g;
bool run() { return gpf(g); }
