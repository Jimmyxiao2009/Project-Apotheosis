#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"
#include "App.g.hpp"

using namespace JITTest;
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
