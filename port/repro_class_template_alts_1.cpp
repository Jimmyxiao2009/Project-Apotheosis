// repro_class_template_alts_1: 角度B —— 备选是类模板且数量多(8+),深互递归。
// 真实形态:UnevaluatedCalc<RawType>、Number<>、AngleRaw<> 等类模板备选,一个变体 6~12 个。
// 目标:用 8+ 类模板备选 + 多个 f 重载互相调用 → switchOn 函数体过大 →
//       clang 放弃内联 always_inline → 发射符号 → 修饰"变参模板内 lambda" → 撞墙。
#include <variant>
#include <optional>
#include <utility>

template<class A, class... B> struct Visitor : Visitor<A>, Visitor<B...> {
    Visitor(A a, B... b) : Visitor<A>(a), Visitor<B...>(b...) { }
    using Visitor<A>::operator();
    using Visitor<B...>::operator();
};
template<class A> struct Visitor<A> : A {
    Visitor(A a) : A(a) { }
    using A::operator();
};
template<class... F> [[gnu::always_inline]] inline Visitor<F...> makeVisitor(F... f) { return Visitor<F...>(f...); }
template<class V, class... F> [[gnu::always_inline]] inline auto switchOn(V&& v, F&&... f)
{ return std::visit(makeVisitor(std::forward<F>(f)...), std::forward<V>(v)); }

// === 真实失败惯用法:类模板备选(仿 UnevaluatedCalc<T> / Number<> / AngleRaw<>)===
struct RawA { }; struct RawB { }; struct RawC { }; struct RawD { };
struct RawE { }; struct RawF { }; struct RawG { }; struct RawH { };

template<class T> struct UnevaluatedCalc { int v; };
template<class T> struct Number          { double v; };
template<class T> struct AngleRaw        { float v; };
template<class T> struct LengthRaw       { long v; };
template<class T> struct PercentRaw      { short v; };
template<class T> struct ResolutionRaw   { char v; };
template<class T> struct TimeRaw         { unsigned v; };
template<class T> struct FrequencyRaw    { signed v; };

// 12 个类模板备选的"大"变体
using Big = std::variant<
    UnevaluatedCalc<RawA>, Number<RawB>, AngleRaw<RawC>, LengthRaw<RawD>,
    PercentRaw<RawE>, ResolutionRaw<RawF>, TimeRaw<RawG>, FrequencyRaw<RawH>,
    UnevaluatedCalc<RawB>, Number<RawC>, AngleRaw<RawD>, LengthRaw<RawE>>;

// 互递归 f 重载簇:每个 alt 类型有自己的重载,且都回调到依赖变体的模板 f。
template<class T> double f(const UnevaluatedCalc<T>&);
template<class T> double f(const Number<T>&);
template<class T> double f(const AngleRaw<T>&);
template<class T> double f(const LengthRaw<T>&);
template<class T> double f(const PercentRaw<T>&);
template<class T> double f(const ResolutionRaw<T>&);
template<class T> double f(const TimeRaw<T>&);
template<class T> double f(const FrequencyRaw<T>&);
template<typename... Ts> double f(const std::variant<Ts...>& v);

// 依赖变体 + 变参模板内泛型 lambda(回调 f → 互递归到全部类模板重载)
template<typename... Ts> double f(const std::variant<Ts...>& v)
{
    return switchOn(v, [&](const auto& alt) -> double { return f(alt); });
}

template<class T> double f(const UnevaluatedCalc<T>& a) { return a.v + 1.0; }
template<class T> double f(const Number<T>& a)          { return a.v + 2.0; }
template<class T> double f(const AngleRaw<T>& a)        { return a.v + 3.0; }
template<class T> double f(const LengthRaw<T>& a)       { return (double)a.v + 4.0; }
template<class T> double f(const PercentRaw<T>& a)      { return (double)a.v + 5.0; }
template<class T> double f(const ResolutionRaw<T>& a)   { return (double)a.v + 6.0; }
template<class T> double f(const TimeRaw<T>& a)         { return (double)a.v + 7.0; }
template<class T> double f(const FrequencyRaw<T>& a)    { return (double)a.v + 8.0; }

Big g;
double run() { return f(g); }
