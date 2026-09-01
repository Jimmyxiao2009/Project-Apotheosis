#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"
#if !defined(APOTHEOSIS_XAML_CODEGEN)
#include "App.g.hpp"
#endif

using namespace Harness;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

App::App()
{
    InitializeComponent();
    Suspending += ref new Windows::UI::Xaml::SuspendingEventHandler(this, &App::OnSuspending);
}

// cookie JSON 落盘的真正触发点(见 App.xaml.h 注释)。拿 deferral,转给引擎线程串行写完再 Complete——
// deferral 是 agile 对象,Complete() 不需要转回 UI 线程调。MainPage::FlushCookiesForSuspend 里实现
// (WebEngine 队列是 MainPage.xaml.cpp 内部实现细节,没有跨 TU 头,故走页面方法转发)。
void App::OnSuspending(Platform::Object^, Windows::ApplicationModel::SuspendingEventArgs^ e)
{
    auto deferral = e->SuspendingOperation->GetDeferral();
    auto page = dynamic_cast<MainPage^>(Window::Current->Content);
    if (!page) {
        if (auto frame = dynamic_cast<Frame^>(Window::Current->Content))
            page = dynamic_cast<MainPage^>(frame->Content);
    }
    if (page)
        page->FlushCookiesForSuspend(deferral);
    else
        deferral->Complete();
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
#if defined(APOTHEOSIS_OFFICIAL_XAML)
    auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);
    if (rootFrame == nullptr) {
        rootFrame = ref new Frame();
        Window::Current->Content = rootFrame;
    }
    if (rootFrame->Content == nullptr)
        rootFrame->Navigate(Windows::UI::Xaml::Interop::TypeName(MainPage::typeid), e->Arguments);
#else
    (void)e;
    if (Window::Current->Content == nullptr)
        Window::Current->Content = ref new MainPage();
#endif
    Window::Current->Activate();
}
