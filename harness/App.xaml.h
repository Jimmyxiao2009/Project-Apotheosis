#pragma once
#include "App.g.h"

namespace Harness {
    ref class App sealed {
    public:
        App();
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;
    private:
        // UWP 真正的挂起信号(比 MainPage 的 Window::VisibilityChanged 更可靠):切后台到进程被冻结/
        // 可能被系统直接终止回收内存之间,系统只给几秒;VisibilityChanged 触发的 fire-and-forget
        // 异步落盘不保证能在冻结前跑完。用 deferral 明确"等我做完"。
        void OnSuspending(Platform::Object^ sender, Windows::ApplicationModel::SuspendingEventArgs^ e);
    };
}
