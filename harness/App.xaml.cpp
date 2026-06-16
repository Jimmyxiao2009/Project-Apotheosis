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
    auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);
    if (rootFrame == nullptr) {
        rootFrame = ref new Frame();
        Window::Current->Content = rootFrame;
    }
    if (rootFrame->Content == nullptr)
        rootFrame->Navigate(Windows::UI::Xaml::Interop::TypeName(MainPage::typeid), e->Arguments);

    Window::Current->Activate();
}
