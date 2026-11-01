# 03 · 构建验证报告（真实编译结果，非静态推断）

> 角色：构建验证工程师
> 原则：**只观察、只记录，未修改任何源码 / vcxproj / shader**。唯一写入的是本文件 + MSBuild 自身产物。
> 环境时间：2026-10-30 01:20 ~ 01:25（Asia/Shanghai）

---

## 0. 结论速览（TL;DR）

| 问题 | 结论 |
|---|---|
| 仓库当前能编译过吗？ | **不能。** MSBuild `Debug\|x64` 退出码 **1**，共 **7 个 error**（2 个文件）。 |
| 错误集中在哪？ | `App.cpp` 1 条（C2660）+ `ProbeBorderFixupPass.cpp` 6 条（C2065）。其余 25 个 TU 全部编译成功（obj 时间戳可证）。 |
| 探针文件进工程了吗？ | **进了。** `ProbeGrid.cpp` / `ProbeUpdatePass.cpp` / `ProbeBorderFixupPass.cpp` 在 `ClCompile`，`ProbeGrid.h` / `ProbeUpdatePass.h` / `ProbeBorderFixupPass.h` / `ProbeTypes.h` / `ProbeDistancePass.h` 在 `ClInclude`，`ShadersProbe.hlsl` 在 `FxCompile`。 |
| HLSL 什么时候编译？ | **运行时**，`d3d12lite::CompileShaderSM66()` → `DxcCreateInstance` + `IDxcCompiler3::Compile`。工程里的 `FxCompile` 项**全部** `ExcludedFromBuild=true`，MSBuild 期不编译任何 shader（仓库里没有任何 `.cso`）。 |
| Shader 有语法错误吗？ | **有，11 条 error**，全在 `ShadersProbe.hlsl`。其余 5 个入口点（`ComputeSmokeCS` / `VSMain` / `GBufferPS` / `LightingVS` / `LightingPS`）全部 `exit 0`。 |
| 修好 C++ 就能跑吗？ | **不能。** C++ 编过之后，`App::init` 走到 `CompileShaderSM66(L"ShadersProbe.hlsl", L"ProbeUpdateCS", ...)` 会抛 `std::runtime_error`。**且因为 DXC 会编译整个 translation unit，即使只想用 `ProbeUpdateCS`，`ProbeDistanceCS` / `BorderFixupCS` 里写错的代码也会让 `ProbeUpdateCS` 编译失败。** |

一句话：**C++ 侧 7 个 error 挡住构建；HLSL 侧 11 个 error 挡住运行**，两者都必须修。

---

## 1. 工具链环境（实测）

```
vswhere  : C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe   (存在)
VS 安装路径: C:\Program Files\Microsoft Visual Studio\2022\Community
VS 版本   : 17.14.36915.13
MSBuild  : C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe
平台工具集: v143 / C++ 标准 stdcpplatest / ConformanceMode=true / WarningLevel=Level3
NuGet     : packages\Microsoft.Direct3D.DXC.1.9.2609.5 已还原（含 build\native\bin\x64\dxc.exe）
DXC 版本  : dxcompiler.dll: 1.9(5512-01b62ad4)(1.9.2609.5) - 1.9.2609.5 (01b62ad47)
```

探测命令与输出：

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
# C:\Program Files\Microsoft Visual Studio\2022\Community
# exit=0
```

---

## 2. 工程文件清单（以 `RenderingProgrameByHand.vcxproj` 为准）

### 2.1 `ClCompile`（真正参与编译的 27 个 TU）

`AccelerationStructure.cpp`, `App.cpp`, `BindlessDescriptorHeap.cpp`, `Camera.cpp`, `ComputeSmokePass.cpp`,
`CPUDescriptorAllocator.cpp`, `d3d12lite.cpp`, `dds_loader.cpp`, `DepthBuffer.cpp`, `GBufferPass.cpp`,
`Input.cpp`, `LightingPass.cpp`, `main.cpp`, `Mesh.cpp`, `MeshLibrary.cpp`, `ProbeBorderFixupPass.cpp`,
`ProbeGrid.cpp`, `ProbeUpdatePass.cpp`, `RenderContext.cpp`, `RenderGraph.cpp`, `RenderTexture.cpp`,
`RootSignature.cpp`, `Scene.cpp`, `SceneGpuData.cpp`, `Texture2D.cpp`, `TransientHeap.cpp`, `UploadHeap.cpp`

> 注意：`DepthBuffer.cpp` 被写在了 `<ClInclude>` 的 ItemGroup 里面（vcxproj 第 169–174 行），但元素名仍是
> `<ClCompile>`，MSBuild 只看元素名不看所属 ItemGroup，所以它**确实参与编译**（本次构建也生成了
> `DepthBuffer.obj`）。这是工程文件的排版瑕疵，不是功能缺失，本次不改。

磁盘上的 `.cpp` 与工程条目**一一对应，无遗漏、无多余**。

### 2.2 `ClInclude`（39 个）

`AccelerationStructure.h`, `App.h`, `BindlessDescriptorHeap.h`, `Camera.h`, `ComputeSmokePass.h`,
`CPUDescriptorAllocator.h`, `d3d.h`, `d3d12lite.h`, `DataTotal.h`, `dds_loader.h`, `DepthBuffer.h`,
`FrameConstants.h`, `GBufferPass.h`, `GpuMemoryBlock.h`, `Input.h`, `InstanceData.h`, `LightingPass.h`,
`LightingTypes.h`, `Mesh.h`, `MeshLibrary.h`, `Model.h`, `ObjectConstants.h`, `PipelineBuilder.h`,
`ProbeBorderFixupPass.h`, `ProbeDistancePass.h`, `ProbeGrid.h`, `ProbeTypes.h`, `ProbeUpdatePass.h`,
`RenderContext.h`, `pass.h`, `RenderGraph.h`, `RenderTexture.h`, `RootSignature.h`, `Scene.h`,
`SceneGpuData.h`, `SceneTypes.h`, `Texture2D.h`, `TransientHeap.h`, `UploadHeap.h`

### 2.3 `None`

| 文件 | 说明 |
|---|---|
| `packages.config` | NuGet 清单 |
| `Readme.md` | **磁盘上不存在**（工程外是 `..\README.md`），只是 IDE 排序项 |
| `ProbeDistanceCS.hlsl` | **0 字节空文件**，且四套配置 `ExcludedFromBuild=true`；没有任何代码引用它 |

### 2.4 `FxCompile`（5 个 HLSL，**全部 ExcludedFromBuild=true × 4 配置**）

`ShadersCommon.hlsl`, `ShadersCompute.hlsl`, `ShadersGBuffer.hlsl`, `ShadersLighting.hlsl`, `ShadersProbe.hlsl`

```xml
<FxCompile Include="ShadersProbe.hlsl">
  <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">true</ExcludedFromBuild>
  <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Release|Win32'">true</ExcludedFromBuild>
  <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">true</ExcludedFromBuild>
  <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Release|x64'">true</ExcludedFromBuild>
</FxCompile>
```

### 2.5 探针相关文件 → 是否真的参与编译？

| 文件 | 在工程里？ | 真的被处理？ | 证据 |
|---|---|---|---|
| `ProbeGrid.cpp` | ✅ `ClCompile` | ✅ 编译成功 | 本次生成 `ProbeGrid.obj`（194,622 B, 01:21:28） |
| `ProbeUpdatePass.cpp` | ✅ `ClCompile` | ✅ 编译成功 | 生成 `ProbeUpdatePass.obj`（208,941 B, 01:21:28） |
| `ProbeBorderFixupPass.cpp` | ✅ `ClCompile` | ❌ **编译失败** | 6 条 C2065；**未生成** `ProbeBorderFixupPass.obj` |
| `ProbeBorderFixupPass.h` | ✅ `ClInclude` | 被 `App.cpp` / `.cpp` 包含 | — |
| `ProbeGrid.h` / `ProbeTypes.h` / `ProbeUpdatePass.h` | ✅ `ClInclude` | 同上 | — |
| `ProbeDistancePass.h` | ✅ `ClInclude` | **14 字节空壳**（只有 `#pragma once`），没有任何 `.cpp` 包含它 | — |
| `ProbeDistanceCS.hlsl` | ✅ `None` | **0 字节**，无人引用（全仓库只出现在 vcxproj / filters） | grep 全仓仅 3 处引用：两处工程文件 + `ShadersProbe.hlsl:60` 的函数名 |
| `ShadersProbe.hlsl` | ✅ `FxCompile` | **构建期不编译**，运行时由 DXC 编译 | 见第 3 节 |

> 结论：DDGI 探针系统的 C++ 文件**确实进了构建**，不是"文件没加进工程"导致的假失败。
> `ProbeDistanceCS.hlsl` / `ProbeDistancePass.h` 是**半成品占位**：没有 C++ pass、没有入口点调用者，属于"未实现"，不应算作缺陷本身。

---

## 3. HLSL 是怎么进工程的（编译期 FxCompile 还是运行时 DXC？）

### 3.1 结论：**运行时 DXC（dxcompiler.dll + IDxcCompiler3），不是编译期**

`RenderingProgrameByHand\d3d12lite.cpp:277` 起：

```cpp
std::vector<uint8_t> d3d12lite::CompileShaderSM66(
    const std::wstring& filePath, const std::wstring& entryPoint, const std::wstring& targetProfile)
{
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    THROW_IF_FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
    THROW_IF_FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));

    uint32_t codePage = DXC_CP_UTF8;
    ComPtr<IDxcBlobEncoding> sourceBlob;
    THROW_IF_FAILED(utils->LoadFile(filePath.c_str(), &codePage, &sourceBlob));   // ← 直接读磁盘 .hlsl

    std::vector<LPCWSTR> arguments = {
        filePath.c_str(), L"-E", entryPoint.c_str(), L"-T", targetProfile.c_str(),
    #if defined(_DEBUG)
        L"-Zi", L"-Qembed_debug", L"-Od",     // Debug
    #else
        L"-O3",                               // Release
    #endif
    };

    ComPtr<IDxcIncludeHandler> includeHandler;
    THROW_IF_FAILED(utils->CreateDefaultIncludeHandler(&includeHandler));         // ← 默认 include handler
    ComPtr<IDxcResult> result;
    THROW_IF_FAILED(compiler->Compile(&sourceBuffer, arguments.data(),
        static_cast<uint32_t>(arguments.size()), includeHandler.Get(), IID_PPV_ARGS(&result)));
    ...
    if (FAILED(status)) throw std::runtime_error("DXC Shader Compilation Failed: " + ...);
}
```

调用点全部在 `App.cpp`（`App::init`）：

| 行 | 文件 | 入口点 | Profile |
|---|---|---|---|
| 118 | `ShadersGBuffer.hlsl` | `VSMain` | `vs_6_6` |
| 119 | `ShadersGBuffer.hlsl` | `GBufferPS` | `ps_6_6` |
| 120 | `ShadersLighting.hlsl` | `LightingVS` | `vs_6_6` |
| 121 | `ShadersLighting.hlsl` | `LightingPS` | `ps_6_6` |
| 158–159 | `ShadersCompute.hlsl` | `ComputeSmokeCS` | `cs_6_6` |
| 193–194 | `ShadersProbe.hlsl` | **`ProbeUpdateCS`** | `cs_6_6` |
| 206–207 | `ShadersProbe.hlsl` | **`BorderFixupCS`** | `cs_6_6` |

（`ProbeDistanceCS` **没有任何 C++ 调用点**。）

### 3.2 关键推论 1：构建成功 ≠ shader 正确

`FxCompile` 全部 `ExcludedFromBuild=true`，MSBuild 期一次也不会调用 `dxc`；仓库里也**没有任何 `.cso` 文件**。
所以 **Visual Studio 里"生成成功"对 shader 毫无保证**，shader 错误只会在**运行时**以异常形式爆出来。
本次验证之所以必须手工跑 `dxc.exe`，原因就在这里。

### 3.3 关键推论 2：`#include "ShadersCommon.hlsl"` 靠什么解析？

- 运行时**没有传 `-I`**（上面的 `arguments` 里没有 `-I`）。
- 靠 `IDxcUtils::CreateDefaultIncludeHandler()`：它先在**被包含文件所在目录**找，再在**当前工作目录**找。
- 因此能解析的前提是：`filePath` 是相对的（`L"ShadersProbe.hlsl"`），且**进程 CWD 就是 `RenderingProgrameByHand\`**（VS 调试时 CWD = `$(ProjectDir)`，成立）。
- 实测（见 5.4）：CWD 设到该目录、只传 `ShadersProbe.hlsl`、不给 `-I`，`#include` **能解析**（报的是语义错误而不是 "file not found"）。

> ⚠️ 部署隐患（非本次构建失败原因，但会影响运行）：NuGet 的 `.targets` 只把 `dxcompiler.dll / dxcompiler.pdb / dxil.dll / dxil.pdb` 作为 `Content` 复制到输出目录，**不复制任何 `.hlsl`**。
> 实测 `x64\Debug\` 下只有一份 **过期的 `Shaders.hlsl`（5,421 B，来自已被删除的 `RenderingProgrameByHand\Shaders.hlsl`，git 状态 `D`）**，没有 `ShadersProbe.hlsl` / `ShadersCommon.hlsl`。
> 即：直接双击 `x64\Debug\RenderingProgrameByHand.exe` 会因 `LoadFile` 失败（CWD 不对）而启动失败；从 VS 启动才正常。

---

## 4. 真实构建：MSBuild `Debug|x64`

### 4.1 完整命令行

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand.sln" `
  /t:Build /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
```

### 4.2 退出码

```
=== MSBUILD_EXITCODE=1 ===
```

（原始日志：`C:\Users\user\AppData\Local\Temp\ddgi-msbuild-debug.log`，UTF-16，56 行；已转存 UTF-8 便于阅读。）

### 4.3 完整输出（未删改）

```text
  AccelerationStructure.cpp
  App.cpp
  BindlessDescriptorHeap.cpp
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\App.cpp(214,25): error C2660: “ProbeBorderFixupPass::Init”: 函数不接受 4 个参数 [D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\RenderingProgrameByHand.vcxproj]
      D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ProbeBorderFixupPass.h(10,10):
      参见“ProbeBorderFixupPass::Init”的声明
      D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\App.cpp(214,25):
      尝试匹配参数列表“(T *, T *, ProbeGrid *, RenderGraph::ResourceId)”时
          with
          [
              T=ID3D12RootSignature
          ]
          and
          [
              T=ID3D12PipelineState
          ]

D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\BindlessDescriptorHeap.cpp(11,20): warning C4018: “<”: 有符号/无符号不匹配 [D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\RenderingProgrameByHand.vcxproj]
  Camera.cpp
  ComputeSmokePass.cpp
  CPUDescriptorAllocator.cpp
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\CPUDescriptorAllocator.cpp(18,20): warning C4018: “<”: 有符号/无符号不匹配 [...]
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\CPUDescriptorAllocator.cpp(21,20): warning C4018: “<”: 有符号/无符号不匹配 [...]
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\CPUDescriptorAllocator.cpp(30,20): warning C4018: “<”: 有符号/无符号不匹配 [...]
  d3d12lite.cpp
  dds_loader.cpp
  GBufferPass.cpp
  Input.cpp
  LightingPass.cpp
  main.cpp
  Mesh.cpp
  MeshLibrary.cpp
  ProbeBorderFixupPass.cpp
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ProbeBorderFixupPass.cpp(18,5): error C2065: “m_countX”: 未声明的标识符 [...]
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ProbeBorderFixupPass.cpp(18,24): error C2065: “m_countY”: 未声明的标识符 [...]
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ProbeBorderFixupPass.cpp(18,43): error C2065: “m_countZ”: 未声明的标识符 [...]
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ProbeBorderFixupPass.cpp(35,16): error C2065: “m_countX”: 未声明的标识符 [...]
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ProbeBorderFixupPass.cpp(36,16): error C2065: “m_countY”: 未声明的标识符 [...]
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ProbeBorderFixupPass.cpp(37,16): error C2065: “m_countZ”: 未声明的标识符 [...]
  ProbeGrid.cpp
  ProbeUpdatePass.cpp
  RenderContext.cpp
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\RenderContext.cpp(90,23): warning C4018: “<”: 有符号/无符号不匹配 [...]
  RenderGraph.cpp
  RenderTexture.cpp
  正在生成代码...
  正在编译...
  RootSignature.cpp
  Scene.cpp
  SceneGpuData.cpp
  Texture2D.cpp
  TransientHeap.cpp
  UploadHeap.cpp
  DepthBuffer.cpp
  正在生成代码...
```

### 4.4 错误清单（7 条，全部摘录）

| # | 文件(行,列) | 代码 | 原文 |
|---|---|---|---|
| 1 | `App.cpp(214,25)` | **C2660** | `“ProbeBorderFixupPass::Init”: 函数不接受 4 个参数`；实参类型 `(ID3D12RootSignature*, ID3D12PipelineState*, ProbeGrid*, RenderGraph::ResourceId)` |
| 2 | `ProbeBorderFixupPass.cpp(18,5)` | **C2065** | `“m_countX”: 未声明的标识符` |
| 3 | `ProbeBorderFixupPass.cpp(18,24)` | **C2065** | `“m_countY”: 未声明的标识符` |
| 4 | `ProbeBorderFixupPass.cpp(18,43)` | **C2065** | `“m_countZ”: 未声明的标识符` |
| 5 | `ProbeBorderFixupPass.cpp(35,16)` | **C2065** | `“m_countX”: 未声明的标识符` |
| 6 | `ProbeBorderFixupPass.cpp(36,16)` | **C2065** | `“m_countY”: 未声明的标识符` |
| 7 | `ProbeBorderFixupPass.cpp(37,16)` | **C2065** | `“m_countZ”: 未声明的标识符` |

**警告 4 条（非阻塞）**：`C4018` 有符号/无符号比较
—— `BindlessDescriptorHeap.cpp(11,20)`、`CPUDescriptorAllocator.cpp(18,20)(21,20)(30,20)`、`RenderContext.cpp(90,23)`。

### 4.5 错误根因（只做定位，不改代码）

`ProbeBorderFixupPass.h` 里的声明是 **11 个参数**：

```cpp
void Init(ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pso,
    RenderGraph::ResourceId atlasId,
    ID3D12Resource* atlasResource,   // UAV barrier 用
    uint32_t countX, uint32_t countY, uint32_t countZ,
    uint32_t uavIndex,
    uint32_t stride,
    uint32_t atlasWidth,
    uint32_t atlasHeight);
```

而 `App.cpp:214` 只传了 **4 个**：

```cpp
m_probeBorderFixupPass.Init(
    m_probeRootSignature.Get(), m_probeBorderPSO.Get(),
    &m_probeGrid, m_probeGridId);
```

两处签名处于**不同的重构阶段**（一处按 `ProbeGrid*` 取参数，一处按逐个标量传参）。
同时 `ProbeBorderFixupPass.h` 的私有成员里**只有** `m_rootSignature / m_pso / m_atlasResource / m_atlasId / m_uavIndex / m_stride / m_atlasWidth / m_atlasHeight`，**缺 `m_countX / m_countY / m_countZ`**，于是 `.cpp` 第 18、35–37 行写这三个成员时全部报 C2065。

> 这是**签名与成员两侧都没对齐**的典型半成品状态：单看 `App.cpp` 或单看 `ProbeBorderFixupPass.*` 都不觉得有问题，只有真正编译才会暴露。**修复时两边必须一起改**（选定一种 Init 契约：要么收 `ProbeGrid*` 自己取 count/stride/宽高，要么在头文件补齐 `m_countX/Y/Z` 并在调用点补 7 个实参）。

### 4.6 构建完整性证据

- **25/27 个 TU 编译成功**：`x64\Debug\` 下本次新生成 **25 个 `.obj`**（时间戳 2026/10/30 01:21:28–01:21:43），含 `ProbeGrid.obj`、`ProbeUpdatePass.obj`、`RenderGraph.obj`、`Scene.obj`、`SceneGpuData.obj`、`GBufferPass.obj`、`LightingPass.obj`、`ComputeSmokePass.obj`、`AccelerationStructure.obj`。
- **失败的 2 个 TU 没有 obj**：`ProbeBorderFixupPass.obj` 不存在；`App.obj` 仍是 **2026/10/22 0:43:30** 的旧产物（未被覆盖）。
- **链接阶段未执行**：`x64\Debug\RenderingProgrameByHand.exe` 时间戳仍是 **2026/10/22 0:43:31**（584,704 B）。
  → 因此**本次无法验证链接期错误**（LNK2019/LNK2001 之类）是否存在；C++ 编译通过后才可能暴露。
- 本次未改动任何源文件；`git status` 在验证前后一致（仍是作者原有的 10 个 `M` + 2 个 `D` + 33 个 `??`，本次仅新增 `docs/ddgi/`）。

---

## 5. Shader 语法检查：用工程自带 DXC 手工编译

### 5.1 工具

```
D:\C++files\RenderingProgrameByHand\packages\Microsoft.Direct3D.DXC.1.9.2609.5\build\native\bin\x64\dxc.exe
dxcompiler.dll: 1.9(5512-01b62ad4)(1.9.2609.5) - 1.9.2609.5 (01b62ad47)
```

### 5.2 命令行与退出码

```powershell
$dxc = "D:\C++files\RenderingProgrameByHand\packages\Microsoft.Direct3D.DXC.1.9.2609.5\build\native\bin\x64\dxc.exe"
$inc = "D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand"

& $dxc -I $inc -E ProbeUpdateCS   -T cs_6_6 -Fo $env:TEMP\ProbeUpdateCS.dxil   "$inc\ShadersProbe.hlsl"   # exit = -2147467259  ❌
& $dxc -I $inc -E ProbeDistanceCS -T cs_6_6 -Fo $env:TEMP\ProbeDistanceCS.dxil "$inc\ShadersProbe.hlsl"   # exit = -2147467259  ❌
& $dxc -I $inc -E BorderFixupCS   -T cs_6_6 -Fo $env:TEMP\BorderFixupCS.dxil   "$inc\ShadersProbe.hlsl"   # exit = -2147467259  ❌
```

退出码 `-2147467259` = `0x80004005`（E_FAIL），是 DXC 编译失败时的正常返回。

### 5.3 三个入口点的输出**逐字相同** —— 这一点非常重要

DXC 会**编译整个 translation unit**（所有函数都要过 parser + 语义检查），而不是只编译 `-E` 指定的那个函数。
所以 `ProbeDistanceCS` / `BorderFixupCS` 里的语法/类型错误会让**所有**入口点一起失败，`ProbeUpdateCS` 也不例外。

> **这就是"运行时为什么一定崩"的直接原因**：`App.cpp:193` 只想编译 `ProbeUpdateCS`，但 DXC 照样会去检查第 60 行以后那些半成品函数，然后返回 E_FAIL。

### 5.4 完整错误全文（11 条，三个入口点完全一致，此处去重展示一次）

```text
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:74:15: error: use of undeclared identifier 'probeIndex'
    uint px = probeIndex % counts.x;
              ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:75:16: error: use of undeclared identifier 'probeIndex'
    uint py = (probeIndex / counts.x) % counts.y;
               ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:76:15: error: use of undeclared identifier 'probeIndex'
    uint pz = probeIndex / (counts.x * counts.y);
              ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:78:31: error: no member named 'gridOriginSpacing' in 'ProbeUpdateParams'
    float3 probePos = g_Probe.gridOriginSpacing.xyz
                      ~~~~~~~ ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:79:52: error: no member named 'gridOriginSpacing' in 'ProbeUpdateParams'
                    + float3(px, py, pz) * g_Probe.gridOriginSpacing.w;
                                           ~~~~~~~ ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:89:16: error: redefinition of 'stride' with a different type: 'const uint' vs 'uint'
    const uint stride = 18; // 16 + 2*1 gutter
               ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:66:10: note: previous definition is here
    uint stride = g_Probe.borderTileStride; // 10 或 18
         ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:97:18: error: use of undeclared identifier 'distAtlas'
    float4 old = distAtlas[pixel];
                 ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:98:20: error: use of undeclared identifier 'frameIndex'
    float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;
                   ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:103:5: error: use of undeclared identifier 'distAtlas'
    distAtlas[pixel] = float4(mean, mean2, 0.0f, 1.0f);
    ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:112:28: error: no member named 'gridCountsAndFrame' in 'ProbeUpdateParams'
    uint3 counts = g_Probe.gridCountsAndFrame.xyz;
                   ~~~~~~~ ^
D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand\ShadersProbe.hlsl:131:64: error: no member named 'misc' in 'ProbeUpdateParams'
    RWTexture2D<float4> atlas = ResourceDescriptorHeap[g_Probe.misc.w];
                                                       ~~~~~~~ ^
```

（PowerShell 额外打印的 `NativeCommandError / FullyQualifiedErrorId` 只是把 stderr 当错误流重定向的噪声，不是 dxc 的报错内容。）

### 5.5 错误归属：按函数划分

| 行号 | 所属函数 | 错误数 | 性质 |
|---|---|---|---|
| 16–55 | `ProbeUpdateCS` | **0** | 唯一逻辑相对完整的入口点，本身没有编译错误 |
| 60–104 | `ProbeDistanceCS` | **9** | 半成品：引用了不存在的变量/字段 |
| 110–155 | `BorderFixupCS` | **2** | 引用了不存在的字段 |

逐条对应关系：

```
:74:15 / :75:16 / :76:15   ProbeDistanceCS 用了未声明的 probeIndex
                           （该函数签名里只有 id / groupId / gtid，没有 probeIndex；
                             也没有从 SV_GroupID 推导出来）
:78:31 / :79:52            ProbeUpdateParams 没有 gridOriginSpacing
                           （这是 LightingParams 的字段，见 ShadersCommon.hlsl:243；
                            ProbeUpdateParams 里对应的其实是 originX/Y/Z + spacing）
:89:16                     stride 在同一作用域重复定义（:66 已有 uint stride，
                           :89 又写 const uint stride = 18）
:97:18 / :103:5            distAtlas 未声明（:95 定义的是 atlas，:67 是 target）
:98:20                     frameIndex 未声明（g_Probe.frameIndex 才是）
:112:28                    ProbeUpdateParams 没有 gridCountsAndFrame
                           （应为 countX/countY/countZ）
:131:64                    ProbeUpdateParams 没有 misc（应为 borderTargetUav）
```

**结构体契约对照**（`ProbeTypes.h:5` ↔ `ShadersProbe.hlsl:3`，两者一致）：

```cpp
struct ProbeUpdateParams {          // HLSL 同名结构体字段完全对应，16 DWORD = 64 B
    float    originX, originY, originZ, spacing;   // 0
    uint32_t countX, countY, countZ, frameIndex;   // 16
    uint32_t probeCount, rayCount, objectIndex, irradianceUav;  // 32
    uint32_t distanceUav, borderTargetUav, borderTileStride, reserved0; // 48
};
static_assert(sizeof(ProbeUpdateParams) == 64);
```

> 也就是说：**C++/HLSL 的结构体本身是对齐的**（两处一致，`static_assert` 也过），
> 出问题的是 `ShadersProbe.hlsl` 里 `ProbeDistanceCS` / `BorderFixupCS` 的**函数体**：
> 它们引用了旧版/别处的字段名（`gridOriginSpacing` / `gridCountsAndFrame` / `misc`，那是 `LightingParams` 的命名）、以及未声明的局部变量。
> 单看 `ProbeUpdateParams` 无法发现问题，只有真正编译才暴露 —— 这正是本报告存在的价值。

### 5.6 对照实验：其它 shader 全部干净

同样的 `dxc -I <dir> -E ... -T ...` 方式（与运行时一致）：

| 文件 | 入口点 | Profile | 退出码 | 错误 |
|---|---|---|---|---|
| `ShadersCompute.hlsl` | `ComputeSmokeCS` | `cs_6_6` | **0** | 无 |
| `ShadersGBuffer.hlsl` | `VSMain` | `vs_6_6` | **0** | 无 |
| `ShadersGBuffer.hlsl` | `GBufferPS` | `ps_6_6` | **0** | 无 |
| `ShadersLighting.hlsl` | `LightingVS` | `vs_6_6` | **0** | 无 |
| `ShadersLighting.hlsl` | `LightingPS` | `ps_6_6` | **0** | 无 |

→ **shader 侧的问题 100% 集中在 `ShadersProbe.hlsl`**，`ShadersCommon.hlsl` 本身（`OctDecode` / `TraceRadiance` / `TraceHitDistance` / `RayQuery` 用法）是健康的。

### 5.7 补充：inclusion 路径实测

```powershell
# CWD = RenderingProgrameByHand\，不传 -I（等价于运行时的调用方式）
& $dxc -E ProbeUpdateCS -T cs_6_6 -Fo $env:TEMP\out2.dxil "ShadersProbe.hlsl"
# exit = -2147467259；报的是 :74 起的语义错误，不是 "ShadersCommon.hlsl not found"
# → 说明默认 include handler 能解析 #include "ShadersCommon.hlsl"
```

```powershell
# 等价 Debug 运行时参数（-Zi -Qembed_debug -Od）再跑一次
& $dxc -E ProbeUpdateCS -T cs_6_6 -Zi -Qembed_debug -Od -Fo out3.dxil "ShadersProbe.hlsl"
# exit = -2147467259，仍然是 11 条 error
```

→ Debug/Release 参数差异不影响结论。

### 5.8 关于"某个入口点本来就不该能编译"

任务要求如实区分"缺陷"与"本来就没实现"。逐条判定：

- **`ProbeUpdateCS`** — **是缺陷**。它是 `App.cpp:193` 真实调用的入口点，必须能编译；目前它本身没错，但被同文件里其它函数拖死。
- **`BorderFixupCS`** — **是缺陷**。`App.cpp:206` 真实调用它；同样必须能编译。
- **`ProbeDistanceCS`** — **属于"未实现"，但当前写法构成阻塞**。它**没有任何 C++ 调用点**（`ProbeDistancePass.h` 是 14 字节空 `#pragma once`，`ProbeDistanceCS.hlsl` 是 0 字节且未参与构建），说明"距离矩 pass"还在半成品阶段。
  但因为它被写进了**同一个 translation unit** `ShadersProbe.hlsl`，只要文件被 DXC 读入就会被检查 → **它的语法错误会连带 `ProbeUpdateCS` 一起失败**。
  → 处理方式二选一：**要么补完**（声明 `probeIndex`、改用正确字段名），**要么先注释/删除**整个函数体，**不能原样留着**。
- **`ProbeDistanceCS.hlsl`（0 字节）/ `ProbeDistancePass.h`（空壳）** — 属于"未实现"，**不影响构建**，不算缺陷。

---

## 6. 最终结论

### 6.1 构建可复现结论

1. `MSBuild RenderingProgrameByHand.sln /t:Build /p:Configuration=Debug /p:Platform=x64` → **失败，exit code = 1，7 个 error、4 个 warning**。
2. 错误 100% 落在 **DDGI 探针边界修复 pass** 上（`App.cpp` 调用点 + `ProbeBorderFixupPass.{h,cpp}` 成员/签名），**与网络、依赖、环境无关**：NuGet 包完整、DXC 可用、其余 25 个 TU 全部编译通过。
3. **链接期未执行**（compile 失败即中止），`.exe` 未更新，因此 **LNK 类错误仍未验证**。

### 6.2 修好 C++ 之后会怎样（重要）

C++ 编译通过后程序**仍然无法启动**：

- `App::init` 按顺序编译 5 个 shader 都成功（已实测 exit 0）；
- 到 `App.cpp:193` `CompileShaderSM66(L"ShadersProbe.hlsl", L"ProbeUpdateCS", L"cs_6_6")` 时，DXC 解析整个文件，命中 11 条错误 → `IDxcResult::GetStatus` 失败 → `throw std::runtime_error("DXC Shader Compilation Failed: ...")`；
- 异常从 `main` 冒出，窗口不会出现。

### 6.3 给修复者的最小行动清单（未执行，仅建议）

1. `ProbeBorderFixupPass`：统一 `Init` 契约（现在头文件 11 参 / 调用点 4 参），并补齐 `m_countX / m_countY / m_countZ` 三个成员。
2. `ShadersProbe.hlsl` 第 60–104 行 `ProbeDistanceCS`：声明 `probeIndex`（应由 `SV_GroupID` 推导），把 `gridOriginSpacing` 改成 `originX/Y/Z + spacing`，去掉重复的 `stride`，`distAtlas` → `atlas`，`frameIndex` → `g_Probe.frameIndex`。
3. `ShadersProbe.hlsl` 第 110–155 行 `BorderFixupCS`：`gridCountsAndFrame.xyz` → `uint3(countX, countY, countZ)`，`misc.w` → `borderTargetUav`。
4. 修完**必须手工跑一遍 `dxc.exe`**（本报告 5.2 的命令），因为 **VS 生成成功不代表 shader 能编译**。

---

## 7. 未验证 / 遗留

| 项 | 状态 |
|---|---|
| `Release\|x64` 构建 | **未执行**（任务只要求 Debug）。同一份源码 + 同一份 ClCompile 列表 + 仅有的差异是 `-O3` vs `-Zi -Od` 与 `NDEBUG`，**预期错误完全相同**，但本报告不把它当作已验证事实。 |
| 链接期错误（LNK2019/2001…） | **未验证**：编译阶段即失败，链接未启动。 |
| 修复后是否暴露新错误 | **无法预判**：不允许改源码，所以只能确认"当前状态下这 7 条是完整集合"（cl 未触发 100 条上限，两个失败 TU 的语义分析已跑到文件末尾）。 |
| 运行时行为（DDGI 是否真的工作、绑定是否正确） | **未验证**：程序当前构建不出来。 |
| `.hlsl` 未部署到输出目录 | 记录为**已知隐患**（见 3.3），不是本次失败原因。 |
| 4 条 `C4018` 警告 | 既有代码风格问题，与本次任务无关，**未修**。 |

### 复现本报告所需的一切

```powershell
# 1) 找 MSBuild
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
  -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
# 2) 构建
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand.sln" `
  /t:Build /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
# 3) shader 检查
$dxc = "D:\C++files\RenderingProgrameByHand\packages\Microsoft.Direct3D.DXC.1.9.2609.5\build\native\bin\x64\dxc.exe"
$inc = "D:\C++files\RenderingProgrameByHand\RenderingProgrameByHand"
& $dxc -I $inc -E ProbeUpdateCS -T cs_6_6 "$inc\ShadersProbe.hlsl"
```
