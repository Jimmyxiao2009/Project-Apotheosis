// probe: 直接逼 MS mangler 修饰一个残留 PackExpansionType 的最小构造。
// 已知 MicrosoftMangle 在 mangleType(PackExpansionType) 抛 "cannot mangle this pack expansion yet"。
// 当变参函数模板的返回类型是 decltype(一个把 F... 当未展开包用的表达式) 时, pack 可存活到修饰。
#include <utility>

// A: 变参函数模板, 尾置 decltype 里 F... 进入一个依赖别名 -> 可能残留 pack expansion
template<class... F> struct Pack {};
template<class... F> auto probeA(F&&... f) -> Pack<F...> { return {}; }

// B: sizeof...(F) 之外, 用 F... 构造一个 tuple-of-pack 的 decltype
template<class... F> auto probeB(F&&... f) -> decltype(Pack<decltype(f)...>{}) { return {}; }

// C: 嵌套变参 lambda 返回包展开
template<class... F> auto probeC(F&&... f) {
    auto l = [](auto... xs) -> Pack<decltype(xs)...> { return {}; };
    return l(f...);
}

struct X{}; struct Y{};
auto run() {
    probeA(X{}, Y{});
    probeB(X{}, Y{});
    probeC(X{}, Y{});
    return 0;
}
