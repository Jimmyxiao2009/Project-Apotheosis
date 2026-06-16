// repro_odr_address_real: 直接 #include 真实 WTF/StdLibExtras.h(及其 Variant.h),
// 用 WebCore 真实惯用法实例化 switchOn,确认墙仍在并定位被修饰实体。
#include <wtf/StdLibExtras.h>
#include <wtf/Variant.h>

struct Leaf { };
bool isCalc(const Leaf&) { return false; }
bool isCalc(int) { return false; }

template<typename... Ts> bool isCalc(const WTF::Variant<Ts...>& component)
{
    return WTF::switchOn(component, [&](auto alternative) -> bool { return isCalc(alternative); });
}

using Inner = WTF::Variant<int, double>;
using Outer = WTF::Variant<Leaf, Inner>;
Outer g;
bool run() { return isCalc(g); }
