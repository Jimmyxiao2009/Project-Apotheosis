# gen-xaml-codebehind.ps1 — 手搓 XAML code-behind 生成器(绕开崩掉的 XamlCompiler)
# 思路:把 MainPage.xaml 的内容树(RootGrid)序列化成字符串,运行期 XamlReader::Load 加载;
#       事件属性全部剥掉(C++/CX 原始 XAML 解析不了事件名),改为生成代码里手动挂;
#       无名的事件元素自动补 x:Name,然后 FindName 取回挂事件。
# 产物:harness\xamlgen\MainPage.g.hpp  (实现 InitializeComponent/Connect 等)
# 用法:pwsh -File E:\Apotheosis\port\gen-xaml-codebehind.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$xamlPath = Join-Path $root 'harness\MainPage.xaml'
$ghPath   = Join-Path $root 'harness\xamlgen\MainPage.g.h'
$outDir   = Join-Path $root 'harness\xamlgen'
[System.IO.Directory]::CreateDirectory($outDir) | Out-Null

$nsP = 'http://schemas.microsoft.com/winfx/2006/xaml/presentation'
$nsX = 'http://schemas.microsoft.com/winfx/2006/xaml'

# 事件名 -> @(castType, delegateType-fully-qualified)
$EVMAP = @{
  'Click'                 = @('::Windows::UI::Xaml::Controls::Button',       '::Windows::UI::Xaml::RoutedEventHandler')
  'Tapped'                = @('::Windows::UI::Xaml::UIElement',              '::Windows::UI::Xaml::Input::TappedEventHandler')
  'ManipulationDelta'     = @('::Windows::UI::Xaml::UIElement',              '::Windows::UI::Xaml::Input::ManipulationDeltaEventHandler')
  'ManipulationCompleted' = @('::Windows::UI::Xaml::UIElement',              '::Windows::UI::Xaml::Input::ManipulationCompletedEventHandler')
  'TextChanged'           = @('::Windows::UI::Xaml::Controls::TextBox',      '::Windows::UI::Xaml::Controls::TextChangedEventHandler')
  'KeyDown'               = @('::Windows::UI::Xaml::UIElement',              '::Windows::UI::Xaml::Input::KeyEventHandler')
  'GotFocus'              = @('::Windows::UI::Xaml::UIElement',              '::Windows::UI::Xaml::RoutedEventHandler')
  'LostFocus'             = @('::Windows::UI::Xaml::UIElement',              '::Windows::UI::Xaml::RoutedEventHandler')
  'Loaded'                = @('::Windows::UI::Xaml::FrameworkElement',       '::Windows::UI::Xaml::RoutedEventHandler')
  'ValueChanged'          = @('::Windows::UI::Xaml::Controls::Slider',       '::Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventHandler')
}
$EVENTS = $EVMAP.Keys

$doc = New-Object System.Xml.XmlDocument
$doc.PreserveWhitespace = $true
$doc.Load($xamlPath)

$wires = New-Object System.Collections.ArrayList
$evCounter = 0

function Get-XName($el) {
  $n = $el.GetAttribute('Name', $nsX)
  if ([string]::IsNullOrEmpty($n)) { $n = $el.GetAttribute('Name') }  # x:Name 或 Name
  return $n
}

function Walk($node) {
  foreach ($child in @($node.ChildNodes)) {
    if ($child.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
    # 该元素上有哪些事件?
    $found = @()
    foreach ($ev in $EVENTS) { if ($child.HasAttribute($ev)) { $found += $ev } }
    if ($found.Count -gt 0) {
      $name = Get-XName $child
      if ([string]::IsNullOrEmpty($name)) {
        $script:evCounter++
        $name = "_ev$($script:evCounter)"
        $child.SetAttribute('Name', $nsX, $name)
      }
      foreach ($ev in $found) {
        $handler = $child.GetAttribute($ev)
        [void]$wires.Add([pscustomobject]@{ Name=$name; Event=$ev; Handler=$handler })
        [void]$child.RemoveAttribute($ev)
      }
    }
    Walk $child
  }
}

$root = $doc.DocumentElement          # <Page>
Walk $root

# 取 Page 下的内容元素(RootGrid)与 Page.Resources
$rootGrid = $null
$pageResourcesInner = $null
foreach ($c in $root.ChildNodes) {
  if ($c.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
  if ($c.LocalName -eq 'Page.Resources') { $pageResourcesInner = $c }
  elseif ($c.LocalName -eq 'Grid') { $rootGrid = $c }
}
if (-not $rootGrid) { throw '找不到 RootGrid' }

# 把 Page.Resources 的内容塞进 RootGrid 作为 Grid.Resources(置于最前)
if ($pageResourcesInner) {
  $gridRes = $doc.CreateElement('Grid.Resources', $nsP)
  foreach ($rc in @($pageResourcesInner.ChildNodes)) { [void]$gridRes.AppendChild($rc.CloneNode($true)) }
  [void]$rootGrid.InsertBefore($gridRes, $rootGrid.FirstChild)
}
# 修 XamlReader::Load 坑:根元素自身属性(如 Background)引用自己 Grid.Resources 里的
# {StaticResource X} —— 解析顺序上属性先于 Grid.Resources 被处理,X 还没建 → 空引用崩。
# 解法:把根元素属性里的 {StaticResource X} 直接替成 X 对应的字面色值。
$brushColors = @{}
if ($pageResourcesInner) {
  foreach ($n in $pageResourcesInner.ChildNodes) {
    if ($n.NodeType -eq [System.Xml.XmlNodeType]::Element -and $n.LocalName -eq 'SolidColorBrush') {
      $k = $n.GetAttribute('Key', $nsX); $c = $n.GetAttribute('Color')
      if ($k -and $c) { $brushColors[$k] = $c }
    }
  }
}
foreach ($attr in @($rootGrid.Attributes)) {
  if ($attr.Value -match '^\{StaticResource\s+(\w+)\}$') {
    $key = $Matches[1]
    if ($brushColors.ContainsKey($key)) { $attr.Value = $brushColors[$key]; "root attr $($attr.Name): {StaticResource $key} -> $($brushColors[$key])" }
  }
}
# 确保独立解析时命名空间齐全
$rootGrid.SetAttribute('xmlns', $nsP)
$rootGrid.SetAttribute('xmlns:x', $nsX)
# 二分用:HC_MAXCHILD=N 只保留 RootGrid 的前 N 个"可视"子(属性元素 Grid.* 保留)
if ($env:HC_MAXCHILD) {
  $max=[int]$env:HC_MAXCHILD; $vi=0
  foreach ($c in @($rootGrid.ChildNodes)) {
    if ($c.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
    if ($c.LocalName -like '*.*') { continue }
    $vi++; if ($vi -gt $max) { [void]$rootGrid.RemoveChild($c) }
  }
  "HC_MAXCHILD=$max  kept $([Math]::Min($vi,$max)) visual children"
}
# 二分第二层:HC_MAXCHILD2=N 把第一个可视子(内容区 Grid)的可视子也砍到前 N
if ($env:HC_MAXCHILD2) {
  $max2=[int]$env:HC_MAXCHILD2
  $fv=@($rootGrid.ChildNodes | Where-Object { $_.NodeType -eq [System.Xml.XmlNodeType]::Element -and $_.LocalName -notlike '*.*' })[0]
  if ($fv) {
    $vj=0
    foreach ($c in @($fv.ChildNodes)) {
      if ($c.NodeType -ne [System.Xml.XmlNodeType]::Element) { continue }
      if ($c.LocalName -like '*.*') { continue }
      $vj++; if ($vj -gt $max2) { [void]$fv.RemoveChild($c) }
    }
    "HC_MAXCHILD2=$max2  内容区保留前 $([Math]::Min($vj,$max2)) 子"
  }
}
$xamlStr = $rootGrid.OuterXml

# 读 MainPage.g.h 的字段表: "private: TYPE^ NAME;"
$fields = @()
foreach ($line in Get-Content $ghPath) {
  if ($line -match 'private:\s*(::[\w:]+)\^\s+(\w+);') { $fields += [pscustomobject]@{ Type=$Matches[1]; Name=$Matches[2] } }
}

# 生成分块宽字符串(MSVC 单字面量上限 ~16K 字符 → 分块 + 运行期拼接;LR 宽字面量 → Platform::String）
function Emit-XamlFunc($s) {
  $delim='APO'; $chunkSize=6000
  $b=New-Object System.Text.StringBuilder
  [void]$b.AppendLine('static ::Platform::String^ __MainPageXaml() {')
  [void]$b.AppendLine('    static const wchar_t* __c[] = {')
  for ($i=0; $i -lt $s.Length; $i+=$chunkSize) {
    $len=[Math]::Min($chunkSize, $s.Length-$i)
    $chunk=$s.Substring($i,$len)
    [void]$b.AppendLine("        LR`"$delim($chunk)$delim`",")
  }
  [void]$b.AppendLine('    };')
  [void]$b.AppendLine('    std::wstring __s;')
  [void]$b.AppendLine('    for (auto __p : __c) __s += __p;')
  [void]$b.AppendLine('    return ref new ::Platform::String(__s.c_str());')
  [void]$b.AppendLine('}')
  return $b.ToString()
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('#pragma once')
[void]$sb.AppendLine('// 手搓生成 —— 勿手改;改 XAML 后重跑 port\gen-xaml-codebehind.ps1')
[void]$sb.AppendLine('#include "MainPage.g.h"')
[void]$sb.AppendLine('#include <string>')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('namespace Harness {')
[void]$sb.AppendLine('')
[void]$sb.Append((Emit-XamlFunc $xamlStr))
[void]$sb.AppendLine('')
[void]$sb.AppendLine('void MainPage::InitializeComponent() {')
[void]$sb.AppendLine('    if (_contentLoaded) return;')
[void]$sb.AppendLine('    _contentLoaded = true;')
[void]$sb.AppendLine('    this->RequestedTheme = ::Windows::UI::Xaml::ElementTheme::Dark;')
[void]$sb.AppendLine('    // 运行期加载内嵌 XAML(绕开崩溃的 XamlCompiler);根上 {StaticResource} 已替成字面值。')
[void]$sb.AppendLine('    auto __root = safe_cast<::Windows::UI::Xaml::FrameworkElement^>(::Windows::UI::Xaml::Markup::XamlReader::Load(__MainPageXaml()));')
[void]$sb.AppendLine('    this->Content = __root;')
if ($env:HC_BISECT) { [void]$sb.AppendLine('    return; // HC_BISECT: 跳过 binds/wires') }
[void]$sb.AppendLine('    // ---- 绑定 x:Name 字段 ----')
foreach ($f in $fields) {
  if ($f.Name -eq 'RootGrid') {
    [void]$sb.AppendLine("    RootGrid = safe_cast<$($f.Type)^>(__root);")
  } else {
    [void]$sb.AppendLine("    $($f.Name) = safe_cast<$($f.Type)^>(__root->FindName(L`"$($f.Name)`"));")
  }
}
[void]$sb.AppendLine('    // ---- 挂事件 ----')
foreach ($w in $wires) {
  $cast = $EVMAP[$w.Event][0]; $del = $EVMAP[$w.Event][1]
  [void]$sb.AppendLine("    safe_cast<$cast^>(__root->FindName(L`"$($w.Name)`"))->$($w.Event) += ref new $del(this, &MainPage::$($w.Handler));")
}
[void]$sb.AppendLine('}')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('void MainPage::Connect(int, ::Platform::Object^) { }')
[void]$sb.AppendLine('::Windows::UI::Xaml::Markup::IComponentConnector^ MainPage::GetBindingConnector(int, ::Platform::Object^) { return nullptr; }')
[void]$sb.AppendLine('void MainPage::UnloadObject(::Windows::UI::Xaml::DependencyObject^) { }')
[void]$sb.AppendLine('void MainPage::DisconnectUnloadedObject(int) { }')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('}')

[System.IO.File]::WriteAllText("$outDir\MainPage.g.hpp", $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
"OK -> $outDir\MainPage.g.hpp"
"fields=$($fields.Count)  wires=$($wires.Count)  evNamed=$evCounter  xamlBytes=$($xamlStr.Length)"
