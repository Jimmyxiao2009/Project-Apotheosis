#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"
#include "App.g.hpp"  // XAML 生成的实现(InitializeComponent + main/Application::Start)

using namespace Harness;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

App::App()
{
    InitializeComponent();
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    (void)e;
    // 手搓:不走 Frame::Navigate(按 TypeName 实例化页面依赖 LoadComponent(App.xaml) 初始化的
    // XAML 类型/元数据系统;本方案绕开了 markup compile,没那步 → Navigate 空指针崩)。
    // 直接 ref new MainPage() 走 C++ 构造,设为窗口内容(本 app 单页,不需要 Frame 导航)。
    if (Window::Current->Content == nullptr) {
        Window::Current->Content = ref new MainPage();
    }
    Window::Current->Activate();
}
