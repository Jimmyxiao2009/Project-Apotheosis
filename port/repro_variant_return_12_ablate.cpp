// repro_variant_return_12_ablate: 控制实验 —— 去掉 requires 约束(其余同 11_min), 看墙是否消失。
#include <utility>
#include <type_traits>
#define ALWAYS_INLINE [[gnu::always_inline]] inline

// 无 requires: 普通变参函数模板转发到成员变参
template<class V, class... F> ALWAYS_INLINE auto switchOn(V&& v, F&&... f)
{ return v.doSwitch(std::forward<F>(f)...); }

struct WithMember { template<class... F> auto doSwitch(F&&... f) const { return (sizeof...(f)); } };
auto run() { WithMember w; return switchOn(w, [](const auto&){ return 1; }, [](int){ return 2; }); }
