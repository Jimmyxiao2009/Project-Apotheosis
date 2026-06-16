// SmokeTest.cpp  —  调 JavaScriptCore C API 执行 JS, 验证移植到 ARM32 UWP 的 JSC 能跑。
#include "pch.h"
#include "SmokeTest.h"

#include <JavaScriptCore/JavaScript.h>
#include <string>

static std::wstring toWide(JSStringRef s)
{
    size_t len = JSStringGetLength(s);
    std::wstring out(len, L'\0');
    // JSChar 是 UTF-16, 与 Windows wchar_t 一致
    const JSChar* chars = JSStringGetCharactersPtr(s);
    for (size_t i = 0; i < len; ++i)
        out[i] = static_cast<wchar_t>(chars[i]);
    return out;
}

std::wstring RunJscSmokeTest()
{
    JSGlobalContextRef ctx = JSGlobalContextCreate(nullptr);
    if (!ctx)
        return L"JSGlobalContextCreate 失败";

    JSStringRef script = JSStringCreateWithUTF8CString("1 + 1");
    JSValueRef exception = nullptr;
    JSValueRef result = JSEvaluateScript(ctx, script, nullptr, nullptr, 0, &exception);
    JSStringRelease(script);

    std::wstring text;
    if (exception) {
        JSStringRef es = JSValueToStringCopy(ctx, exception, nullptr);
        text = L"JS 异常: " + toWide(es);
        JSStringRelease(es);
    } else {
        double n = JSValueToNumber(ctx, result, nullptr);
        // 同时跑一段稍复杂的, 证明解释器真的在工作
        JSStringRef s2 = JSStringCreateWithUTF8CString("(function(){var s=0;for(var i=1;i<=100;i++)s+=i;return s;})()");
        JSValueRef r2 = JSEvaluateScript(ctx, s2, nullptr, nullptr, 0, nullptr);
        JSStringRelease(s2);
        double sum = JSValueToNumber(ctx, r2, nullptr);

        wchar_t buf[128];
        swprintf_s(buf, L"1 + 1 = %g\nΣ1..100 = %g\n\nJSC on ARM32 UWP ✓", n, sum);
        text = buf;
    }

    JSGlobalContextRelease(ctx);
    return text;
}
