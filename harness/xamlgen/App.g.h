#pragma once
//------------------------------------------------------------------------------
//  手搓快照(原 Pass1 产物)+ 改动:去掉 IXamlMetadataProvider。
//  原因:本方案运行期用 XamlReader::Load 加载纯框架类型的 XAML。若 App 仍实现
//  IXamlMetadataProvider 但 GetXamlType 返回 nullptr,框架解析框架类型时拿到 null
//  会在 Windows.UI.Xaml 里空指针 AV(c0000005)。去掉它 → 框架走内置类型解析。
//------------------------------------------------------------------------------

namespace Harness
{
    partial ref class App : public ::Windows::UI::Xaml::Application
    {
    public:
        void InitializeComponent();
    private:
        bool _contentLoaded;
    };
}
