#pragma once
// 手搓生成 —— 绕开崩掉的 XamlCompiler。App.xaml 为空(无 app 级资源)。
// App 不再实现 IXamlMetadataProvider(见 App.g.h),XamlReader::Load 走框架内置类型解析。
#include "App.g.h"

namespace Harness {

void App::InitializeComponent() {
    if (_contentLoaded) return;
    _contentLoaded = true;
    // App.xaml 无内容/资源,无需 LoadComponent。
}

} // namespace Harness

// UWP C++/CX 入口:原 App.g.hpp 自带 main + Application::Start。
[::Platform::MTAThread]
int main(::Platform::Array<::Platform::String^>^) {
    ::Windows::UI::Xaml::Application::Start(
        ref new ::Windows::UI::Xaml::ApplicationInitializationCallback(
            [](::Windows::UI::Xaml::ApplicationInitializationCallbackParams^) {
                auto app = ref new ::Harness::App();
                (void)app;
            }));
    return 0;
}
