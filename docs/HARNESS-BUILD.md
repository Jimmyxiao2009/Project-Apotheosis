# Harness Build Guide

## Purpose

`harness/` is the ARM32 UWP host for the WebKit port. It contains the C++/CX UI, XAML pages, app manifest, packaged fonts and native runtime dependencies. The host cannot run on the x64 build machine; a successful appx build only verifies the build and package pipeline. Runtime validation requires a Lumia device.

The authoritative entry point is:

```powershell
pwsh -NoProfile -File E:\Apotheosis\port\build-harness.ps1
```

Do not invoke `MSBuild.exe` against `Harness.vcxproj` directly for normal development. The project intentionally uses two Visual Studio installations.

## Why Two Visual Studios

The current WebKit, driver, and harness link closure was built with the v143 ARM CRT. It must be linked by the VS18/v143 toolchain. The legacy C++/CX XAML compiler in VS18 fails during a clean official XAML Pass2 build with `WMC9999` on the installed SDKs.

VS2017's C++/CX compiler can produce the required official XAML outputs. Its v141 CRT cannot link the current v143 WebKit static libraries: attempting to use VS2017 for the whole harness produces unresolved v143 CRT symbols such as `__std_init_once_*` and `__std_fs_*`.

The supported pipeline therefore separates the jobs:

```text
VS2017 MSBuild + v141 ARM + SDK 17763
    official C++/CX XAML BuildCompile
    -> Generated Files\*.g.h / *.g.hpp / *.xbf / XamlTypeInfo*.g.cpp

VS18 MSBuild + MSVC v143 ARM 14.44.35207 + SDK 22621
    compile harness and generated XAML sources
    -> link existing WebKit/driver libraries
    -> package and sign ARM appx
```

`BuildCompile` is deliberate. It performs the temporary native compilation, WinMD creation, official XAML Pass2, and generated-source compilation without running the final v141 linker.

## Required Local Components

The defaults below match the current build machine. Paths may be overridden with script parameters where noted.

| Component | Required version / path | Role |
|---|---|---|
| Visual Studio 2017 | `15.9`, MSBuild and ARM C++/UWP workload | Official C++/CX XAML generation |
| VS2017 compiler | `14.16.27023\bin\Hostx64\arm\cl.exe` | Builds the temporary XAML metadata input |
| XAML SDK | `10.0.17763.0` | VS2017 XAML compiler task |
| Visual Studio 18 | MSBuild plus ARM C++/UWP workload | Final host build and appx packaging |
| VS18 compiler | `14.44.35207\bin\Hostx64\arm\cl.exe` | Links against current WebKit libraries |
| Final SDK | `10.0.22621.0` ARM libraries | v143 UWP build and packaging |
| vcpkg | `C:\vcpkg\installed\arm-uwp` | Runtime and static third-party libraries |
| ICU | `C:\icu-arm-uwp` | ICU static libraries and runtime DLLs |
| ANGLE | `angle\arm` under the repository root | EGL/GLES libraries and deployment DLLs |
| Driver | `port\WebCoreDriver-gpu.lib/.dll` | C ABI bridge to WebCore |

The script finds both VS installations through `vswhere.exe` and fails before compiling when a required compiler, SDK component, or XAML task is missing.

## Normal Workflow

### Change C++ only

Run the normal build. It regenerates official XAML too, which keeps generated files in sync and is the supported default.

```powershell
pwsh -NoProfile -File E:\Apotheosis\port\build-harness.ps1
```

### Change `App.xaml` or `MainPage.xaml`

Run the same command. Do not edit `Generated Files\*.g.*`; VS2017 owns those files.

```powershell
pwsh -NoProfile -File E:\Apotheosis\port\build-harness.ps1
```

### Verify a clean build

This cleans the v143 host outputs, runs VS2017 official XAML codegen from scratch, then builds and packages with v143.

```powershell
pwsh -NoProfile -File E:\Apotheosis\port\build-harness.ps1 -Clean
```

### Change `port/*.cpp`

Rebuild the driver first, then build the harness so the new DLL is packaged.

```powershell
pwsh -NoProfile -File E:\Apotheosis\port\link-driver-gpu.ps1
pwsh -NoProfile -File E:\Apotheosis\port\build-harness.ps1
```

### Change upstream WebKit source

Rebuild WebCore, relink the driver, then package the harness.

```powershell
. E:\Apotheosis\port\arm32-uwp-env.ps1
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C E:\Apotheosis\build-clang-gpu WebCore
pwsh -NoProfile -File E:\Apotheosis\port\link-driver-gpu.ps1
pwsh -NoProfile -File E:\Apotheosis\port\build-harness.ps1
```

## Build Parameters

`build-harness.ps1` has conservative defaults. Override a path or SDK only when the matching dependency is installed.

```powershell
pwsh -NoProfile -File E:\Apotheosis\port\build-harness.ps1 `
  -SdkVersion 10.0.22621.0 `
  -XamlSdkVersion 10.0.17763.0 `
  -VCToolsVersion 14.44.35207 `
  -VcpkgRoot C:\vcpkg `
  -IcuRoot C:\icu-arm-uwp
```

- `SdkVersion`: SDK used by the final VS18/v143 build. It needs ARM `WindowsApp.lib`.
- `XamlSdkVersion`: SDK used by VS2017's official XAML compiler. It needs `XamlCompiler\Microsoft.Windows.UI.Xaml.Build.Tasks.dll`.
- `VCToolsVersion`: v143 toolset used for the final host build. It must match the CRT expected by the built WebKit libraries.
- `VcpkgRoot` and `IcuRoot`: dependency roots used consistently for library lookup and appx deployment contents.
- `Clean`: clean the final v143 output before generating and rebuilding.

## XAML Modes

### Official mode

This is the default and should be used for normal development.

```powershell
pwsh -NoProfile -File E:\Apotheosis\port\build-harness.ps1 -XamlMode Official
```

The project uses these MSBuild properties internally:

- `ApotheosisUseOfficialXaml=true`: VS2017 sees `ApplicationDefinition` and `Page`, invokes its normal C++/CX pipeline, and writes `harness\Generated Files`.
- `ApotheosisXamlCodegen=true`: excludes generated implementation headers while VS2017 creates the temporary WinMD needed by Pass2.
- `ApotheosisConsumeOfficialXaml=true`: VS18 consumes the VS2017-generated headers, XAML type metadata sources, and XBF files without executing its own XAML compiler.

### Fallback mode

The fallback serializes `MainPage.xaml` for runtime `XamlReader::Load`, manually resolves named fields, and manually attaches XAML events. It exists only as a recovery option.

```powershell
pwsh -NoProfile -File E:\Apotheosis\port\build-harness.ps1 -XamlMode Fallback
```

Fallback inputs and outputs:

- Input: `harness\MainPage.xaml`
- Generator: `port\gen-xaml-codebehind.ps1`
- Output: `harness\xamlgen\MainPage.g.hpp`

Fallback does not validate normal C++/CX `LoadComponent`, XBF, or official XAML metadata behavior. Return to `Official` once the toolchain is available.

## Outputs

Official XAML generation writes transient files under:

```text
harness\Generated Files\
  App.g.h / App.g.hpp / App.xbf
  MainPage.g.h / MainPage.g.hpp / MainPage.xbf
  XamlTypeInfo.g.cpp / XamlTypeInfo.Impl.g.cpp / XamlTypeInfo.g.h
```

Final packaged output is normally:

```text
harness\AppPackages\Harness\Harness_<version>_ARM_Test\
  Harness_<version>_ARM.appx
  Harness_<version>_ARM.appxsym
```

The package version comes from `harness\Package.appxmanifest`. Update the deployment command's `-Ver` argument when that version changes.

## Deployment

The appx is ARM32 and cannot be run on the x64 build machine. Deploy it to a device with Device Portal enabled:

```powershell
pwsh -NoProfile -File E:\Apotheosis\tools\deploy-launch.ps1 `
  -Ip <device-ip> `
  -Ver <manifest-version>
```

Use `tools\Deploy-Robust.ps1` when the phone's Wi-Fi connection is unstable. GPU, navigation, input, persistence, and crash behavior require on-device validation.

## Troubleshooting

| Symptom | Cause | Action |
|---|---|---|
| `Visual Studio 2017 ... is required` | VS2017 or its MSBuild component is absent | Install the VS2017 ARM/UWP C++ workload, including MSBuild. |
| Missing `14.16.27023\...\arm\cl.exe` | VS2017 ARM compiler is not installed | Add ARM C++ build tools to VS2017. |
| Missing XAML build task | `XamlSdkVersion` does not have the XAML compiler | Install that SDK's UWP/XAML tools or select an installed SDK. |
| `WMC9999` from VS18 XAML targets | VS18 was allowed to compile XAML directly | Use `build-harness.ps1`; it routes official XAML generation through VS2017. |
| `__std_init_once_*` or `__std_fs_*` at link | VS2017/v141 is trying to link v143 WebKit libraries | Do not run a full VS2017 `Build`; VS2017 must stop at `BuildCompile`. |
| `App.g.hpp` or `MainPage.g.hpp` missing | Official codegen did not finish, or generated files were cleaned | Run `build-harness.ps1 -Clean`; do not manually copy generated files. |
| Linker cannot find `WebCoreDriver-gpu.lib` | Driver was not relinked or the expected GPU build output is absent | Run `port\link-driver-gpu.ps1`, then rebuild harness. |
| Appx lacks a runtime DLL | A new native dependency is missing from `Harness.vcxproj` deployment items | Add its DLL with `DeploymentContent=true`, then rebuild and inspect the appx. |

## Maintenance Rules

- Keep `port\WebCoreDriver.h` and `harness\WebCoreDriver.h` ABI-identical.
- Do not use VS2017 for final linking while WebKit is built with v143.
- Do not use VS18's XAML targets directly; the script's generated-source consumption mode is intentional.
- Keep repository, vcpkg, and generated-file paths ASCII.
- Treat `Generated Files`, `xamlgen`, `ARM`, `AppPackages`, `.obj`, `.lib`, `.dll`, and build logs as build artifacts unless explicitly tracked source changes require review.
