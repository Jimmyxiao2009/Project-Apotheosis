#pragma once
// 手搓生成 —— 勿手改;改 XAML 后重跑 port\gen-xaml-codebehind.ps1
#include "MainPage.g.h"
#include <string>

namespace Harness {

static ::Platform::String^ __MainPageXaml() {
    static const wchar_t* __c[] = {
        LR"APO(<Grid x:Name="RootGrid" Background="#FF161618" xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"><Grid.Resources>
        <!-- 统一深色调(贴近 LBrowser 暗色 chrome;不依赖 ThemeResource 解析,设备上确定一致) -->
        <SolidColorBrush x:Key="PageBg" Color="#FF161618" />
        <SolidColorBrush x:Key="Chrome" Color="#FF2A2A2C" />
        <SolidColorBrush x:Key="Surface" Color="#FF323234" />
        <SolidColorBrush x:Key="Inset" Color="#FF1B1B1D" />
        <SolidColorBrush x:Key="Sep" Color="#22FFFFFF" />
        <SolidColorBrush x:Key="TxtHi" Color="#FFF2F2F3" />
        <SolidColorBrush x:Key="TxtLo" Color="#FF9BA0A6" />
        <SolidColorBrush x:Key="Accent" Color="#FF3AA0FF" />
        <SolidColorBrush x:Key="Scrim" Color="#B3000000" />

        <!-- 底栏图标键:大触摸目标,透明底 -->
        <Style x:Key="NavBtn" TargetType="Button">
            <Setter Property="Background" Value="Transparent" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="BorderThickness" Value="0" />
            <Setter Property="FontFamily" Value="Segoe MDL2 Assets" />
            <Setter Property="FontSize" Value="19" />
            <Setter Property="Width" Value="50" />
            <Setter Property="Height" Value="54" />
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
            <Setter Property="Height" Value="42" />
            <Setter Property="Padding" Value="10,0" />
        </Style>
        <!-- 动作面板:顶部快捷图标列 -->
        <Style x:Key="QuickBtn" TargetType="Button">
            <Setter Property="Background" Value="Transparent" />
            <Setter Property="BorderThickness" Value="0" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="HorizontalAlignment" Value="Stretch" />
            <Setter Property="Padding" Value="0,10" />
        </Style>
        <!-- 动作面板:整行菜单项 -->
        <Style x:Key="MenuRow" TargetType="Button">
            <Setter Property="Background" Value="Transparent" />
            <Setter Property="BorderThickness" Value="0" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="HorizontalAlignment" Value="Stretch" />
            <Setter Property="HorizontalContentAlignment" Value="Left" />
            <Setter Property="FontSize" Value="17" />
            <Setter Property="Padding" Value="18,13" />
        </Style>
        <!-- 设置页的整行按钮 -->
        <Style x:Key="SetRow" TargetType="Button">
            <Setter Property="Background" Value="{StaticResource Surface}" />
            <Setter Property="Foreground" Value="{StaticResource TxtHi}" />
            <Setter Property="BorderThickness" Value="0" />
            <Setter Property="HorizontalAlignment" Value="Stretch" />
            <Setter Property="HorizontalContentAlignment" Value="Left" />
            <Setter Property="Padding" Value="14,12" />
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
            <Border Background="White">
                <!-- 自由滚动:内容区直接接 ManipulationDelta(单指拖→引擎滚动+惯性,捏合→Scale)。
                     点击走 Tapped;坐标用 GetPosition(ContentArea) 映回引擎像素(MapTapToEngine)。 -->
                <Grid x:Name="ContentArea" Background="White" ManipulationMode="TranslateX,TranslateY,TranslateInertia,Scale" IsTapEnabled="True">
                    <Image x:Name="RenderImage" Stretch="None" VerticalAlignment="Top" HorizontalAlignment="Left" />
                </Grid>
            </Border>

            <!-- GPU 直呈现面(SwapChainPanel);GPU 起来后承载 TextureMapper 合成输出。 -->
            <SwapChainPanel x:Name="GpuPanel" HorizontalAlignment="Stretch" VerticalAlignment="Stretch" IsHitTestVisible="False" Visibility="Collapsed" />

            <!-- 输入法捕获框:1×1 透明,聚焦唤起键盘;键入转发给引擎活会话。 -->
            <TextBox x:Name="ImeBox" Width="1" Height="1" Opacity="0" Margin="0" Padding="0" BorderThickness="0" MinWidth="0" MinHeight="0" HorizontalAlignment="Left" VerticalAlignment="Top" IsTabStop="True" />

            <!-- 顶部加载进度条 -->
            <ProgressBar x:Name="Progress" Height="2.5" VerticalAlignment="Top" Foreground="{StaticResource Accent}" Background="Transparent" IsIndeterminate="False" Visibility="Collapsed" />

            <!-- 悬浮翻页键(触发懒加载/看下方内容)。仅有会话时显示。 -->
            <StackPanel x:Name="ScrollFab" Orientation="Vertical" HorizontalAlignment="Right" VerticalAlignment="Bottom" Margin="0,0,14,16" Visibility="Collapsed">
                <Border Width="48")APO",
        LR"APO( Height="48" CornerRadius="24" Background="#CC2A2A2C" Margin="0,0,0,10">
                    <Button Width="48" Height="48" Padding="0" Background="Transparent" Foreground="#FFFFFFFF" FontSize="19" BorderThickness="0" Content="▲" x:Name="_ev1" />
                </Border>
                <Border Width="48" Height="48" CornerRadius="24" Background="#E63AA0FF">
                    <Button Width="48" Height="48" Padding="0" Background="Transparent" Foreground="#FFFFFFFF" FontSize="19" BorderThickness="0" Content="▼" x:Name="_ev2" />
                </Border>
            </StackPanel>

            <!-- 页内查找条(顶部浮条) -->
            <Border x:Name="FindBar" VerticalAlignment="Top" HorizontalAlignment="Stretch" Background="{StaticResource Chrome}" BorderBrush="{StaticResource Sep}" BorderThickness="0,0,0,1" Visibility="Collapsed">
                <Grid Margin="8,6">
                    <Grid.ColumnDefinitions>
                        <ColumnDefinition Width="*" />
                        <ColumnDefinition Width="Auto" />
                        <ColumnDefinition Width="Auto" />
                        <ColumnDefinition Width="Auto" />
                        <ColumnDefinition Width="Auto" />
                    </Grid.ColumnDefinitions>
                    <Border Grid.Column="0" Background="{StaticResource Inset}" CornerRadius="8">
                        <TextBox x:Name="FindBox" FontSize="15" Height="36" BorderThickness="0" Background="Transparent" Foreground="{StaticResource TxtHi}" VerticalContentAlignment="Center" Padding="10,0" PlaceholderText="页内查找" />
                    </Border>
                    <TextBlock x:Name="FindCount" Grid.Column="1" Text="" Foreground="{StaticResource TxtLo}" FontSize="13" VerticalAlignment="Center" Margin="10,0" />
                    <Button x:Name="FindPrev" Grid.Column="2" Style="{StaticResource IconBtn}" Content="▲" FontSize="13" />
                    <Button x:Name="FindNext" Grid.Column="3" Style="{StaticResource IconBtn}" Content="▼" FontSize="13" />
                    <Button x:Name="FindClose" Grid.Column="4" Style="{StaticResource IconBtn}" Content="✕" />
                </Grid>
            </Border>

            <!-- 地址栏建议下拉:锚定内容区底部 → 浮在底栏正上方。点项即导航。 -->
            <Border x:Name="SuggestPanel" VerticalAlignment="Bottom" HorizontalAlignment="Stretch" Background="#F22A2A2C" BorderBrush="{StaticResource Sep}" BorderThickness="0,1,0,0" Visibility="Collapsed">
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
            <Border Grid.Row="0" Background="{StaticResource Chrome}">
                <TextBlock x:Name="TitleText" Text="EdgeHTML Reborn" Foreground="{StaticResource TxtLo}" FontSize="11" HorizontalAlignment="Center" Margin="14,3,14,2" TextTrimming="CharacterEllipsis" />
            </Border>

            <!-- 导航栏:[标签数] | 🔒 地址 [Go/⟳/✕] | ⋯ -->
            <Grid Grid.Row="1" Height="54" Background="{StaticResource Chrome}" BorderBrush="{StaticResource Sep}" BorderThickness="0,1,0,0">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="*" />
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="Auto" />
                </Grid.ColumnDefinitions>

                <!-- 标签键:方框数字,点开标签切换器 -->
                <Button x:Name="TabsBtn" Grid.Column="0" Background="Transparent" BorderThickness="0" Width="50" Height="54" Padding="0">
                    <Border BorderBrush="{StaticResource TxtHi}" BorderThickness="2" CornerRadius="5" Width="24" Height="24">
                        <TextBlock x:Name="TabCountText" Text="1" Foreground="{StaticResource TxtHi}" FontSize="12" HorizontalAlignment="Center" VerticalAlignment="Center" />
                    </Border>
                </Button>

                <Rectangle Grid.Column="1" Fill="{StaticResource Sep}" Width="1" Height="22" VerticalAlignment="Center" />

                <!-- 地址胶囊:锁标 + 地址输入 + 上下文键(Go/刷新/停止)。三者统一 38 高、垂直居中。 -->
                <Border Grid.Column="2" Background="{StaticResource Inset}" CornerRadius="9" Margin="6,8" Padding="0" Height="38">
                    <Grid>
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="Auto" />
                            <ColumnDefinition Width="*" />
                            <ColumnDefinition Width="Auto" />
                        </Grid.ColumnDefinitions>
                        <!-- 锁标:固定居中槽,glyph 与地址文字同基线 -->
                        <TextBlock x:Name="LockIcon" Grid.Column="0" Text="" FontFamily="Segoe MDL2 Assets" FontSize="14" Foreground="{StaticResource TxtLo}" TextLineBounds="Tight" VerticalAlignment="Center" HorizontalAlignment="Center" Margin="10,0,2,0" />
                        <TextBox x:Name="UrlBox" Grid.Column="1" FontSize="15" Height="38" BorderThickness="0" Background="Transparent" Foreground="{StaticResource TxtHi}" VerticalAlignment="Center" VerticalContentAlignment="Center" Padding="6,0" InputScope="Url" Text="" PlaceholderText="搜索或输入网址" />
                        <Button x:Name="UrlActionBtn" Grid.Column="2" Background="Transparent" BorderThickness="0" Foreground="{StaticResource TxtHi}" Width="40" Height="38" Padding="0" FontSize="16" VerticalAlignment="Center" VerticalContentAlignment="Center" HorizontalContentAlignment="Center" Content="↻" />
 )APO",
        LR"APO(                   </Grid>
                </Border>

                <Rectangle Grid.Column="3" Fill="{StaticResource Sep}" Width="1" Height="22" VerticalAlignment="Center" />

                <!-- 菜单(More):弹出底部动作面板 -->
                <Button x:Name="MenuBtn" Grid.Column="4" Style="{StaticResource NavBtn}" Content="" />
            </Grid>
        </Grid>

        <!-- ============================================================================
             以下为全屏/底部浮层。Grid.RowSpan=2 覆盖内容+底栏。
             ============================================================================ -->

        <!-- ===== 动作面板(⋯ 弹出的底部 sheet)===== -->
        <Grid x:Name="ActionMenu" Grid.Row="0" Grid.RowSpan="2" Background="{StaticResource Scrim}" Visibility="Collapsed">
            <Border VerticalAlignment="Bottom" Background="{StaticResource Surface}" CornerRadius="16,16,0,0" x:Name="_ev3">
                <ScrollViewer VerticalScrollBarVisibility="Auto" MaxHeight="520">
                    <StackPanel Margin="0,10,0,16">
                        <!-- 抓手条 -->
                        <Border Width="40" Height="4" CornerRadius="2" Background="#55FFFFFF" Margin="0,0,0,12" />

                        <!-- 快捷:后退 / 前进 / 刷新 / 收藏 -->
                        <Grid Margin="6,2,6,6">
                            <Grid.ColumnDefinitions>
                                <ColumnDefinition Width="*" /><ColumnDefinition Width="*" />
                                <ColumnDefinition Width="*" /><ColumnDefinition Width="*" />
                            </Grid.ColumnDefinitions>
                            <Button x:Name="BackBtn" Grid.Column="0" Style="{StaticResource QuickBtn}" IsEnabled="False">
                                <StackPanel>
                                    <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="22" HorizontalAlignment="Center" Foreground="{StaticResource TxtHi}" />
                                    <TextBlock Text="后退" FontSize="12" HorizontalAlignment="Center" Foreground="{StaticResource TxtLo}" Margin="0,5,0,0" />
                                </StackPanel>
                            </Button>
                            <Button x:Name="FwdBtn" Grid.Column="1" Style="{StaticResource QuickBtn}" IsEnabled="False">
                                <StackPanel>
                                    <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="22" HorizontalAlignment="Center" Foreground="{StaticResource TxtHi}" />
                                    <TextBlock Text="前进" FontSize="12" HorizontalAlignment="Center" Foreground="{StaticResource TxtLo}" Margin="0,5,0,0" />
                                </StackPanel>
                            </Button>
                            <Button Grid.Column="2" Tag="reload" Style="{StaticResource QuickBtn}" x:Name="_ev4">
                                <StackPanel>
                                    <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="22" HorizontalAlignment="Center" Foreground="{StaticResource TxtHi}" />
                                    <TextBlock Text="刷新" FontSize="12" HorizontalAlignment="Center" Foreground="{StaticResource TxtLo}" Margin="0,5,0,0" />
                                </StackPanel>
                            </Button>
                            <Button Grid.Column="3" Tag="bookmark" Style="{StaticResource QuickBtn}" x:Name="_ev5">
                                <StackPanel>
                                    <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="22" HorizontalAlignment="Center" Foreground="#FFFFD24A" />
                                    <TextBlock x:Name="ActFavLabel" Text="收藏" FontSize="12" HorizontalAlignment="Center" Foreground="{StaticResource TxtLo}" Margin="0,5,0,0" />
                                </StackPanel>
                            </Button>
                        </Grid>

                        <Border Height="1" Background="{StaticResource Sep}" Margin="14,4" />

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
            )APO",
        LR"APO(                </StackPanel>
                        </Button>
                        <Button Tag="share" Style="{StaticResource MenuRow}" x:Name="_ev10">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="分享" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                        <Button Tag="copylink" Style="{StaticResource MenuRow}" x:Name="_ev11">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="复制链接" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>
                        <Button Tag="download" Style="{StaticResource MenuRow}" x:Name="_ev12">
                            <StackPanel Orientation="Horizontal">
                                <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="17" Width="34" VerticalAlignment="Center" Foreground="{StaticResource TxtLo}" />
                                <TextBlock Text="下载此页" VerticalAlignment="Center" />
                            </StackPanel>
                        </Button>

                        <Border Height="1" Background="{StaticResource Sep}" Margin="14,4" />

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

                        <Border Height="1" Background="{StaticResource Sep}" Margin="14,4" />

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

            <Grid Grid.Row="0" Background="{StaticResource Chrome}">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*" />
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="Auto" />
                </Grid.ColumnDefinitions>
                <TextBlock Grid.Column="0" Text="菜单" Foreground="{StaticResource TxtHi}" FontSize="22" FontWeight="SemiBold" Margin="16,0" VerticalAlignment="Center" />
                <Button x:Name="GpuBtn" Grid.Column="1" Style="{StaticResource TabBtn}" Foreground="{StaticResource TxtLo}" Content="🖥 GPU" VerticalAlignment="Center" />
                <Button x:Name="UaBtn" Grid.Column="2" Style="{StaticResource TabBtn}" Foreground="{StaticResource Accent}" Content="📱 手机UA" VerticalAlignment="Center" />
                <Button Grid.Column="3" Style="{StaticResource IconBtn}" Content="✕" x:Name="_ev17" />
            </Grid>

            <Grid Grid.Row="1" Background="{StaticResource Surface}">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="*" />
                    <ColumnDefinition Width="*" />
                    <ColumnDefinition Width="*" />
                    <ColumnDefinition Width="Auto" />
                </Grid.ColumnDefinitions>
                <Button x:Name="TabFav" Grid.Column="0" Style="{StaticResource TabBtn}" Content="★ 收藏" />
                <Button x:Name="TabHist" Grid.Column="1" Style="{StaticResource TabBtn}" Content="🕑 历史" />
                <Button x:Name="TabDl" Grid.Column="2" Style="{StaticResource TabBtn}" Content="↓ 下载" />
                <Button x:Nam)APO",
        LR"APO(e="ActionBtn" Grid.Column="3" Style="{StaticResource TabBtn}" Foreground="{StaticResource Accent}" Content="★ 收藏此页" />
            </Grid>

            <ScrollViewer Grid.Row="2" VerticalScrollBarVisibility="Auto">
                <StackPanel x:Name="DrawerList" Margin="8" />
            </ScrollViewer>
        </Grid>

        <!-- ===== 设置页(全屏)===== -->
        <Grid x:Name="SettingsPage" Grid.Row="0" Grid.RowSpan="2" Background="{StaticResource PageBg}" Visibility="Collapsed">
            <Grid.RowDefinitions>
                <RowDefinition Height="Auto" />
                <RowDefinition Height="*" />
            </Grid.RowDefinitions>
            <Grid Grid.Row="0" Background="{StaticResource Chrome}" Padding="3,6">
                <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="Auto" />
                    <ColumnDefinition Width="*" />
                </Grid.ColumnDefinitions>
                <Button Grid.Column="0" Style="{StaticResource IconBtn}" FontFamily="Segoe MDL2 Assets" Content="" x:Name="_ev18" />
                <TextBlock Grid.Column="1" Text="设置" Foreground="{StaticResource TxtHi}" FontSize="20" FontWeight="SemiBold" VerticalAlignment="Center" Margin="6,0" />
            </Grid>
            <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto">
                <StackPanel Margin="18,12">
                    <TextBlock Text="默认搜索引擎" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,8,0,4" />
                    <ComboBox x:Name="SetSearchCombo" HorizontalAlignment="Stretch">
                        <ComboBoxItem Content="Bing" />
                        <ComboBoxItem Content="Google" />
                        <ComboBoxItem Content="DuckDuckGo" />
                        <ComboBoxItem Content="百度" />
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

                    <TextBlock Text="GPU" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,18,0,4" />
                    <ToggleSwitch x:Name="SetGpuSwitch" Header="默认启用 GPU 渲染(加载首个网页后自动开)" Foreground="{StaticResource TxtHi}" Margin="0,0,0,6" />
                    <Button Tag="gpu" Style="{StaticResource SetRow}" Content="立即开启 GPU 合成(重启回软件)" x:Name="_ev19" />

                    <TextBlock Text="清除数据" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,18,0,4" />
                    <Button Tag="clearhist" Style="{StaticResource SetRow}" Content="清除历史记录" Margin="0,0,0,6" x:Name="_ev20" />
                    <Button Tag="clearfav" Style="{StaticResource SetRow}" Content="清除全部收藏" Margin="0,0,0,6" x:Name="_ev21" />
                    <Button Tag="cleardl" Style="{StaticResource SetRow}" Content="清除下载记录" x:Name="_ev22" />

                    <TextBlock Text="诊断" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,18,0,4" />
                    <Button Tag="export" Style="{StaticResource SetRow}" Content="导出调试日志 / 崩溃 dump" x:Name="_ev23" />

                    <TextBlock Text="关于 / 更新" Foreground="{StaticResource TxtLo}" FontSize="13" Margin="0,18,0,4" />
                    <TextBlock x:Name="VersionText" Text="版本 —" Foreground="{StaticResource TxtHi}" FontSize="15" Margin="0,0,0,8" />
                    <Button Tag="checkupdate" Style="{StaticResource SetRow}" Content="检查更新(GitHub Releases)" x:Name="_ev24" />

                    <TextBlock Text="EdgeHTML Reborn / Apotheosis — WebKit (WebCore) 2.52.4 — ARM32 UWP" Foreground="#FF80868B" FontSize="12" TextWrapping="Wrap" Margin="0,22,0,24" />
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
            <Grid Grid.Row="0" Background="{StaticResource Chrome}" Padding="6,6">
                <Grid.ColumnDefinitions><ColumnDefinition Width="*" /><ColumnDefinition Width="Auto" /></Grid.ColumnDefinitions>
                <TextBlock x:Name="TabSwitcherTitle" Grid.Column="0" Text="标签" Foreground="{StaticResource TxtHi}" FontSize="20" FontWeight="SemiBold" VerticalAlignment="Center" Margin="10,0" />
                <Button Grid.Column="1" Sty)APO",
        LR"APO(le="{StaticResource TabBtn}" Foreground="{StaticResource Accent}" Content="完成" x:Name="_ev25" />
            </Grid>
            <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto">
                <StackPanel x:Name="TabList" Margin="10,10" />
            </ScrollViewer>
            <Button Grid.Row="2" HorizontalAlignment="Stretch" HorizontalContentAlignment="Center" Background="{StaticResource Chrome}" Foreground="{StaticResource Accent}" Padding="0,15" x:Name="_ev26">
                <StackPanel Orientation="Horizontal">
                    <TextBlock Text="" FontFamily="Segoe MDL2 Assets" FontSize="15" VerticalAlignment="Center" Foreground="{StaticResource Accent}" />
                    <TextBlock Text="新建标签页" Margin="10,0,0,0" VerticalAlignment="Center" Foreground="{StaticResource Accent}" />
                </StackPanel>
            </Button>
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
    ActionMenu = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root->FindName(L"ActionMenu"));
    Drawer = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root->FindName(L"Drawer"));
    SettingsPage = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root->FindName(L"SettingsPage"));
    TabSwitcher = safe_cast<::Windows::UI::Xaml::Controls::Grid^>(__root->FindName(L"TabSwitcher"));
    TabList = safe_cast<::Windows::UI::Xaml::Controls::StackPanel^>(__root->FindName(L"TabList"));
    TabSwitcherTitle = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"TabSwitcherTitle"));
    SetSearchCombo = safe_cast<::Windows::UI::Xaml::Controls::ComboBox^>(__root->FindName(L"SetSearchCombo"));
    SetHomeBox = safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"SetHomeBox"));
    SetUaSwitch = safe_cast<::Windows::UI::Xaml::Controls::ToggleSwitch^>(__root->FindName(L"SetUaSwitch"));
    SetUaCustomBox = safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"SetUaCustomBox"));
    SetZoomSlider = safe_cast<::Windows::UI::Xaml::Controls::Slider^>(__root->FindName(L"SetZoomSlider"));
    SetTabModeSwitch = safe_cast<::Windows::UI::Xaml::Controls::ToggleSwitch^>(__root->FindName(L"SetTabModeSwitch"));
    SetGpuSwitch = safe_cast<::Windows::UI::Xaml::Controls::ToggleSwitch^>(__root->FindName(L"SetGpuSwitch"));
    VersionText = safe_cast<::Windows::UI::Xaml::Controls::TextBlock^>(__root->FindName(L"VersionText"));
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
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"ContentArea"))->ManipulationDelta += ref new ::Windows::UI::Xaml::Input::ManipulationDeltaEventHandler(this, &MainPage::OnImageManipDelta);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"ContentArea"))->Tapped += ref new ::Windows::UI::Xaml::Input::TappedEventHandler(this, &MainPage::OnPageTapped);
    safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"ImeBox"))->TextChanged += ref new ::Windows::UI::Xaml::Controls::TextChangedEventHandler(this, &MainPage::OnImeTextChanged);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"ImeBox"))->KeyDown += ref new ::Windows::UI::Xaml::Input::KeyEventHandler(this, &MainPage::OnImeKeyDown);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev1"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnScrollUp);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev2"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnScrollDown);
    safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"FindBox"))->TextChanged += ref new ::Windows::UI::Xaml::Controls::TextChangedEventHandler(this, &MainPage::OnFindChanged);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"FindBox"))->KeyDown += ref new ::Windows::UI::Xaml::Input::KeyEventHandler(this, &MainPage::OnFindKeyDown);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FindPrev"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnFindPrev);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FindNext"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnFindNext);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"FindClose"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnFindClose);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"TabsBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnTabs);
    safe_cast<::Windows::UI::Xaml::Controls::TextBox^>(__root->FindName(L"UrlBox"))->TextChanged += ref new ::Windows::UI::Xaml::Controls::TextChangedEventHandler(this, &MainPage::OnUrlChanged);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"UrlBox"))->KeyDown += ref new ::Windows::UI::Xaml::Input::KeyEventHandler(this, &MainPage::OnUrlKeyDown);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"UrlBox"))->GotFocus += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnUrlGotFocus);
    safe_cast<::Windows::UI::Xaml::UIElement^>(__root->FindName(L"UrlBox"))->LostFocus += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnUrlLostFocus);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"UrlActionBtn"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnUrlAction);
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
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev25"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnTabSwitcherDone);
    safe_cast<::Windows::UI::Xaml::Controls::Button^>(__root->FindName(L"_ev26"))->Click += ref new ::Windows::UI::Xaml::RoutedEventHandler(this, &MainPage::OnNewTab);
}

void MainPage::Connect(int, ::Platform::Object^) { }
::Windows::UI::Xaml::Markup::IComponentConnector^ MainPage::GetBindingConnector(int, ::Platform::Object^) { return nullptr; }
void MainPage::UnloadObject(::Windows::UI::Xaml::DependencyObject^) { }
void MainPage::DisconnectUnloadedObject(int) { }

}
