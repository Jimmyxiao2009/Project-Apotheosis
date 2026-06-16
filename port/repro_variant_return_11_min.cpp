// repro_variant_return_11_min: 最小本质触发(无 variant/Visitor)——
// requires 约束的变参函数模板 switchOn, 被一个带变参成员 .switchOn(F...) 的类型选中 HasSwitchOn 重载,
// 该约束变参重载被发射成符号 -> MS mangler 撞 "cannot mangle this pack expansion yet"。
#include <utility>
#include <type_traits>

#define ALWAYS_INLINE [[gnu::always_inline]] inline

template<typename T> concept HasSwitchOn = requires(T t) { t.doSwitch([](const auto&) {}); };

// !HasSwitchOn 重载(占位, 形成约束重载集)
template<class V, class... F> requires (!HasSwitchOn<V>) ALWAYS_INLINE auto switchOn(V&&, F&&... f) { return (sizeof...(f)); }
// HasSwitchOn 重载: 受约束的变参模板, auto 返回(无 decltype 也撞墙)
template<class V, class... F> requires (HasSwitchOn<V>) ALWAYS_INLINE auto switchOn(V&& v, F&&... f)
{ return v.doSwitch(std::forward<F>(f)...); }

// 自带变参成员 doSwitch 的类型 -> 满足 HasSwitchOn -> 命中受约束重载
struct WithMember {
    template<class... F> auto doSwitch(F&&... f) const { return (sizeof...(f)); }
};

auto run() { WithMember w; return switchOn(w, [](const auto&){ return 1; }, [](int){ return 2; }); }
