/* clang-cl-arm-shim.h  —  补齐 clang-cl 在 ARM32 上缺失的 MSVC 专有 intrinsic。
 * 经 toolchain 的 /FI 强制包含到每个 TU(在 STL 头之前)。
 * 仅对 clang-cl + ARM(32位)生效, 其余编译器/架构为空。
 *
 * 背景: MSVC STL 的 <bit>(__msvc_bit_utils.hpp)在 _M_ARM 分支用 _CountLeadingZeros/
 *       _CountLeadingZeros64(ARM CLZ intrinsic), 但没 !__clang__ 守卫, clang-cl 无此声明。
 */
#pragma once
#if defined(__clang__) && defined(_M_ARM)

#ifdef __cplusplus
extern "C" {
#endif

static __forceinline unsigned int _CountLeadingZeros(unsigned long _Val) {
    return _Val ? (unsigned int)__builtin_clz((unsigned int)_Val) : 32u;
}

static __forceinline unsigned int _CountLeadingZeros64(unsigned __int64 _Val) {
    return _Val ? (unsigned int)__builtin_clzll((unsigned long long)_Val) : 64u;
}

#ifdef __cplusplus
}
#endif

#endif /* __clang__ && _M_ARM */
