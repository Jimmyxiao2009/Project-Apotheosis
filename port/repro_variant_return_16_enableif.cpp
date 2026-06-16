// repro_variant_return_16: 用 enable_if(SFINAE)替代 requires 约束, 看墙是否消失。
#include <utility>
#include <type_traits>
#define ALWAYS_INLINE [[gnu::always_inline]] inline

template<class T, class = void> struct HasMember : std::false_type {};
template<class T> struct HasMember<T, std::void_t<decltype(std::declval<T>().doSwitch([](const auto&){}))>> : std::true_type {};

template<class V, class... F, std::enable_if_t<!HasMember<V>::value, int> = 0>
ALWAYS_INLINE auto switchOn(V&&, F&&... f) { return (sizeof...(f)); }
template<class V, class... F, std::enable_if_t<HasMember<V>::value, int> = 0>
ALWAYS_INLINE auto switchOn(V&& v, F&&... f) { return v.doSwitch(std::forward<F>(f)...); }

struct WithMember { template<class... F> auto doSwitch(F&&... f) const { return (sizeof...(f)); } };
auto run() { WithMember w; return switchOn(w, [](const auto&){ return 1; }, [](int){ return 2; }); }
