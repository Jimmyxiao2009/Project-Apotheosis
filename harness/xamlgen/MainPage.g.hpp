#pragma once
// 手搓生成 —— 勿手改;改 XAML 后重跑 port\gen-xaml-codebehind.ps1
#include "MainPage.g.h"
#include <string>

namespace Harness {

static ::Platform::String^ __MainPageXaml() {
    static const wchar_t* __c[] = {
        LR"APO(<Grid x:Name="RootGrid" Background="#FF080D11" xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"><Grid.Resources>
        <SolidColorBrush x:Key="PageBg" Color="#FF080D11" />
        <SolidColorBrush x:Key="Chrome" Color="#FF10171D" />
        <SolidColorBrush x:Key="Surface" Color="#FF172129" />
        <SolidColorBrush x:Key="SurfaceHi" Color="#FF1D2B34" />
        <SolidColorBrush x:Key="Inset" Color="#FF090F13" />
        <SolidColorBrush x:Key="Sep" Color="#FF263640" />
        <SolidColorBrush x:Key="TxtHi" Color="#FFF4F7F8" />
        <SolidColorBrush x:Key="TxtLo" Color="#FF91A2AD" />
        <SolidColorBrush x:Key="Accent" Color="#FF45D6C5" />
        <SolidColorBrush x:Key="AccentDim" Color="#FF183D3B" />
        <SolidColorBrush x:Key="Warm" Color="#FFFFB454" />
        <SolidColorBrush x:Key="Danger" Color="#FFFF6B6B" />
        <SolidColorBrush x:Key="Scrim" Color="#D9080D11" />

        <!-- 底栏图标键:大触摸目标,透明底 -->
        <Style x:Key="NavBtn" TargetType="Button">
            <Setter Property="Background" Value="Transparent" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="BorderThickness" Value="0" />
            <Setter Property="FontFamily" Value="Segoe MDL2 Assets" />
            <Setter Property="FontSize" Value="20" />
            <Setter Property="Width" Value="52" />
            <Setter Property="Height" Value="62" />
            <Setter Property="Padding" Value="0" />
            <Setter Property="VerticalAlignment" Value="Stretch" />
        </Style>
        <!-- 小图标键(查找条/抽屉关闭等) -->
        <Style x:Key="IconBtn" TargetType="Button">
            <Setter Property="Background" Value="Transparent" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="BorderThickness" Value="0" />
            <Setter Property="FontSize" Value="16" />
            <Setter Property="Width" Value="40" />
            <Setter Property="Height" Value="40" />
            <Setter Property="Padding" Value="0" />
            <Setter Property="VerticalAlignment" Value="Center" />
        </Style>
        <Style x:Key="TabBtn" TargetType="Button">
            <Setter Property="Background" Value="Transparent" />
            <Setter Property="Foreground" Value="{StaticResource TxtLo}" />
            <Setter Property="BorderThickness" Value="0" />
            <Setter Property="FontSize" Value="14" />
            <Setter Property="Height" Value="46" />
            <Setter Property="Padding" Value="10,0" />
        </Style>
        <!-- Apotheosis: 悬浮翻页键(▲/▼)。原先上键是 SurfaceHi 圆底 + 亮字形,下键固定用 Accent 圆底 +
             深色字形 —— 一对键里有一个永远看着像"被按住/选中",实机上就是那颗反白的按钮。两键现在共用
             同一常态外观,反白只出现在 Pressed 态(此时才换成 Accent 底 + 深色字形,给出按下反馈)。
             圆底做进模板,XAML 里不再需要包一层 Border。 -->
        <Style x:Key="ScrollFabBtn" TargetType="Button">
            <Setter Property="Width" Value="46" />
            <Setter Property="Height" Value="46" />
            <Setter Property="Padding" Value="0" />
            <Setter Property="FontSize" Value="19" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="Button">
                        <Border x:Name="FabRoot" Width="46" Height="46" CornerRadius="23" Background="{StaticResource SurfaceHi}" BorderBrush="{StaticResource Sep}" BorderThickness="1">
                            <VisualStateManager.VisualStateGroups>
                                <VisualStateGroup x:Name="CommonStates">
                                    <VisualState x:Name="Normal" />
                                    <VisualState x:Name="PointerOver" />
                                    <VisualState x:Name="Pressed">
                                        <Storyboard>
                                            <ObjectAnimationUsingKeyFrames Storyboard.TargetName="FabRoot" Storyboard.TargetProperty="Background">
                                                <DiscreteObjectKeyFrame KeyTime="0" Value="{StaticResource Accent}" />
                                            </ObjectAnimationUsingKeyFrames>
                                            <ObjectAnimationUsingKeyFrames Storyboard.TargetName="FabGlyph" Storyboard.TargetProperty="Foreground">
                                                <DiscreteObjectKeyFrame KeyTime="0" Value="#FF07110F" />
                                            </ObjectAnimationUsingKeyFrames>
                                        </Storyboard>
                                    </VisualState>
                                    <VisualState x:Name="Disabled">
                                        <Storyboard>
                                            <DoubleAnimation Storyboard.TargetName="FabRoot" Storyboard.TargetProperty="Opacity" To="0.4" Duration="0" />
                                        </Storyboard>
                                    </VisualState>
                                </VisualStateGroup>
                            </VisualStateManager.VisualStateGroups>
                            <ContentPresenter x:Name="FabGlyph" Foreground="{StaticResource TxtHi}" HorizontalAlignment="Center" VerticalAlignment="Center" Content="{TemplateBinding Content}" FontSize="{TemplateBinding FontSize}" />
                        </Border>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>
        <!-- 动作面板:顶部快捷图标列。Apotheosis: 图标与文字改成并排(见下面那一行的注释),
             行高因此只剩图标那一行,竖直内边距从 13 收到 6 —— 整行约 76 → 41 DIP,字形仍是 22。 -->
        <Style x:Key="QuickBtn" TargetType="Button">
            <Setter Property="Background" Value="Transparent" />
            <Setter Property="BorderThickness" Value=")APO",
        LR"APO(0" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="HorizontalAlignment" Value="Stretch" />
            <Setter Property="Padding" Value="0,6" />
        </Style>
        <!-- 动作面板:整行菜单项 -->
        <Style x:Key="MenuRow" TargetType="Button">
            <Setter Property="Background" Value="Transparent" />
            <Setter Property="BorderThickness" Value="0" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="HorizontalAlignment" Value="Stretch" />
            <Setter Property="HorizontalContentAlignment" Value="Left" />
            <Setter Property="FontSize" Value="16" />
            <Setter Property="Padding" Value="18,14" />
        </Style>
        <!-- 设置页的整行按钮 -->
        <Style x:Key="SetRow" TargetType="Button">
            <Setter Property="Background" Value="{StaticResource Surface}" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="BorderThickness" Value="0" />
            <Setter Property="HorizontalAlignment" Value="Stretch" />
            <Setter Property="HorizontalContentAlignment" Value="Left" />
            <Setter Property="Padding" Value="15,13" />
            <Setter Property="FontSize" Value="15" />
        </Style>
        <!-- 深色输入框(地址栏/查找条)。默认 TextBox 模板在 Focused 态把 BorderElement 刷白、
             把 ContentElement 切到 Light 主题(文字变黑) → 聚焦时整条地址栏发白。这里的模板
             (基于 SDK 15063 默认模板)让 Normal/PointerOver/Focused 三态外观完全一致,
             并把内置 DeleteButton 的触摸目标放大到 44x40(字形仍是 14);触摸区只向左扩,
             ✕ 本身仍贴输入框右缘(与默认模板同位置)。
             注意:不要给模板加 VerticalContentAlignment 绑定 —— 文字垂直居中仍靠元素上的 Padding。 -->
        <Style x:Key="DarkFieldBox" TargetType="TextBox">
            <Setter Property="MinWidth" Value="0" />
            <Setter Property="MinHeight" Value="0" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="Background" Value="Transparent" />
            <Setter Property="BorderBrush" Value="Transparent" />
            <Setter Property="BorderThickness" Value="0" />
            <Setter Property="SelectionHighlightColor" Value="{StaticResource Accent}" />
            <Setter Property="ScrollViewer.HorizontalScrollMode" Value="Auto" />
            <Setter Property="ScrollViewer.VerticalScrollMode" Value="Auto" />
            <Setter Property="ScrollViewer.HorizontalScrollBarVisibility" Value="Hidden" />
            <Setter Property="ScrollViewer.VerticalScrollBarVisibility" Value="Hidden" />
            <Setter Property="ScrollViewer.IsDeferredScrollingEnabled" Value="False" />
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="TextBox">
                        <Grid>
                            <Grid.Resources>
                                <!-- 清除键:透明底,大触摸目标,字形不变大 -->
                                <Style x:Key="DarkFieldDeleteButtonStyle" TargetType="Button">
                                    <Setter Property="Template">
                                        <Setter.Value>
                                            <ControlTemplate TargetType="Button">
                                                <Grid x:Name="ButtonLayoutGrid" Background="Transparent" BorderThickness="0">
                                                    <VisualStateManager.VisualStateGroups>
                                                        <VisualStateGroup x:Name="CommonStates">
                                                            <VisualState x:Name="Normal" />
                                                            <VisualState x:Name="PointerOver">
                                                                <Storyboard>
                                                                    <ObjectAnimationUsingKeyFrames Storyboard.TargetName="GlyphElement" Storyboard.TargetProperty="Foreground">
                                                                        <DiscreteObjectKeyFrame KeyTime="0" Value="{StaticResource TxtHi}" />
                                                                    </ObjectAnimationUsingKeyFrames>
                                                                </Storyboard>
                                                            </VisualState>
                                                            <VisualState x:Name="Pressed">
                                                                <Storyboard>
                                                                    <ObjectAnimationUsingKeyFrames Storyboard.TargetName="GlyphElement" Storyboard.TargetProperty="Foreground">
                                                                        <DiscreteObjectKeyFrame KeyTime="0" Value="{StaticResource Accent}" />
                                                                    </ObjectAnimationUsingKeyFrames>
                                                                </Storyboard>
                                                            </VisualState>
                                                            <VisualState x:Name="Disabled">
                                                                <Storyboard>
                                                                    <DoubleAnimation Storyboard.TargetName="ButtonLayoutGrid" Storyboard.TargetProperty="Opacity" To="0" Duration="0" />
                                                                </Storyboard>
                                                            </VisualState>
                                                        </VisualStateGroup>
                                                    </VisualStateManager.VisualStateGroups>
                                                    <!-- 字形靠右钉死:按钮比默认模板的 34 宽了 10,居中会把 ✕ 往左推 10/2+…;
         )APO",
        LR"APO(                                                右边距 10 让字形正好落在默认模板的位置,多出来的宽度全部向左扩成触摸区。 -->
                                                    <TextBlock x:Name="GlyphElement" Text="" FontFamily="Segoe MDL2 Assets" FontSize="14" FontStyle="Normal" Foreground="{StaticResource TxtLo}" HorizontalAlignment="Right" VerticalAlignment="Center" Margin="0,0,10,0" AutomationProperties.AccessibilityView="Raw" />
                                                </Grid>
                                            </ControlTemplate>
                                        </Setter.Value>
                                    </Setter>
                                </Style>
                            </Grid.Resources>
                            <VisualStateManager.VisualStateGroups>
                                <VisualStateGroup x:Name="CommonStates">
                                    <VisualState x:Name="Normal" />
                                    <!-- 空:PointerOver / Focused 与 Normal 完全同貌 -->
                                    <VisualState x:Name="PointerOver" />
                                    <VisualState x:Name="Focused" />
                                    <VisualState x:Name="Disabled">
                                        <Storyboard>
                                            <ObjectAnimationUsingKeyFrames Storyboard.TargetName="ContentElement" Storyboard.TargetProperty="Foreground">
                                                <DiscreteObjectKeyFrame KeyTime="0" Value="{StaticResource TxtLo}" />
                                            </ObjectAnimationUsingKeyFrames>
                                            <ObjectAnimationUsingKeyFrames Storyboard.TargetName="PlaceholderTextContentPresenter" Storyboard.TargetProperty="Foreground">
                                                <DiscreteObjectKeyFrame KeyTime="0" Value="{StaticResource TxtLo}" />
                                            </ObjectAnimationUsingKeyFrames>
                                        </Storyboard>
                                    </VisualState>
                                </VisualStateGroup>
                                <VisualStateGroup x:Name="ButtonStates">
                                    <VisualState x:Name="ButtonVisible">
                                        <Storyboard>
                                            <ObjectAnimationUsingKeyFrames Storyboard.TargetName="DeleteButton" Storyboard.TargetProperty="Visibility">
                                                <DiscreteObjectKeyFrame KeyTime="0">
                                                    <DiscreteObjectKeyFrame.Value>
                                                        <Visibility>Visible</Visibility>
                                                    </DiscreteObjectKeyFrame.Value>
                                                </DiscreteObjectKeyFrame>
                                            </ObjectAnimationUsingKeyFrames>
                                        </Storyboard>
                                    </VisualState>
                                    <VisualState x:Name="ButtonCollapsed" />
                                </VisualStateGroup>
                            </VisualStateManager.VisualStateGroups>
                            <Grid.ColumnDefinitions>
                                <ColumnDefinition Width="*" />
                                <ColumnDefinition Width="Auto" />
                            </Grid.ColumnDefinitions>
                            <Grid.RowDefinitions>
                                <RowDefinition Height="Auto" />
                                <RowDefinition Height="*" />
                            </Grid.RowDefinitions>
                            <Border x:Name="BorderElement" Grid.Row="1" Grid.RowSpan="1" Grid.ColumnSpan="2" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="{TemplateBinding BorderThickness}" />
                            <ContentPresenter x:Name="HeaderContentPresenter" x:DeferLoadStrategy="Lazy" Visibility="Collapsed" Grid.Row="0" Grid.ColumnSpan="2" Margin="0,0,0,8" FontWeight="Normal" Foreground="{StaticResource TxtLo}" Content="{TemplateBinding Header}" ContentTemplate="{TemplateBinding HeaderTemplate}" TextWrapping="{TemplateBinding TextWrapping}" />
                            <ScrollViewer x:Name="ContentElement" Grid.Row="1" HorizontalScrollMode="{TemplateBinding ScrollViewer.HorizontalScrollMode}" HorizontalScrollBarVisibility="{TemplateBinding ScrollViewer.HorizontalScrollBarVisibility}" VerticalScrollMode="{TemplateBinding ScrollViewer.VerticalScrollMode}" VerticalScrollBarVisibility="{TemplateBinding ScrollViewer.VerticalScrollBarVisibility}" IsHorizontalRailEnabled="{TemplateBinding ScrollViewer.IsHorizontalRailEnabled}" IsVerticalRailEnabled="{TemplateBinding ScrollViewer.IsVerticalRailEnabled}" IsDeferredScrollingEnabled="{TemplateBinding ScrollViewer.IsDeferredScrollingEnabled}" Margin="{TemplateBinding BorderThickness}" Padding="{TemplateBinding Padding}" IsTabStop="False" ZoomMode="Disabled" AutomationProperties.AccessibilityView="Raw" />
                            <ContentPresenter x:Name="PlaceholderTextContentPresenter" Grid.Row="1" Grid.ColumnSpan="2" Foreground="{StaticResource TxtLo}" IsHitTestVisible="False" Margin="{TemplateBinding BorderThickness}" Padding="{TemplateBinding Padding}" Content="{TemplateBinding PlaceholderText}" TextWrapping="{TemplateBinding TextWrapping}" />
                            <!-- 清除键固定贴输入框右缘(第 1 列 = Auto,靠右;绝不铺满内容列)。
                                 Margin 右 -2 = 默认模板的 HelperButtonThemePadding,位置与默认模板一致。 -->
                            <Button x:Name="DeleteButton" Grid.Row="1" Grid.Column="1" Style="{StaticResource DarkFieldDeleteButtonStyle}" Background="Transparent" BorderThickness="0" Padding="0" Margin="0,0,-2,0" MinWidth="44" MinHeight="36" Hor)APO",
        LR"APO(izontalAlignment="Right" VerticalAlignment="Stretch" IsTabStop="False" Visibility="Collapsed" AutomationProperties.AccessibilityView="Raw" />
                        </Grid>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>
    </Grid.Resources>
        <!-- 软键盘弹出时整体上移(仅地址栏聚焦时,见 code-behind InputPane 处理) -->
        <Grid.RenderTransform>
            <TranslateTransform x:Name="RootShift" Y="0" />
        </Grid.RenderTransform>
        <Grid.RowDefinitions>
            <RowDefinition Height="*" />      <!-- 内容(网页) -->
            <RowDefinition Height="Auto" />   <!-- 底部 chrome -->
        </Grid.RowDefinitions>

        <!-- ===== 内容区:网页渲染 + 浮层 ===== -->
        <Grid Grid.Row="0">
            <Border Background="White" Margin="6,6,6,0" CornerRadius="10">
                <!-- 自由滚动:内容区直接接 ManipulationDelta(单指拖→引擎滚动+惯性,捏合→Scale)。
                     点击走 Tapped;坐标用 GetPosition(ContentArea) 映回引擎像素(MapTapToEngine)。 -->
                <Grid x:Name="ContentArea" Background="White" ManipulationMode="TranslateX,TranslateY,TranslateInertia,Scale" IsTapEnabled="True">
                    <!-- 固定 720x1080 的软件帧须随可用视口铺满；否则横屏时右侧会留下空白。 -->
                    <Image x:Name="RenderImage" Stretch="Fill" />
                </Grid>
            </Border>

            <!-- GPU 直呈现面(SwapChainPanel);GPU 起来后承载 TextureMapper 合成输出。 -->
            <SwapChainPanel x:Name="GpuPanel" HorizontalAlignment="Stretch" VerticalAlignment="Stretch" IsHitTestVisible="False" Visibility="Collapsed" />

            <!-- 输入法捕获框:1×1 透明,聚焦唤起键盘;键入转发给引擎活会话。 -->
            <TextBox x:Name="ImeBox" Width="1" Height="1" Opacity="0" Margin="0" Padding="0" BorderThickness="0" MinWidth="0" MinHeight="0" HorizontalAlignment="Left" VerticalAlignment="Top" IsTabStop="True" />

            <!-- 顶部加载进度条 -->
            <ProgressBar x:Name="Progress" Height="3" VerticalAlignment="Top" Foreground="{StaticResource Accent}" Background="Transparent" IsIndeterminate="False" Visibility="Collapsed" />

            <!-- 悬浮翻页键(触发懒加载/看下方内容)。仅有会话时显示。 -->
            <StackPanel x:Name="ScrollFab" Orientation="Vertical" HorizontalAlignment="Right" VerticalAlignment="Bottom" Margin="0,0,16,18" Visibility="Collapsed">
                <Button Style="{StaticResource ScrollFabBtn}" Margin="0,0,0,9" Content="▲" x:Name="_ev1" />
                <Button Style="{StaticResource ScrollFabBtn}" Content="▼" x:Name="_ev2" />
            </StackPanel>

            <!-- 页内查找条(顶部浮条) -->
            <Border x:Name="FindBar" VerticalAlignment="Top" HorizontalAlignment="Stretch" Background="{StaticResource Chrome}" BorderBrush="{StaticResource Sep}" BorderThickness="0,0,0,1" Visibility="Collapsed">
                <Grid Margin="10,8">
                    <Grid.ColumnDefinitions>
                        <ColumnDefinition Width="*" />
                        <ColumnDefinition Width="Auto" />
                        <ColumnDefinition Width="Auto" />
                        <ColumnDefinition Width="Auto" />
                        <ColumnDefinition Width="Auto" />
                    </Grid.ColumnDefinitions>
                    <Border Grid.Column="0" Background="{StaticResource Inset}" BorderBrush="{StaticResource Sep}" BorderThickness="1" CornerRadius="18">
                        <TextBox x:Name="FindBox" Style="{StaticResource DarkFieldBox}" FontSize="15" Height="36" BorderThickness="0" Background="Transparent" Foreground="{StaticResource TxtHi}" VerticalContentAlignment="Center" MinHeight="0" Padding="10,8,10,6" PlaceholderText="页内查找" />
                    </Border>
                    <TextBlock x:Name="FindCount" Grid.Column="1" Text="" Foreground="{StaticResource TxtLo}" FontSize="13" VerticalAlignment="Center" Margin="10,0" />
                    <Button x:Name="FindPrev" Grid.Column="2" Style="{StaticResource IconBtn}" Content="▲" FontSize="13" />
                    <Button x:Name="FindNext" Grid.Column="3" Style="{StaticResource IconBtn}" Content="▼" FontSize="13" />
                    <Button x:Name="FindClose" Grid.Column="4" Style="{StaticResource IconBtn}" Content="✕" />
                </Grid>
            </Border>

            <!-- 地址栏建议下拉:锚定内容区底部 → 浮在底栏正上方。点项即导航。 -->
            <Border x:Name="SuggestPanel" VerticalAlignment="Bottom" HorizontalAlignment="Stretch" Background="{StaticResource Chrome}" BorderBrush="{StaticResource Sep}" BorderThickness="0,1,0,0" Margin="8,0" CornerRadius="12,12,0,0" Visibility="Collapsed">
                <ScrollViewer MaxHeight="340" VerticalScrollBarVisibility="Auto">
                    <StackPanel x:Name="SuggestList" Margin="8,6" />
                </ScrollViewer>
            </Border>
        </Grid>

        <!-- ===== 底部 chrome:状态行 + 导航栏 ===== -->
        <Grid Grid.Row="1">
            <Grid.RowDefinitions>
                <RowDefinition Height="Auto" />
                <RowDefinition Height="Auto" />
            </Grid.RowDefinitions>

            <!-- 细状态行:页面标题 / 临时提示(code 大量写 TitleText 当 toast) -->
            <Border Grid.Row="0" Background="{StaticResource Chrome}" BorderBrush="{StaticResource Sep}" BorderThickness="0,1,0,0">
                <Grid Margin="14,4,14,3">
                    <Grid.ColumnDefinitions><ColumnDefinition Width="Auto" /><ColumnDefinition Width="*" /></Grid.ColumnDefinitions>
                    <Ellipse Grid.Column="0" Width="5" Height="5" Fill="{StaticResource Accent}" VerticalAlignment="Center" Margin="0,0,8,0" />
                    <TextBlock x:Name="TitleText" Grid.Column="1" Text="APOTHEOSIS" Foreground="{StaticResource TxtLo}" FontSize="11" CharacterSpacing="90" HorizontalAlignment="Left" TextTrimming="CharacterEllipsis" />
                </Grid>
            </Border>

            <!-- 导航栏:[标签数] | 🔒 地址 [Go/⟳/✕] | ⋯ -->
            <Grid Grid.Row="1" Height="62" Background="{StaticResource Chrome}">
                <Grid.ColumnDefini)APO",
        LR"APO(tions>
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="*" />
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="Auto" />
                </Grid.ColumnDefinitions>

                <!-- 标签键:方框数字,点开标签切换器 -->
                <Button x:Name="TabsBtn" Grid.Column="0" Background="Transparent" BorderThickness="0" Width="54" Height="62" Padding="0" IsHoldingEnabled="False">
                    <Border BorderBrush="{StaticResource Accent}" Background="{StaticResource AccentDim}" BorderThickness="1.5" CornerRadius="7" Width="28" Height="28">
                        <TextBlock x:Name="TabCountText" Text="1" Foreground="{StaticResource Accent}" FontSize="12" FontWeight="SemiBold" HorizontalAlignment="Center" VerticalAlignment="Center" />
                    </Border>
                </Button>

                <!-- Apotheosis: the hairline separators that used to sit in columns 1 and 3 are
                     gone — on a 432 DIP phone screen every DIP of the address field counts, and
                     the pill's own rounded border already separates it from the two keys. The
                     Auto columns stay (empty = zero width) so no Grid.Column index moves. -->

                <!-- 地址胶囊:锁标 + 地址输入 + 上下文键(Go/刷新/停止)。三者统一 40 高。UWP TextBox 不理会 VerticalContentAlignment(模板没绑),文字靠 Padding 上下值居中。 -->
                <Border Grid.Column="2" Background="{StaticResource Inset}" BorderBrush="{StaticResource Sep}" BorderThickness="1" CornerRadius="20" Margin="7,9" Padding="0" Height="42">
                    <Grid>
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="Auto" />
                            <ColumnDefinition Width="*" />
                            <ColumnDefinition Width="Auto" />
                        </Grid.ColumnDefinitions>
                        <!-- 锁标:固定居中槽,glyph 与地址文字同基线 -->
                        <TextBlock x:Name="LockIcon" Grid.Column="0" Text="" FontFamily="Segoe MDL2 Assets" FontSize="14" Foreground="{StaticResource Warm}" TextLineBounds="Tight" VerticalAlignment="Center" HorizontalAlignment="Center" Margin="10,0,2,0" />
                        <TextBox x:Name="UrlBox" Grid.Column="1" Style="{StaticResource DarkFieldBox}" FontSize="15" Height="40" MinHeight="0" Margin="0" BorderThickness="0" Background="Transparent" Foreground="{StaticResource TxtHi}" VerticalAlignment="Center" VerticalContentAlignment="Center" Padding="6,10,6,8" InputScope="Url" Text="" PlaceholderText="搜索或输入网址" />
                        <!-- Apotheosis: glyph in its own TextBlock (TextLineBounds="Tight", like LockIcon) —
                             plain Button.Content centred on the font's line box, not its glyph ink, which
                             sat visibly low for U+21BB. UpdateUrlActionGlyph sets UrlActionGlyph->Text now. -->
                        <Button x:Name="UrlActionBtn" Grid.Column="2" Background="Transparent" BorderThickness="0" Width="42" Height="40" Padding="0" VerticalAlignment="Center" HorizontalContentAlignment="Center" VerticalContentAlignment="Center">
                            <TextBlock x:Name="UrlActionGlyph" Text="↻" FontSize="17" TextLineBounds="Tight" Foreground="{StaticResource Accent}" HorizontalAlignment="Center" VerticalAlignment="Center" />
                        </Button>
                        <!-- 编辑地址时占用同一格:白色清除键顶掉刷新/停止键(见 OnUrlGotFocus/OnUrlLostFocus),
                             输入框因此拿到整条胶囊的宽度。IsTabStop=False → 点它不夺焦,软键盘不收。 -->
                        <Button x:Name="UrlClearBtn" Grid.Column="2" Background="Transparent" BorderThickness="0" Foreground="{StaticResource TxtHi}" Width="44" Height="40" Padding="0" FontSize="17" IsTabStop="False" Visibility="Collapsed" VerticalAlignment="Center" VerticalContentAlignment="Center" HorizontalContentAlignment="Center" Content="✕" />
                    </Grid>
                </Border>

                <!-- 菜单(More):弹出底部动作面板 -->
                <Button x:Name="MenuBtn" Grid.Column="4" Style="{StaticResource NavBtn}" Content="" />
            </Grid>
        </Grid>

        <!-- ============================================================================
             以下为全屏/底部浮层。Grid.RowSpan=2 覆盖内容+底栏。
             ============================================================================ -->

        <!-- ===== 动作面板(⋯ 弹出的底部 sheet)===== -->
        <Grid x:Name="ActionMenu" Grid.Row="0" Grid.RowSpan="2" Background="{StaticResource Scrim}" Visibility="Collapsed">
            <Border VerticalAlignment="Bottom" Background="{StaticResource Surface}" BorderBrush="{StaticResource Sep}" BorderThickness="1,1,1,0" CornerRadius="20,20,0,0" Margin="6,0" x:Name="_ev3">
                <ScrollViewer VerticalScrollBarVisibility="Auto" MaxHeight="520">
                    <StackPanel Margin="0,12,0,18">
                        <Grid Margin="18,0,18,10">
                            <Grid.ColumnDefinitions><ColumnDefinition Width="*" /><ColumnDefinition Width="Auto" /></Grid.ColumnDefinitions>
                            <StackPanel Grid.Column="0">
                                <TextBlock Text="PAGE COMMANDS" Foreground="{StaticResource Accent}" FontSize="11" CharacterSpacing="120" />
                                <TextBlock Text="当前页面" Foreground="{StaticResource TxtHi}" FontSize="21" FontWeight="SemiBold" Margin="0,3,0,0" />
                            </StackPanel>
                            <Border Grid.Column="1" Width="34" Height="4" CornerRadius="2" Background="{StaticResource Sep}" VerticalAlignment="Top" Margin="0,3,0,0" />
                        </Grid>
                        <!-- Apotheosis: 快捷行 = 后退 / 前进 / 刷新 / 收藏。刷新原先只在地址栏上下文键上,
                             但那颗键在编辑地址时会被清除键顶掉,而且这一排本来就是"当前页面"的动作,
                             缺一个刷新反而要绕路;Tag="reloa)APO",
        LR"APO(d" 已被 OnAction 分发,无需新代码。
                             图标与文字由上下叠放改为并排:行高从"图标+间距+文字+两倍内边距"塌成"图标+两倍内边距",
                             约 76 → 41 DIP(要求的一半),而字形仍是 22 —— 只是排布变了,图标没缩小。 -->
                        <Grid Margin="10,2,10,8" Background="{StaticResource Inset}">
                            <Grid.ColumnDefinitions>
                                <ColumnDefinition Width="*" /><ColumnDefinition Width="*" />
                                <ColumnDefinition Width="*" /><ColumnDefinition Width="*" />
                            </Grid.ColumnDefinitions>
                            <Button x:Name="BackBtn" Grid.Column="0" Style="{StaticResource QuickBtn}" IsEnabled="False">
                                <StackPanel Orientation="Horizontal" HorizontalAlignment="Center">
                                    <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="22" VerticalAlignment="Center" Foreground="{StaticResource TxtHi}" />
                                    <TextBlock Text="后退" FontSize="12" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" Margin="6,1,0,0" />
                                </StackPanel>
                            </Button>
                            <Button x:Name="FwdBtn" Grid.Column="1" Style="{StaticResource QuickBtn}" IsEnabled="False">
                                <StackPanel Orientation="Horizontal" HorizontalAlignment="Center">
                                    <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="22" VerticalAlignment="Center" Foreground="{StaticResource TxtHi}" />
                                    <TextBlock Text="前进" FontSize="12" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" Margin="6,1,0,0" />
                                </StackPanel>
                            </Button>
                            <Button Grid.Column="2" Tag="reload" Style="{StaticResource QuickBtn}" x:Name="_ev4">
                                <StackPanel Orientation="Horizontal" HorizontalAlignment="Center">
                                    <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="22" VerticalAlignment="Center" Foreground="{StaticResource Accent}" />
                                    <TextBlock Text="刷新" FontSize="12" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" Margin="6,1,0,0" />
                                </StackPanel>
                            </Button>
                            <Button Grid.Column="3" Tag="bookmark" Style="{StaticResource QuickBtn}" x:Name="_ev5">
                                <StackPanel Orientation="Horizontal" HorizontalAlignment="Center">
                                    <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="22" VerticalAlignment="Center" Foreground="{StaticResource Warm}" />
                                    <TextBlock x:Name="ActFavLabel" Text="收藏" FontSize="12" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" Margin="6,1,0,0" />
                                </StackPanel>
                            </Button>
                        </Grid>

                        <TextBlock Text="BROWSE" Foreground="{StaticResource TxtLo}" FontSize="10" CharacterSpacing="120" Margin="18,8,18,3" />

                        <Button Tag="newtab" Style="{StaticResource MenuRow}" x:Name="_ev6">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="新标签页" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                        <Button Tag="home" Style="{StaticResource MenuRow}" x:Name="_ev7">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="主页" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                        <Button Tag="ua" Style="{StaticResource MenuRow}" x:Name="_ev8">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock x:Name="ActUaLabel" Text="桌面版网站" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                        <Button Tag="find" Style="{StaticResource MenuRow}" x:Name="_ev9">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="页内查找" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                        <Button Tag="share" Style="{StaticResource MenuRow}" x:Name="_ev10">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="分享" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                        <Button Tag="copylink" Style="{StaticResource MenuRow}" x:Name="_ev11">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock)APO",
        LR"APO( Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="复制链接" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                        <Button Tag="download" Style="{StaticResource MenuRow}" x:Name="_ev12">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="下载此页" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>

                        <TextBlock Text="LIBRARY" Foreground="{StaticResource TxtLo}" FontSize="10" CharacterSpacing="120" Margin="18,10,18,3" />

                        <Button Tag="bookmarks" Style="{StaticResource MenuRow}" x:Name="_ev13">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="书签" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                        <Button Tag="history" Style="{StaticResource MenuRow}" x:Name="_ev14">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="历史记录" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                        <Button Tag="downloads" Style="{StaticResource MenuRow}" x:Name="_ev15">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="下载内容" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>

                        <Border Height="1" Background="{StaticResource Sep}" Margin="16,10,16,5" />

                        <Button Tag="settings" Style="{StaticResource MenuRow}" x:Name="_ev16">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="设置" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                    </StackPanel>
                </ScrollViewer>
            </Border>
        </Grid>

        <!-- ===== 抽屉:收藏/历史/下载 ===== -->
        <Grid x:Name="Drawer" Grid.Row="0" Grid.RowSpan="2" Background="{StaticResource PageBg}" Visibility="Collapsed">
            <Grid.RowDefinitions>
                <RowDefinition Height="Auto" />
                <RowDefinition Height="Auto" />
                <RowDefinition Height="*" />
            </Grid.RowDefinitions>

            <Grid Grid.Row="0" Background="{StaticResource Chrome}" Padding="8,8" BorderBrush="{StaticResource Sep}" BorderThickness="0,0,0,1">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*" />
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="Auto" />
                </Grid.ColumnDefinitions>
                <StackPanel Grid.Column="0" Margin="10,0" VerticalAlignment="Center">
                    <TextBlock Text="LIBRARY" Foreground="{StaticResource Accent}" FontSize="10" CharacterSpacing="140" />
                    <TextBlock Text="浏览资料库" Foreground="{StaticResource TxtHi}" FontSize="21" FontWeight="SemiBold" Margin="0,2,0,0" />
                </StackPanel>
                <Button x:Name="GpuBtn" Grid.Column="1" Style="{StaticResource TabBtn}" Foreground="{StaticResource TxtLo}" Content="🖥 GPU" VerticalAlignment="Center" />
                <Button x:Name="UaBtn" Grid.Column="2" Style="{StaticResource TabBtn}" Foreground="{StaticResource Accent}" Content="📱 手机UA" VerticalAlignment="Center" />
                <Button Grid.Column="3" Style="{StaticResource IconBtn}" Content="✕" x:Name="_ev17" />
            </Grid>

            <Grid Grid.Row="1" Background="{StaticResource Chrome}" Margin="8,8,8,0">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*" />
                    <ColumnDefinition Width="*" />
                    <ColumnDefinition Width="*" />
                    <ColumnDefinition Width="Auto" />
                </Grid.ColumnDefinitions>
                <Button x:Name="TabFav" Grid.Column="0" Style="{StaticResource TabBtn}" Content="★ 收藏" />
                <Button x:Name="TabHist" Grid.Column="1" Style="{StaticResource TabBtn}" Content="🕑 历史" />
                <Button x:Name="TabDl" Grid.Column="2" Style="{StaticResource TabBtn}" Content="↓ 下载" />
                <Button x:Name="ActionBtn" Grid.Column="3" Style="{StaticResource TabBtn}" Foreground="{StaticResource Accent}" Content="★ 收藏此页" />
            </Grid>

            <ScrollViewer Grid.Row="2" VerticalScrollBarVisibility="Auto">
                <StackPanel x:Name="DrawerList" Margin="10,10,10,18" />
            </ScrollViewer>
        </Grid>

        <!-- ===== 设置页(全屏)===== -->
        <Grid x:Name="SettingsPage")APO",
        LR"APO( Grid.Row="0" Grid.RowSpan="2" Background="{StaticResource PageBg}" Visibility="Collapsed">
            <Grid.RowDefinitions>
                <RowDefinition Height="Auto" />
                <RowDefinition Height="*" />
            </Grid.RowDefinitions>
            <Grid Grid.Row="0" Background="{StaticResource Chrome}" Padding="8,8" BorderBrush="{StaticResource Sep}" BorderThickness="0,0,0,1">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="*" />
                </Grid.ColumnDefinitions>
                <Button Grid.Column="0" Style="{StaticResource IconBtn}" FontFamily="Segoe MDL2 Assets" Content="" x:Name="_ev18" />
                <StackPanel Grid.Column="1" VerticalAlignment="Center" Margin="7,0">
                    <TextBlock Text="SYSTEM" Foreground="{StaticResource Accent}" FontSize="10" CharacterSpacing="140" />
                    <TextBlock Text="浏览器设置" Foreground="{StaticResource TxtHi}" FontSize="20" FontWeight="SemiBold" Margin="0,1,0,0" />
                </StackPanel>
            </Grid>
            <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto">
                <StackPanel Margin="16,14">
                    <!-- 界面语言:首启 OOBE 选定,这里可随时改(离开设置页即生效,见 HideSettings)。 -->
                    <TextBlock Text="LANGUAGE" Foreground="{StaticResource Accent}" FontSize="10" CharacterSpacing="130" Margin="0,6,0,7" />
                    <TextBlock Text="界面语言" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,0,0,4" />
                    <ComboBox x:Name="SetLangCombo" HorizontalAlignment="Stretch">
                        <ComboBoxItem Content="中文" />
                        <ComboBoxItem Content="English" />
                    </ComboBox>

                    <TextBlock Text="SEARCH &amp; START" Foreground="{StaticResource Accent}" FontSize="10" CharacterSpacing="130" Margin="0,22,0,7" />
                    <TextBlock Text="默认搜索引擎" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,0,0,4" />
                    <ComboBox x:Name="SetSearchCombo" HorizontalAlignment="Stretch">
                        <ComboBoxItem Content="Bing" />
                        <ComboBoxItem Content="Google" />
                        <ComboBoxItem Content="DuckDuckGo" />
                        <ComboBoxItem Content="百度" />
                        <ComboBoxItem Content="Qwant" />
                    </ComboBox>

                    <TextBlock Text="主页(URL,留空用内置主页)" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,18,0,4" />
                    <TextBox x:Name="SetHomeBox" HorizontalAlignment="Stretch" InputScope="Url" PlaceholderText="about:home" />

                    <ToggleSwitch x:Name="SetUaSwitch" Header="启动请求桌面版网站" Foreground="{StaticResource TxtHi}" Margin="0,18,0,0" />

                    <TextBlock Text="自定义 User-Agent(留空=用上面的开关;改后刷新网页生效)" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,16,0,4" />
                    <TextBox x:Name="SetUaCustomBox" HorizontalAlignment="Stretch" TextWrapping="Wrap" AcceptsReturn="False" PlaceholderText="Mozilla/5.0 (Windows NT 10.0; Win64; x64) ... Chrome/120 Safari/537.36 Edg/120" />

                    <Grid Margin="0,16,0,0">
                        <Grid.ColumnDefinitions><ColumnDefinition Width="*" /><ColumnDefinition Width="Auto" /></Grid.ColumnDefinitions>
                        <TextBlock Grid.Column="0" Text="默认缩放" Foreground="{StaticResource TxtHi}" FontSize="16" VerticalAlignment="Center" />
                        <TextBlock Grid.Column="1" x:Name="SetZoomLabel" Text="100%" Foreground="{StaticResource TxtLo}" FontSize="14" VerticalAlignment="Center" />
                    </Grid>
                    <Slider x:Name="SetZoomSlider" Minimum="50" Maximum="200" StepFrequency="10" Value="100" />

                    <ToggleSwitch x:Name="SetTabModeSwitch" Header="并发多引擎标签(暂搁置,后续实现)" IsEnabled="False" Foreground="{StaticResource TxtHi}" Margin="0,4,0,0" />

                    <TextBlock Text="RENDERING" Foreground="{StaticResource Accent}" FontSize="10" CharacterSpacing="130" Margin="0,22,0,7" />
                    <ToggleSwitch x:Name="SetGpuSwitch" Header="默认启用 GPU 渲染(加载首个网页后自动开)" Foreground="{StaticResource TxtHi}" Margin="0,0,0,6" />
                    <Button Tag="gpu" Style="{StaticResource SetRow}" Content="立即开启 GPU 合成(重启回软件)" x:Name="_ev19" />

                    <TextBlock Text="PRIVACY" Foreground="{StaticResource Warm}" FontSize="10" CharacterSpacing="130" Margin="0,22,0,7" />
                    <Button Tag="clearhist" Style="{StaticResource SetRow}" Content="清除历史记录" Margin="0,0,0,6" x:Name="_ev20" />
                    <Button Tag="clearfav" Style="{StaticResource SetRow}" Content="清除全部收藏" Margin="0,0,0,6" x:Name="_ev21" />
                    <Button Tag="cleardl" Style="{StaticResource SetRow}" Content="清除下载记录" Margin="0,0,0,6" x:Name="_ev22" />
                    <Button Tag="clearcookies" Style="{StaticResource SetRow}" Foreground="{StaticResource Danger}" Content="清除 Cookie(退出全部登录)" x:Name="_ev23" />

                    <!-- 自动检查更新：唯一一条非用户发起的外部请求（api.github.com），默认关。 -->
                    <ToggleSwitch x:Name="SetUpdateSwitch" Header="自动检查更新" Foreground="{StaticResource TxtHi}" Margin="0,12,0,2" />
                    <TextBlock Text="开启后每次启动会连接 api.github.com 一次" Foreground="{StaticResource TxtLo}" FontSize="13" TextWrapping="Wrap" Margin="0,0,0,8" />
                    <Button Tag="checkupdate" Style="{StaticResource SetRow}" Content="立即检查更新" x:Name="_ev24" />

                    <!-- 推测预取（speculation rules）：页面可提前取用户未点击的 URL。默认关； -->
                    <!-- “仅 Wi-Fi”按连接资费判定（NetworkCostType::Unrestricted），网络变化时重算。 -->
                    <TextBlock Text="预取网站建议的页面" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,18,0,4" />
                    <ComboBox x:Name="SetPrefetchCombo" HorizontalAlign)APO",
        LR"APO(ment="Stretch">
                        <ComboBoxItem Content="关闭预取" />
                        <ComboBoxItem Content="仅 Wi-Fi" />
                        <ComboBoxItem Content="始终" />
                    </ComboBox>
                    <TextBlock Text="网站可提前加载你还没点击的链接" Foreground="{StaticResource TxtLo}" FontSize="13" TextWrapping="Wrap" Margin="0,4,0,0" />

                    <TextBlock Text="DIAGNOSTICS" Foreground="{StaticResource Accent}" FontSize="10" CharacterSpacing="130" Margin="0,22,0,7" />
                    <Button Tag="export" Style="{StaticResource SetRow}" Content="导出调试日志 / 崩溃 dump" x:Name="_ev25" />

                    <!-- 开发者选项:默认关闭的调试用界面元素。 -->
                    <TextBlock Text="DEVELOPER" Foreground="{StaticResource Accent}" FontSize="10" CharacterSpacing="130" Margin="0,22,0,7" />
                    <TextBlock Text="开发者选项" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,0,0,4" />
                    <ToggleSwitch x:Name="SetScrollFabSwitch" Header="显示翻页按钮" Foreground="{StaticResource TxtHi}" Margin="0,0,0,6" />
                    <ToggleSwitch x:Name="SetInstantPanSwitch" Header="即时跟手滚动(实验)" Foreground="{StaticResource TxtHi}" Margin="0,0,0,6" />
                    <ToggleSwitch x:Name="SetThreadedRasterSwitch" Header="多线程栅格化(实验)" Foreground="{StaticResource TxtHi}" Margin="0,0,0,6" />

                    <TextBlock Text="ABOUT" Foreground="{StaticResource Accent}" FontSize="10" CharacterSpacing="130" Margin="0,22,0,7" />
                    <TextBlock x:Name="VersionText" Text="版本 —" Foreground="{StaticResource TxtHi}" FontSize="15" Margin="0,0,0,8" />

                    <!-- App 版本由 ShowSettings 从包清单填进来(见 code-behind)。 -->
                    <TextBlock x:Name="AboutFooterText" Text="EdgeHTML Reborn / Apotheosis — WebKit (WebCore) 2.52.4 — ARM32 UWP" Foreground="#FF80868B" FontSize="12" TextWrapping="Wrap" Margin="0,22,0,24" />
                </StackPanel>
            </ScrollViewer>
        </Grid>

        <!-- ===== 标签切换器(全屏)===== -->
        <Grid x:Name="TabSwitcher" Grid.Row="0" Grid.RowSpan="2" Background="{StaticResource PageBg}" Visibility="Collapsed">
            <Grid.RowDefinitions>
                <RowDefinition Height="Auto" />
                <RowDefinition Height="*" />
                <RowDefinition Height="Auto" />
            </Grid.RowDefinitions>
            <Grid Grid.Row="0" Background="{StaticResource Chrome}" Padding="10,10" BorderBrush="{StaticResource Sep}" BorderThickness="0,0,0,1">
                <Grid.ColumnDefinitions><ColumnDefinition Width="*" /><ColumnDefinition Width="Auto" /></Grid.ColumnDefinitions>
                <StackPanel Grid.Column="0" Margin="8,0" VerticalAlignment="Center">
                    <TextBlock Text="WORKSPACE" Foreground="{StaticResource Accent}" FontSize="10" CharacterSpacing="140" />
                    <TextBlock x:Name="TabSwitcherTitle" Text="标签" Foreground="{StaticResource TxtHi}" FontSize="21" FontWeight="SemiBold" Margin="0,2,0,0" />
                </StackPanel>
                <Button Grid.Column="1" Style="{StaticResource TabBtn}" Foreground="{StaticResource Accent}" Content="完成" x:Name="_ev26" />
            </Grid>
            <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto">
                <StackPanel x:Name="TabList" Margin="12,12" />
            </ScrollViewer>
            <Button Grid.Row="2" HorizontalAlignment="Stretch" HorizontalContentAlignment="Center" Background="{StaticResource AccentDim}" Foreground="{StaticResource Accent}" BorderBrush="{StaticResource Accent}" BorderThickness="0,1,0,0" Padding="0,16" x:Name="_ev27">
                <StackPanel Orientation="Horizontal">
                    <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="15" VerticalAlignment="Center" Foreground="{StaticResource Accent}" />
                    <TextBlock Text="新建标签页" Margin="10,0,0,0" VerticalAlignment="Center" Foreground="{StaticResource Accent}" />
                </StackPanel>
            </Button>
        </Grid>

        <!-- ===== 首启 OOBE:欢迎 + 选语言(English / 中文)。仅全新安装(settings.ini 无 lang)弹出。 ===== -->
        <Grid x:Name="OobePanel" Grid.Row="0" Grid.RowSpan="2" Background="{StaticResource PageBg}" Visibility="Collapsed">
            <StackPanel VerticalAlignment="Center" HorizontalAlignment="Center" Margin="36,0" MaxWidth="380">
                <Border Width="64" Height="64" CornerRadius="32" BorderBrush="{StaticResource Accent}" BorderThickness="2" Background="{StaticResource AccentDim}" Margin="0,0,0,22">
                    <TextBlock Text="A" Foreground="{StaticResource Accent}" FontSize="28" FontWeight="SemiBold" HorizontalAlignment="Center" VerticalAlignment="Center" />
                </Border>
                <TextBlock Text="APOTHEOSIS" Foreground="{StaticResource Accent}" FontSize="12" CharacterSpacing="220" HorizontalAlignment="Center" />
                <TextBlock Text="EdgeHTML Reborn" Foreground="{StaticResource TxtHi}" FontSize="30" FontWeight="SemiBold" HorizontalAlignment="Center" Margin="0,6,0,0" />
                <TextBlock Text="Modern web, reborn on Windows Phone" Foreground="{StaticResource TxtLo}" FontSize="13" HorizontalAlignment="Center" TextAlignment="Center" TextWrapping="Wrap" Margin="0,10,0,0" />
                <TextBlock Text="让被放弃的 Windows Phone 重新跑现代网页" Foreground="{StaticResource TxtLo}" FontSize="13" HorizontalAlignment="Center" TextAlignment="Center" TextWrapping="Wrap" Margin="0,2,0,0" />
                <TextBlock Text="Choose your language · 选择语言" Foreground="{StaticResource TxtHi}" FontSize="16" HorizontalAlignment="Center" Margin="0,44,0,18" />
                <Border Background="{StaticResource Accent}" CornerRadius="4" Margin="0,0,0,12">
                    <Button Tag="en" Background="Transparent" Foreground="#FF07110F" BorderThickness="0" HorizontalAlignment="Stretch" HorizontalContentAlignment="Center" Padding="0,16" FontSize="18" Con)APO",
        LR"APO(tent="English" x:Name="_ev28" />
                </Border>
                <Border Background="{StaticResource Surface}" BorderBrush="{StaticResource Sep}" BorderThickness="1" CornerRadius="4">
                    <Button Tag="zh" Background="Transparent" Foreground="{StaticResource TxtHi}" BorderThickness="0" HorizontalAlignment="Stretch" HorizontalContentAlignment="Center" Padding="0,16" FontSize="18" Content="中文" x:Name="_ev29" />
                </Border>
            </StackPanel>
        </Grid>
    </Grid>)APO",
    };
    std::wstring __s;
    for (auto __p : __c) __s += __p;
    return ref new ::Platform::String(__s.c_str());
}

void MainPage::InitializeComponent() {
    if (_contentLoaded) return;
    _contentLoaded = true;
    this->RequestedTheme = ::Windows::UI::Xaml::ElementTheme::Dark;
    // 运行期加载内嵌 XAML(绕开崩溃的 XamlCompiler);根上 {StaticResource} 已替成字面值。
    auto __root = safe_cast<::Windows::UI::Xaml::FrameworkElement^>(::Windows::UI::Xaml::Markup::XamlReader::Load(__MainPageXaml()));
    this->Content = __root;
    // ---- 绑定 x:Name 字段 ----
    RootGrid = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root);
    RootShift = safe_cast<::Windows::UI::Xaml::Media::TranslateTransform^>(__root->FindName(L"RootShift"));
    OobePanel = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root->FindName(L"OobePanel"));
    ActionMenu = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root->FindName(L"ActionMenu"));
    Drawer = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root->FindName(L"Drawer"));
    SettingsPage = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root->FindName(L"SettingsPage"));
    TabSwitcher = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root->FindName(L"TabSwitcher"));
    TabList = safe_cast<::Windows::UI::Xaml::Controls::StackPanel^>(__root->FindName(L"TabList"));
    TabSwitcherTitle = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"TabSwitcherTitle"));
    SetLangCombo = safe_cast<::Windows::UI::Xaml::Controls::ComboBox^>(__root->FindName(L"SetLangCombo"));
    SetSearchCombo = safe_cast<::Windows::UI::Xaml::Controls::ComboBox^>(__root->FindName(L"SetSearchCombo"));
    SetHomeBox = safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"SetHomeBox"));
    SetUaSwitch = safe_cast<::Windows::UI::Xaml::Controls::ToggleSwitch^>(__root->FindName(L"SetUaSwitch"));
    SetUaCustomBox = safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"SetUaCustomBox"));
    SetZoomSlider = safe_cast<::Windows::UI::Xaml::Controls::Slider^>(__root->FindName(L"SetZoomSlider"));
    SetTabModeSwitch = safe_cast<::Windows::UI::Xaml::Controls::ToggleSwitch^>(__root->FindName(L"SetTabModeSwitch"));
    SetGpuSwitch = safe_cast<::Windows::UI::Xaml::Controls::ToggleSwitch^>(__root->FindName(L"SetGpuSwitch"));
    SetUpdateSwitch = safe_cast<::Windows::UI::Xaml::Controls::ToggleSwitch^>(__root->FindName(L"SetUpdateSwitch"));
    SetPrefetchCombo = safe_cast<::Windows::UI::Xaml::Controls::ComboBox^>(__root->FindName(L"SetPrefetchCombo"));
    SetScrollFabSwitch = safe_cast<::Windows::UI::Xaml::Controls::ToggleSwitch^>(__root->FindName(L"SetScrollFabSwitch"));
    SetInstantPanSwitch = safe_cast<::Windows::UI::Xaml::Controls::ToggleSwitch^>(__root->FindName(L"SetInstantPanSwitch"));
    SetThreadedRasterSwitch = safe_cast<::Windows::UI::Xaml::Controls::ToggleSwitch^>(__root->FindName(L"SetThreadedRasterSwitch"));
    VersionText = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"VersionText"));
    AboutFooterText = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"AboutFooterText"));
    SetZoomLabel = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"SetZoomLabel"));
    DrawerList = safe_cast<::Windows::UI::Xaml::Controls::StackPanel^>(__root->FindName(L"DrawerList"));
    TabFav = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"TabFav"));
    TabHist = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"TabHist"));
    TabDl = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"TabDl"));
    ActionBtn = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"ActionBtn"));
    GpuBtn = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"GpuBtn"));
    UaBtn = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"UaBtn"));
    ActUaLabel = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"ActUaLabel"));
    BackBtn = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"BackBtn"));
    FwdBtn = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FwdBtn"));
    ActFavLabel = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"ActFavLabel"));
    TabsBtn = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"TabsBtn"));
    MenuBtn = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"MenuBtn"));
    LockIcon = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"LockIcon"));
    UrlBox = safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"UrlBox"));
    UrlActionBtn = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"UrlActionBtn"));
    UrlActionGlyph = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"UrlActionGlyph"));
    UrlClearBtn = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"UrlClearBtn"));
    TabCountText = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"TabCountText"));
    TitleText = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"TitleText"));
    GpuPanel = safe_cast<::Windows::UI::Xaml::Controls::SwapChainPanel^>(__root->FindName(L"GpuPanel"));
    ImeBox = safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"ImeBox"));
    Progress = safe_cast<::Windows::UI::Xaml::Controls::ProgressBar^>(__root->FindName(L"Progress"));
    ScrollFab = safe_cast<::Windows::UI::Xaml::Controls::StackPanel^>(__root->FindName(L"ScrollFab"));
    FindBar = safe_cast<::Windows::UI::Xaml::Controls::Border^>(__root->FindName(L"FindBar"));
    SuggestPanel = safe_cast<::Windows::UI::Xaml::Controls::Border^>(__root->FindName(L"SuggestPanel"));
    SuggestList = safe_cast<::Windows::UI::Xaml::Controls::StackPanel^>(__root->FindName(L"SuggestList"));
    FindCount = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"FindCount"));
    FindPrev = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FindPrev"));
    FindNext = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FindNext"));
    FindClose = safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FindClose"));
    FindBox = safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"FindBox"));
    ContentArea = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root->FindName(L"ContentArea"));
    RenderImage = safe_cast<::Windows::UI::Xaml::Controls::Image^>(__root->FindName(L"RenderImage"));
    // ---- 挂事件 ----
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"ContentArea"))->ManipulationCompleted += ref new ::Windows::UI::Xaml::Input::ManipulationCompletedEventHandler(this, &MainPage::OnImageManipCompleted);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"ContentArea"))->Tapped += ref new ::Windows::UI::Xaml::Input::TappedEventHandler(this, &MainPage::OnPageTapped);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"ContentArea"))->ManipulationDelta += ref new ::Windows::UI::Xaml::Input::ManipulationDeltaEventHandler(this, &MainPage::OnImageManipDelta);
    safe_cast<::Windows::UI::Xaml::FrameworkElement^>(__root->FindName(L"GpuPanel"))->Loaded += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnGpuPanelLoaded);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"ImeBox"))->KeyDown += ref new ::Windows::UI::Xaml::Input::KeyEventHandler(this, &MainPage::OnImeKeyDown);
    safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"ImeBox"))->TextChanged += ref new ::Windows::UI::Xaml::Controls::TextChangedEventHandler(this, &MainPage::OnImeTextChanged);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev1"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnScrollUp);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev2"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnScrollDown);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"FindBox"))->KeyDown += ref new ::Windows::UI::Xaml::Input::KeyEventHandler(this, &MainPage::OnFindKeyDown);
    safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"FindBox"))->TextChanged += ref new ::Windows::UI::Xaml::Controls::TextChangedEventHandler(this, &MainPage::OnFindChanged);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FindPrev"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnFindPrev);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FindNext"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnFindNext);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FindClose"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnFindClose);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"TabsBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnTabs);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"UrlBox"))->KeyDown += ref new ::Windows::UI::Xaml::Input::KeyEventHandler(this, &MainPage::OnUrlKeyDown);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"UrlBox"))->GotFocus += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnUrlGotFocus);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"UrlBox"))->LostFocus += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnUrlLostFocus);
    safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"UrlBox"))->TextChanged += ref new ::Windows::UI::Xaml::Controls::TextChangedEventHandler(this, &MainPage::OnUrlChanged);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"UrlActionBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnUrlAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"UrlClearBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnUrlClear);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"MenuBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnMenu);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"ActionMenu"))->Tapped += ref new ::Windows::UI::Xaml::Input::TappedEventHandler(this, &MainPage::OnActionScrimTap);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"_ev3"))->Tapped += ref new ::Windows::UI::Xaml::Input::TappedEventHandler(this, &MainPage::OnSheetTap);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"BackBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnBack);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FwdBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnForward);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev4"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev5"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev6"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev7"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev8"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev9"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev10"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev11"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev12"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev13"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev14"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev15"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev16"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"GpuBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnToggleGpu);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"UaBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnToggleUA);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev17"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnDrawerClose);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"TabFav"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnTabFav);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"TabHist"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnTabHist);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"TabDl"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnTabDl);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"ActionBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnPrimaryAction);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev18"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnSettingsBack);
    safe_cast<::Windows::UI::Xaml::Controls::Slider^>(__root->FindName(L"SetZoomSlider"))->ValueChanged += ref new ::Windows::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventHandler(this, &MainPage::OnZoomChanged);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev19"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnSettingsBtn);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev20"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnSettingsBtn);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev21"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnSettingsBtn);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev22"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnSettingsBtn);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev23"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnSettingsBtn);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev24"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnSettingsBtn);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev25"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnSettingsBtn);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev26"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnTabSwitcherDone);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev27"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnNewTab);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev28"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnOobeLang);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev29"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnOobeLang);
}

void MainPage::Connect(int, ::Platform::Object^) { }
::Windows::UI::Xaml::Markup::IComponentConnector^ MainPage::GetBindingConnector(int, ::Platform::Object^) { return nullptr; }
void MainPage::UnloadObject(::Windows::UI::Xaml::DependencyObject^) { }
void MainPage::DisconnectUnloadedObject(int) { }

}
