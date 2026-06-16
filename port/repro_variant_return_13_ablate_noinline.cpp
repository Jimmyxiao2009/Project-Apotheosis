// repro_variant_return_13: 控制 —— 保留 requires, 但去掉 ALWAYS_INLINE(普通 inline), 看墙是否仍在。
#include <utility>
#include <type_traits>

template<typename T> concept HasSwitchOn = requires(T t) { t.doSwitch([](const auto&) {}); };
template<class V, class... F> requires (!HasSwitchOn<V>) inline auto switchOn(V&&, F&&... f) { return (sizeof...(f)); }
template<class V, class... F> requires (HasSwitchOn<V>) inline auto switchOn(V&& v, F&&... f)
{ return v.doSwitch(std::forward<F>(f)...); }
struct WithMember { template<class... F> auto doSwitch(F&&... f) const { return (sizeof...(f)); } };
auto run() { WithMember w; return switchOn(w, [](const auto&){ return 1; }, [](int){ return 2; }); }
