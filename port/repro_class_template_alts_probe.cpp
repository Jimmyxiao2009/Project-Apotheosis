// probe:直接逼 MS 修饰器对"模板实参为依赖包展开类型"撞墙。
// 形态:把一个 Visitor<F...>(其中 F... 仍含包展开 / 或闭包带变参 operator())作为
// 变参函数模板的实参,强制发射。
#include <variant>
#include <utility>
#include <tuple>

// A) 变参闭包(operator() 自身模板 + 内部包展开),取地址逼发射
template<class... Ts>
struct Wrap {
    std::tuple<Ts...> t;
    template<class... Us> auto operator()(Us&&... us) const { return (0 + ... + sizeof(Us)); }
};

// 变参函数模板,实参类型含包(Wrap<Ts...>),body 触发 operator()<...> 实例化
template<class... Ts> [[gnu::noinline]] auto emitMe(const Wrap<Ts...>& w, Ts... xs)
{
    return w(xs...);   // operator()<Ts...> 包展开
}

// 取地址 -> 必须修饰 emitMe<int,double,char> 的名字
using FnT = double (*)(const Wrap<int,double,char>&, int, double, char);

double sink;
auto* gp = (void*)+[]{ return 0; };

template auto emitMe<int,double,char>(const Wrap<int,double,char>&, int, double, char);

int run()
{
    Wrap<int,double,char> w{};
    return (int)emitMe(w, 1, 2.0, 'c');
}
