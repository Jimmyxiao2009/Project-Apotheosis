// repro_class_template_alts_2: 角度B 加强 —— 类模板备选里嵌套 variant/optional,
// 让 lambda 的 f(alt) 再次落到"依赖变体的模板 f" → 跨多个变体实例互递归,
// switchOn 在每层都被发射 → 逼修饰变参模板内 lambda。
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

struct RawA { }; struct RawB { }; struct RawC { }; struct RawD { };

template<class T> struct UnevaluatedCalc { int v; };
template<class T> struct Number          { double v; };
template<class T> struct AngleRaw        { float v; };
template<class T> struct LengthRaw       { long v; };
template<class T> struct PercentRaw      { short v; };
template<class T> struct ResolutionRaw   { char v; };
template<class T> struct TimeRaw         { unsigned v; };
template<class T> struct FrequencyRaw    { signed v; };

// 叶子重载
template<class T> double f(const UnevaluatedCalc<T>& a) { return a.v + 1.0; }
template<class T> double f(const Number<T>& a)          { return a.v + 2.0; }
template<class T> double f(const AngleRaw<T>& a)        { return a.v + 3.0; }
template<class T> double f(const LengthRaw<T>& a)       { return (double)a.v + 4.0; }
template<class T> double f(const PercentRaw<T>& a)      { return (double)a.v + 5.0; }
template<class T> double f(const ResolutionRaw<T>& a)   { return (double)a.v + 6.0; }
template<class T> double f(const TimeRaw<T>& a)         { return (double)a.v + 7.0; }
template<class T> double f(const FrequencyRaw<T>& a)    { return (double)a.v + 8.0; }

// optional 形态(仿真实 calc:备选可为 optional<NumericType>)
template<class T> double f(const std::optional<T>& o) { return o ? f(*o) : 0.0; }

// 依赖变体 + 变参模板内泛型 lambda(回调自身,跨实例互递归)
template<typename... Ts> double f(const std::variant<Ts...>& v)
{
    return switchOn(v, [&](const auto& alt) -> double { return f(alt); });
}

// 多层嵌套变体:每层都用 8+ 类模板备选,且其中一个备选是更深一层变体
using L0 = std::variant<UnevaluatedCalc<RawA>, Number<RawA>, AngleRaw<RawA>, LengthRaw<RawA>,
                        PercentRaw<RawA>, ResolutionRaw<RawA>, TimeRaw<RawA>, FrequencyRaw<RawA>>;
using L1 = std::variant<UnevaluatedCalc<RawB>, Number<RawB>, AngleRaw<RawB>, LengthRaw<RawB>,
                        PercentRaw<RawB>, ResolutionRaw<RawB>, TimeRaw<RawB>, std::optional<L0>>;
using L2 = std::variant<UnevaluatedCalc<RawC>, Number<RawC>, AngleRaw<RawC>, LengthRaw<RawC>,
                        PercentRaw<RawC>, ResolutionRaw<RawC>, TimeRaw<RawC>, std::optional<L1>>;
using L3 = std::variant<UnevaluatedCalc<RawD>, Number<RawD>, AngleRaw<RawD>, LengthRaw<RawD>,
                        PercentRaw<RawD>, ResolutionRaw<RawD>, TimeRaw<RawD>, std::optional<L2>>;

L3 g;
double run() { return f(g); }
