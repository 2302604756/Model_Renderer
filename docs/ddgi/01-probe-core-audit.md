# 01 · DDGI 探针核心侧审计报告

- 审计对象：`D:\C++files\RenderingProgrameByHand`（D3D12 + DXR，VS2022 / v143 / C++latest / SM 6.6）
- 审计性质：**只读**。未修改任何 `.cpp` / `.h` / `.hlsl` / `.vcxproj`
- 审计日期：2026-10-30
- 审计范围：探针核心侧 13 个文件（见 §2），并向外追踪到 `App.cpp`、`ShadersCommon.hlsl`、`ShadersLighting.hlsl`、`RenderGraph.*`、`RootSignature.*`、`LightingTypes.h`、`d3d12lite.*`

---

## 0. 结论速览（先看这 5 条）

| # | 结论 | 严重度 |
|---|---|---|
| 1 | **工程目前编译不过**。MSVC 侧 7 条错误、2 个文件；全部集中在探针改造未收尾处。其余 25 个 `.cpp` 单独编译全部通过。 | 阻断 |
| 2 | **着色器侧 11 条 DXC 错误**，且因为三个 CS 入口共用同一编译单元 `ShadersProbe.hlsl`，**`ProbeUpdateCS` 也一起编译失败**。`App::init` 会在 `App.cpp:193` 抛 `std::runtime_error`，程序起不来。 | 阻断 |
| 3 | `ProbeTypes.h` 的 CPU 结构体与 `ShadersProbe.hlsl` 的 `ProbeUpdateParams` **声明层 100% 一致**（64 B / 16 DWORD，逐字段同名同偏移）。真正的错位不在结构体，而在**着色器函数体引用了另一套早已不存在的字段名**（`gridOriginSpacing` / `gridCountsAndFrame` / `misc`）。任务描述里"CPU vs HLSL 字段不一致"这个假设需要修正。 | 认知纠偏 |
| 4 | `ProbeUpdateCS` 的线程映射与 `Dispatch` **完全匹配**（`SV_DispatchThreadID.x / 8` == `SV_GroupID.x`），这一条不是 bug。真正的映射错误在**死代码** `ProbeDistanceCS`：它把 `SV_DispatchThreadID` 当图集坐标、又同时把 `SV_GroupThreadID` 当 tile 内坐标，两套模型互相矛盾。 | 认知纠偏 |
| 5 | **最严重的算法缺陷：DDGI 根本没有接入着色路径。** `LightingPS` 的最终着色（`ShadersLighting.hlsl:116-135`）完全不用探针，仍然逐像素打 2 条余弦半球光追射线的 `TraceGI`。探针图集只在 debug 视图 8/9 里被最近邻 `.Load` 读出来"看一眼"。即使把上面所有编译错误修完，**画面也不会有任何 DDGI 效果**。 | 设计级 |

---

## 1. 审计方法与可复现命令

本报告不靠"读代码推断"，所有编译诊断均为**实测**。环境与命令如下。

### 1.1 环境

| 项目 | 实际值 |
|---|---|
| 编译器 | `cl.exe` 19.44.35222（MSVC 14.44.35207），HostX64/x64 |
| 着色器编译器（工程运行时实际使用的） | `dxcompiler.dll` **1.8.2502.11**（工程根目录自带，见 `RenderingProgrameByHand/dxcompiler.dll` 版本信息） |
| 交叉验证用 dxc | `Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe`（同为 1.8.2502.11）；另用 NuGet `Microsoft.Direct3D.DXC.1.9.2609.5` 的 dxc 1.9 复核 |
| 目标 | `/std:c++latest /EHsc /W3 /D_DEBUG /MDd /DUNICODE /D_UNICODE`（对齐 `Debug|x64`，见 `.vcxproj:34 CharacterSet=Unicode`、`:106-108`） |

### 1.2 命令

```bat
:: C++ 侧：逐个源码文件、输出到 %TEMP%，不触碰 x64/ 与源码目录
call "...\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /c /std:c++latest /EHsc /W3 /D_DEBUG /MDd /DUNICODE /D_UNICODE /I<RenderingProgrameByHand> /Fo"%TEMP%\\" ProbeBorderFixupPass.cpp
cl /nologo /c /std:c++latest /EHsc /W3 /D_DEBUG /MDd /DUNICODE /D_UNICODE /I<RenderingProgrameByHand> /Fo"%TEMP%\\" App.cpp

:: HLSL 侧：完全复刻 d3d12lite::CompileShaderSM66 的参数
dxc -E ProbeUpdateCS  -T cs_6_6 ShadersProbe.hlsl -Fo out.dxil
dxc -E BorderFixupCS  -T cs_6_6 ShadersProbe.hlsl -Fo out.dxil
dxc -E ProbeDistanceCS -T cs_6_6 ShadersProbe.hlsl -Fo out.dxil
```

### 1.3 一个关键的、容易被忽略的事实：**HLSL 不参与 MSBuild**

`RenderingProgrameByHand.vcxproj` 里所有 `.hlsl` 都是 `ExcludedFromBuild=true`：

| 位置 | 内容 |
|---|---|
| `.vcxproj:240-245` | `ShadersProbe.hlsl` 是 `<FxCompile>`，但 **Debug/Release × Win32/x64 四个配置全部 `ExcludedFromBuild=true`** |
| `.vcxproj:207-212` | `ProbeDistanceCS.hlsl` 是 `<None>`，且四个配置亦全部 `ExcludedFromBuild=true` |

真正的编译发生在**运行时**：`App.cpp:193` / `App.cpp:206` 调 `d3d12lite::CompileShaderSM66()`，它内部 `DxcCreateInstance` + `compiler->Compile()`（`d3d12lite.cpp:282-320`），失败时 **`throw std::runtime_error`**（`d3d12lite.cpp:331-334`）。

> 推论：**着色器错误不会在"生成解决方案"时暴露**，只会在启动瞬间以异常形式炸出来。这正是这套代码"看起来写完了、其实跑不起来"的机制性原因。审计时如果只看 MSBuild 是否通过，会漏掉全部 11 条 HLSL 错误。

---

## 2. 文件现状一览

| 文件 | 行数 | 做了什么（一句话） | 是否参与编译 |
|---|---|---|---|
| `ProbeGrid.h` | 49 | 探针网格 CPU 描述 + 持有两张 octahedral atlas（辐照度 10-stride、距离矩 18-stride）的 `RenderTexture` | 是（`.vcxproj:190` ClInclude） |
| `ProbeGrid.cpp` | 14 | 调 `RenderTexture::InitUAV` 建两张 `R16G16B16A16_FLOAT` atlas | 是（`.vcxproj:146`） |
| `ProbeTypes.h` | 24 | 定义 CPU 侧 `ProbeUpdateParams`（64 B）+ `static_assert` | 是（`.vcxproj:191`） |
| `ProbeUpdatePass.h` | 29 | 探针更新 pass 声明 | 是（`.vcxproj:192`） |
| `ProbeUpdatePass.cpp` | 65 | 填 `ProbeUpdateParams` → 绑 5 个根参数 → `Dispatch(probeCount,1,1)` → UAV barrier | 是（`.vcxproj:147`） |
| `ProbeBorderFixupPass.h` | 33 | gutter 修补 pass 声明（`Init` 收 **9** 个参数） | 是（`.vcxproj:188`） |
| `ProbeBorderFixupPass.cpp` | 50 | 填 counts + `borderTargetUav`/`borderTileStride` → 覆盖全图集 Dispatch → UAV barrier | 是（`.vcxproj:145`） |
| `ProbeDistancePass.h` | 1 | **空壳**，只有 `#pragma once`，无类、无实现、无 `.cpp` | 是（`.vcxproj:189` ClInclude，但无人 include，无副作用） |
| `ProbeDistanceCS.hlsl` | **0 字节** | 占位空文件 | **否**（`.vcxproj:207` `<None>` + 全配置 ExcludedFromBuild）；也从未被 `LoadFile` |
| `ShadersProbe.hlsl` | 155 | 三个 CS 入口：`ProbeUpdateCS`(:17)、`ProbeDistanceCS`(:60)、`BorderFixupCS`(:110) | **否（不参与 FxCompile）**，改由 `App.cpp` 运行时 DXC 编译 |
| `pass.h` | 11 | `RenderPass` 抽象基类（`GetName`/`Declare`/`Execute`） | 是 |
| `RenderTexture.h` | 43 | 资源 + RTV + bindless SRV + UAV 索引 + 当前状态 | 是（`.vcxproj:150`） |
| `RenderTexture.cpp` | 115 | `Init` / `InitUAV` / `TransitionTo` 实现 | 是 |

### 2.1 `RenderTexture` 的接口核查（任务点：确认 `InitUAV` / `GetUAVBindlessIndex` / 状态字段是否存在）

**全部存在，`ProbeGrid` 的调用是合法的**，不构成编译错误：

| 需要的符号 | 存在？ | 位置 |
|---|---|---|
| `InitUAV(device, bindless, w, h, format)` | ✅ | `RenderTexture.h:22-25`，实现 `RenderTexture.cpp:72-115` |
| `GetUAVBindlessIndex()` | ✅（内联） | `RenderTexture.h:26` → 返回 `m_uavBindlessIndex`(`:42`) |
| `GetBindlessIndex()`（SRV） | ✅ | `RenderTexture.h:32` → `m_bindlessIndex`(`:39`) |
| 状态字段 `m_state` | ✅ | `RenderTexture.h:40`，初始 `D3D12_RESOURCE_STATE_COMMON` |
| `GetState()` | ✅ | `RenderTexture.h:34` |
| `TransitionTo()` | ✅ | `RenderTexture.h:29` / `.cpp:56-68` |
| `InitUAV` 是否建了 UAV 描述符 | ✅ | `RenderTexture.cpp:108-114`（`D3D12_UAV_DIMENSION_TEXTURE2D`，`MipSlice=0`） |
| `InitUAV` 是否建了 SRV 描述符 | ✅ | `RenderTexture.cpp:98-105` |
| 初始资源状态 | ✅ | `RenderTexture.cpp:93-95`：`D3D12_RESOURCE_STATE_UNORDERED_ACCESS`，`m_state` 同步 |

> 注意一处**可用但危险**的细节：`RenderTexture::m_state` 与 `RenderGraph` 的资源状态是**两套独立账本**。`RenderGraph::Execute` 只改自己的 `m_resources[i].state`（`RenderGraph.cpp:102-108`），**不回写** `RenderTexture::m_state`。探针图集被 `LightingPass` 声明为 `PIXEL_SHADER_RESOURCE`（`LightingPass.cpp:49`）后，`m_atlas.m_state` 仍停留在 `UNORDERED_ACCESS`。当前没人对探针 atlas 调 `TransitionTo`，所以没炸；一旦有人调，`StateBefore` 就是错的。详见 §7.1。

---

## 3. 精确编译错误清单

### 3.1 构建期错误（MSVC，实测原文）

**共 7 条错误，分布在 2 个文件。** 以下 `错误原文` 为 `cl.exe` 的真实输出（非推断）。

| # | file:line | 错误码 | 错误原文（cl 实测） | 根因 |
|---|---|---|---|---|
| C1 | `ProbeBorderFixupPass.cpp:18` | C2065 | `“m_countX”: 未声明的标识符` | `ProbeBorderFixupPass.h:24-32` 的私有成员区**没有** `m_countX` |
| C2 | `ProbeBorderFixupPass.cpp:18` | C2065 | `“m_countY”: 未声明的标识符` | 同上 |
| C3 | `ProbeBorderFixupPass.cpp:18` | C2065 | `“m_countZ”: 未声明的标识符` | 同上 |
| C4 | `ProbeBorderFixupPass.cpp:35` | C2065 | `“m_countX”: 未声明的标识符` | 同上 |
| C5 | `ProbeBorderFixupPass.cpp:36` | C2065 | `“m_countY”: 未声明的标识符` | 同上 |
| C6 | `ProbeBorderFixupPass.cpp:37` | C2065 | `“m_countZ”: 未声明的标识符` | 同上 |
| C7 | `App.cpp:214` | **C2660** | `“ProbeBorderFixupPass::Init”: 函数不接受 4 个参数`<br>`ProbeBorderFixupPass.h(10): note: 参见“ProbeBorderFixupPass::Init”的声明`<br>`App.cpp(214): note: 尝试匹配参数列表“(T *, T *, ProbeGrid *, RenderGraph::ResourceId)”时 / with [T=ID3D12RootSignature] and [T=ID3D12PipelineState]` | 声明要 9 个参数，调用只给 4 个 |

#### C1~C6 的细节：声明与使用的断裂

```cpp
// ProbeBorderFixupPass.h:24-32  —— 私有成员区，只有 5 个 uint32_t
private:
    ID3D12RootSignature* m_rootSignature = nullptr;
    ID3D12PipelineState* m_pso = nullptr;
    ID3D12Resource* m_atlasResource = nullptr;
    RenderGraph::ResourceId m_atlasId = RenderGraph::kInvalidId;
    uint32_t m_uavIndex;         // :29  ← 注意：无默认初始化
    uint32_t m_stride;
    uint32_t m_atlasWidth;
    uint32_t m_atlasHeight;
    // ← 到此为止，没有 m_countX / m_countY / m_countZ
};
```

```cpp
// ProbeBorderFixupPass.cpp:14-23  —— Init 里凭空赋值
    m_atlasId = atlasId;
    m_atlasResource = atlasResource;
    m_countX = countX; m_countY = countY; m_countZ = countZ;   // :18  ← 3 条 C2065
    m_uavIndex = uavIndex;
```

```cpp
// ProbeBorderFixupPass.cpp:34-39  —— Execute 里再次凭空读取
    ProbeUpdateParams p{};
    p.countX = m_countX;      // :35  ← C2065
    p.countY = m_countY;      // :36  ← C2065
    p.countZ = m_countZ;      // :37  ← C2065
    p.borderTargetUav = m_uavIndex;
    p.borderTileStride = m_stride;
```

- `Init` 的**形参**里有 `countX, countY, countZ`（`ProbeBorderFixupPass.h:14`），说明作者知道要传进来；
- 但私有成员区漏写了三个 `uint32_t`。**这是纯粹的"写了一半"**，与算法无关。
- 附带隐患：`:29-32` 的 `m_uavIndex / m_stride / m_atlasWidth / m_atlasHeight` **没有默认初始化**（`= 0`）。当前 `Init` 会赋值所以不会用到未初始化值，但一旦 C7 修成"可选参数"，就会读到垃圾值。

#### C7 的细节：调用方与声明方各自演进过

```cpp
// ProbeBorderFixupPass.h:10-18  —— 9 个参数
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

```cpp
// App.cpp:214-216  —— 只传 4 个，且第 3、4 个实参类型也和声明对不上
m_probeBorderFixupPass.Init(
    m_probeRootSignature.Get(), m_probeBorderPSO.Get(),
    &m_probeGrid, m_probeGridId);
```

逐参数比对（`T*` 为 `ID3D12RootSignature*` / `ID3D12PipelineState*`，来自 cl 的 note）：

| 形参序号 | 形参类型（头文件） | `App.cpp:214` 实参 | 是否匹配 |
|---|---|---|---|
| 1 | `ID3D12RootSignature*` | `m_probeRootSignature.Get()` | ✅ |
| 2 | `ID3D12PipelineState*` | `m_probeBorderPSO.Get()` | ✅ |
| 3 | `RenderGraph::ResourceId` | `&m_probeGrid`（`ProbeGrid*`） | ❌ 类型不符 |
| 4 | `ID3D12Resource*` | `m_probeGridId`（`ResourceId`） | ❌ 类型不符 |
| 5 | `uint32_t countX` | — 缺失 | ❌ |
| 6 | `uint32_t countY` | — 缺失 | ❌ |
| 7 | `uint32_t countZ` | — 缺失 | ❌ |
| 8 | `uint32_t uavIndex` | — 缺失 | ❌ |
| 9 | `uint32_t stride` | — 缺失 | ❌ |
| 10 | `uint32_t atlasWidth` | — 缺失 | ❌ |
| 11 | `uint32_t atlasHeight` | — 缺失 | ❌ |

> `App.cpp:214` 显然是按**更早一版"只要 `ProbeGrid*` 就能自己取一切"**的签名写的。头文件改成"把参数一个个摊平传进来"，调用点没跟着改。

### 3.2 运行期错误（DXC，实测原文）

**共 11 条 error + 1 条 note。** 三条命令（`-E ProbeUpdateCS`、`-E BorderFixupCS`、`-E ProbeDistanceCS`）产出的诊断**完全相同**——因为 DXC 会对整个编译单元做语义分析，而三个入口都在同一个文件里。

| # | file:line | 错误原文（dxc 实测） | 根因 |
|---|---|---|---|
| H1 | `ShadersProbe.hlsl:74` | `error: use of undeclared identifier 'probeIndex'` | `ProbeDistanceCS` 内没有定义 `probeIndex`；它是 `ProbeUpdateCS` 的局部变量（`:24`），作用域不跨函数 |
| H2 | `ShadersProbe.hlsl:75` | 同上 | 同上 |
| H3 | `ShadersProbe.hlsl:76` | 同上 | 同上 |
| H4 | `ShadersProbe.hlsl:78` | `error: no member named 'gridOriginSpacing' in 'ProbeUpdateParams'` | 结构体字段已改名成 `originX/originY/originZ/spacing`（`ShadersProbe.hlsl:5`） |
| H5 | `ShadersProbe.hlsl:79` | 同上 | 同上 |
| H6 | `ShadersProbe.hlsl:89` | `error: redefinition of 'stride' with a different type: 'const uint' vs 'uint'`<br>`ShadersProbe.hlsl:66:10: note: previous definition is here` | 同一函数作用域内 `:66` 已有 `uint stride`，`:89` 又声明 `const uint stride` |
| H7 | `ShadersProbe.hlsl:97` | `error: use of undeclared identifier 'distAtlas'` | 实际声明的局部变量名是 `atlas`（`:95`） |
| H8 | `ShadersProbe.hlsl:98` | `error: use of undeclared identifier 'frameIndex'` | `ProbeDistanceCS` 没有取 `g_Probe.frameIndex`（`ProbeUpdateCS` 在 `:22` 取了） |
| H9 | `ShadersProbe.hlsl:103` | `error: use of undeclared identifier 'distAtlas'` | 同 H7 |
| H10 | `ShadersProbe.hlsl:112` | `error: no member named 'gridCountsAndFrame' in 'ProbeUpdateParams'` | 字段是 `countX/countY/countZ`（`:6`），无 `gridCountsAndFrame` |
| H11 | `ShadersProbe.hlsl:131` | `error: no member named 'misc' in 'ProbeUpdateParams'` | 无 `misc` 字段；CPU 侧写的是 `borderTargetUav`（偏移 52） |

#### 关键代码锚点

```hlsl
// ShadersProbe.hlsl:64-67
    uint3 counts = uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ);
    uint stride = g_Probe.borderTileStride; // 10 或 18      ← :66 定义了 stride
    uint target = g_Probe.borderTargetUav;  // 要修的目标 atlas
```

```hlsl
// ShadersProbe.hlsl:74-79
    uint px = probeIndex % counts.x;                        // :74  H1
    uint py = (probeIndex / counts.x) % counts.y;           // :75  H2
    uint pz = probeIndex / (counts.x * counts.y);           // :76  H3
    float3 probePos = g_Probe.gridOriginSpacing.xyz         // :78  H4
                    + float3(px, py, pz) * g_Probe.gridOriginSpacing.w;  // :79  H5
```

```hlsl
// ShadersProbe.hlsl:89（与 :66 同一作用域，重复声明）
    const uint stride = 18; // 16 + 2*1 gutter            // :89  H6
```

```hlsl
// ShadersProbe.hlsl:95-103
    RWTexture2D<float4> atlas = ResourceDescriptorHeap[target];   // :95  变量名是 atlas
    float4 old = distAtlas[pixel];                                 // :97  H7
    float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;               // :98  H8
    float mean  = lerp(old.x, t, alpha);
    float mean2 = lerp(old.y, t * t, alpha);
    distAtlas[pixel] = float4(mean, mean2, 0.0f, 1.0f);            // :103 H9
```

```hlsl
// ShadersProbe.hlsl:112 / 131
    uint3 counts = g_Probe.gridCountsAndFrame.xyz;                 // :112 H10
    RWTexture2D<float4> atlas = ResourceDescriptorHeap[g_Probe.misc.w];  // :131 H11
```

#### 关于 H10 / H11 的定性：**不是拼写问题，是"结构体版本割裂"**

`gridCountsAndFrame` 和 `misc` 在全工程**一次也没有被定义过**（已全文检索 `misc|gridCountsAndFrame`）：

| 名字 | 出现在哪 | 是否定义 |
|---|---|---|
| `gridOriginSpacing` | `ShadersProbe.hlsl:78,79`（用）；`ShadersCommon.hlsl:243`、`LightingTypes.h:18`、`LightingPass.cpp:71`、`ShadersLighting.hlsl:75,76`（**是 `LightingParams` 的字段，不是 `ProbeUpdateParams` 的**） | 只存在于 `LightingParams` |
| `gridCountsAndFrame` | 仅 `ShadersProbe.hlsl:112` | **无定义** |
| `gridCountsAndProbeIndex` | `ShadersCommon.hlsl:244`、`ShadersLighting.hlsl:77,91,98,107` | 是 `LightingParams` 的字段 |
| `misc` | 仅 `ShadersProbe.hlsl:131` | **无定义**（`dds_loader.cpp:75,77` 里的 `miscFlag/miscFlags2` 是 DDS 头字段，无关） |

> 判定：`ProbeDistanceCS` / `BorderFixupCS` 是从**更早的"探针与光照共用一个参数结构体"版本**粘过来的。后来 `ProbeUpdateParams` 被重构成扁平字段（`ProbeTypes.h` + `ShadersProbe.hlsl:3-12` 同步改了），但这两个函数的**函数体没跟着改**。`gridCountsAndFrame` 连旧版本里都不叫这名字（旧的很可能是 `gridCountsAndProbeIndex` 或 `misc`），属于**二次改名遗漏**。

### 3.3 错误传播链：为什么"修 ProbeUpdateCS 就够"是错的

这是本次审计**最有操作价值的发现**，务必确认：

```
ShadersProbe.hlsl  ← 单一编译单元，内含 3 个入口
        │
        ├─ ProbeUpdateCS   (:17)  ← 本身 0 错误
        ├─ ProbeDistanceCS (:60)  ← 含 H1~H9
        └─ BorderFixupCS   (:110) ← 含 H10~H11
                    │
                    ▼
    DXC 对整个 TU 做语义分析（不只看 -E 指定的入口）
                    │
                    ▼
  App.cpp:193  CompileShaderSM66(L"ShadersProbe.hlsl", L"ProbeUpdateCS", L"cs_6_6")
                    │  失败
                    ▼
  d3d12lite.cpp:331-334  throw std::runtime_error("DXC Shader Compilation Failed: ...")
                    │
                    ▼
  App.cpp:195  assert(!probeCsBlob.empty());   ← 永远执行不到
                    ▼
        App::init 抛异常 → 程序无法启动
```

**实测证据（决定性）**：

| 编译对象 | 入口 | 结果 |
|---|---|---|
| 原始 `ShadersProbe.hlsl` | `-E ProbeUpdateCS` | ❌ exit `-2147467259`，11 条 error |
| 原始 `ShadersProbe.hlsl` | `-E BorderFixupCS` | ❌ exit `-2147467259`，**同样 11 条 error** |
| 原始 `ShadersProbe.hlsl` | `-E ProbeDistanceCS` | ❌ exit `-2147467259`，**同样 11 条 error** |
| **临时副本只保留第 1~55 行**（即只留 `ProbeUpdateCS`） | `-E ProbeUpdateCS` | ✅ **exit 0，零诊断** |

→ `ProbeUpdateCS` 自身的着色器代码是**干净可用**的；11 条错误 100% 来自另外两个函数。**但因为同处一个 TU，它一起陪葬。**

> 两个 DXC 版本（工程自带的 1.8.2502.11 与 NuGet 的 1.9.2609.5）诊断**逐字一致**，排除"版本差异"这种解释。

### 3.4 已排查的"伪错误"（避免误导修复方向）

| 现象 | 是不是真错误 | 依据 |
|---|---|---|
| `main.cpp` 报 `C2440 / C2664`（`const wchar_t[]` → `LPCSTR`） | ❌ **伪错误**。只是我的临时命令漏了 `/DUNICODE /D_UNICODE` | 工程 `CharacterSet=Unicode`（`.vcxproj:34,41,47,54`）；补上后 `main.cpp` **exit=0** |
| `#include "Pass.h"` 但磁盘上是 `pass.h` | ⚠️ 隐患，当前**不报错** | `ProbeUpdatePass.h:1`、`ProbeBorderFixupPass.h:2`、`RenderGraph.cpp:2` 都写 `"Pass.h"`；NTFS 大小写不敏感所以能过。换到大小写敏感的文件系统/远程构建会把所有文件炸掉 |
| `ProbeDistancePass.h` 只有 `#pragma once` | ❌ 不报错 | 它只作为 ClInclude 存在（`.vcxproj:189`），无人 include |
| `ProbeDistanceCS.hlsl` 0 字节 | ❌ 不报错 | `<None>` + 全配置 `ExcludedFromBuild`（`.vcxproj:207-212`），MSBuild 完全不碰 |
| `ProbeUpdatePass.cpp` 能编过，是否说明它没问题 | ❌ 只能说明**没有语法/类型错误** | 逻辑问题见 §4.3、§5、§6 |

### 3.5 全工程编译扫描结果（证明"只有探针在报错"）

对 `.vcxproj` 里全部 27 个 `ClCompile` 源文件逐一执行 `cl /c`：

| 结果 | 文件 |
|---|---|
| 通过（25 个） | `AccelerationStructure` `BindlessDescriptorHeap` `Camera` `ComputeSmokePass` `CPUDescriptorAllocator` `d3d12lite` `dds_loader` `GBufferPass` `Input` `LightingPass` `main` `Mesh` `MeshLibrary` `ProbeGrid` `ProbeUpdatePass` `RenderContext` `RenderGraph` `RenderTexture` `RootSignature` `Scene` `SceneGpuData` `Texture2D` `TransientHeap` `UploadHeap` `DepthBuffer` |
| **失败（2 个）** | `ProbeBorderFixupPass.cpp`（6× C2065）、`App.cpp`（1× C2660） |

> 结论：**探针改造是当前唯一阻碍工程构建的地方**，其它 25 个编译单元是干净的。好消息。

---

## 4. CPU 侧 ↔ HLSL 侧常量缓冲逐字段对照

### 4.1 结构体声明层：**完全一致，零不一致**

`ProbeTypes.h:5-23` vs `ShadersProbe.hlsl:3-12`，逐字段：

| 字节偏移 | 大小 | `ProbeTypes.h` 字段 | `ShadersProbe.hlsl` 字段 | 类型 | 一致？ |
|---|---|---|---|---|---|
| 0 | 4 | `float originX` | `float originX` | float | ✅ |
| 4 | 4 | `float originY` | `float originY` | float | ✅ |
| 8 | 4 | `float originZ` | `float originZ` | float | ✅ |
| 12 | 4 | `float spacing` | `float spacing` | float | ✅ |
| 16 | 4 | `uint32_t countX` | `uint countX` | uint | ✅ |
| 20 | 4 | `uint32_t countY` | `uint countY` | uint | ✅ |
| 24 | 4 | `uint32_t countZ` | `uint countZ` | uint | ✅ |
| 28 | 4 | `uint32_t frameIndex` | `uint frameIndex` | uint | ✅ |
| 32 | 4 | `uint32_t probeCount` | `uint probeCount` | uint | ✅ |
| 36 | 4 | `uint32_t rayCount` | `uint rayCount` | uint | ✅ |
| 40 | 4 | `uint32_t objectIndex` | `uint objectIndex` | uint | ✅ |
| 44 | 4 | `uint32_t irradianceUav` | `uint irradianceUav` | uint | ✅ |
| 48 | 4 | `uint32_t distanceUav` | `uint distanceUav` | uint | ✅ |
| 52 | 4 | `uint32_t borderTargetUav` | `uint borderTargetUav` | uint | ✅ |
| 56 | 4 | `uint32_t borderTileStride` | `uint borderTileStride` | uint | ✅ |
| 60 | 4 | `uint32_t reserved0` | `uint reserved0` | uint | ✅ |
| **64** | — | **`static_assert(sizeof==64)`** (`ProbeTypes.h:24`) | 无 padding（全 4 B 标量） | — | ✅ |

布局侧的三方对齐也成立：

| 环节 | 值 | 位置 |
|---|---|---|
| C++ 结构体大小 | 64 B（`static_assert` 强制） | `ProbeTypes.h:24` |
| 根签名预留 | `Add32BitConstants(16, 4)` = 16 DWORD @ b4 | `App.cpp:186` |
| 上传 DWORD 数 | `SetComputeRoot32BitConstants(0, 16, &p, 0)` | `ProbeUpdatePass.cpp:51`、`ProbeBorderFixupPass.cpp:43` |
| HLSL 绑定 | `ConstantBuffer<ProbeUpdateParams> g_Probe : register(b4)` | `ShadersProbe.hlsl:13` |

> **修正任务描述中的一个前提**：任务点 3 预期"CPU 64 字节 vs HLSL 结构体，列出不一致处"。实测结论是**结构体声明没有任何不一致**。真正的不一致发生在**着色器函数体 ↔ 结构体**之间（§3.2 的 H4/H10/H11），以及**CPU 填充 ↔ 结构体字段**之间（§4.3）。

### 4.2 一个结构体，三个 pass，字段子集互不重叠（架构问题）

| 字段 | 偏移 | `ProbeUpdateCS` 用 | `ProbeDistanceCS` 用 | `BorderFixupCS` 用 | CPU 侧谁填 |
|---|---|---|---|---|---|
| `originX/Y/Z`, `spacing` | 0-12 | ✅ `:34-35` | ⚠️ 想要（写成 `gridOriginSpacing`，H4/H5） | ❌ | `ProbeUpdatePass.cpp:31-34` |
| `countX/Y/Z` | 16-24 | ✅ `:30-32` | ✅ `:64` | ⚠️ 想要（写成 `gridCountsAndFrame`，H10） | Update`:35-37` / Fixup`:35-37` |
| `frameIndex` | 28 | ✅ `:22` | ⚠️ 想要（H8） | ❌ | Update`:38` / **Fixup 未填（=0）** |
| `probeCount` | 32 | ✅ `:21` | ❌ | ❌ | Update`:39` |
| `rayCount` | 36 | ❌ **shader 从不读** | ❌ | ❌ | Update`:40`（硬写 64） |
| `objectIndex` | 40 | ✅ 经 `TraceRadiance`→`g_Lighting` 间接（实际读的是 b2） | ❌ | ❌ | Update`:41` |
| `irradianceUav` | 44 | ✅ `:50` | ❌ | ❌ | Update`:42` |
| `distanceUav` | 48 | ❌ | ⚠️ 本该用（改用 `borderTargetUav`，见下） | ❌ | **无人填（=0）** |
| `borderTargetUav` | 52 | ❌ | ⚠️ 被当作距离图 UAV 用 `:67` | ⚠️ 想要（写成 `misc.w`，H11） | Fixup`:38` |
| `borderTileStride` | 56 | ❌ | ⚠️ 被当作自己的 stride 用 `:66` | ❌ | Fixup`:39` |
| `reserved0` | 60 | ❌ | ❌ | ❌ | 无人填 |

> 三个 pass 共用一个 64 B 块，但**没有一个字段是三方都要的**。其中 `distanceUav`(48)、`reserved0`(60) **全工程无人写入、无人读取**——是纯粹的占位。
>
> 语义污染最典型的一处：`ProbeDistanceCS:66` 把 `borderTileStride`（本来语义是"gutter 修补的目标 stride"）当成"距离矩自己的 stride"用，然后在 `:89` 又用一个局部 `const uint stride = 18` 把它覆盖掉。**同一个字段在同一函数里被赋予两种含义**，H6 正是这次覆盖引发的重复声明。

### 4.3 CPU 侧填充矩阵：两个 pass 各填一半，谁也不填另一半

```cpp
// ProbeUpdatePass.cpp:30-42
       ProbeUpdateParams p{};          // ← 值初始化，未赋的字段 = 0
    p.originX = grid.origin.x;         // :31
    ...
    p.spacing = grid.spacing;          // :34
    p.countX = grid.countX;            // :35
    p.countY = grid.countY;            // :36
    p.countZ = grid.countZ;            // :37
    p.frameIndex = m_frameIndex++;     // :38
    p.probeCount = grid.GetProbeCount(); // :39
    p.rayCount = 64;                   // :40
    p.objectIndex = m_objectDataBindlessIndex; // :41
    p.irradianceUav = m_probeGrid->GetUAVIndex(); // :42
    // ← distanceUav / borderTargetUav / borderTileStride / reserved0 全为 0
```

```cpp
// ProbeBorderFixupPass.cpp:34-39
    ProbeUpdateParams p{};              // ← 同样值初始化
    p.countX = m_countX;                // :35
    p.countY = m_countY;                // :36
    p.countZ = m_countZ;                // :37
    p.borderTargetUav = m_uavIndex;     // :38
    p.borderTileStride = m_stride;      // :39
    // ← origin/spacing / frameIndex / probeCount / rayCount /
    //    objectIndex / irradianceUav / distanceUav 全为 0
```

| 字段 | 偏移 | `ProbeUpdatePass` | `ProbeBorderFixupPass` | 后果 |
|---|---|---|---|---|
| `originX/Y/Z`, `spacing` | 0-12 | ✅ 填 | ❌ 留 0 | Fixup 侧 shader 现在不用；若按 H4/H5"顺手改成 `originX`"就会拿到 `(0,0,0)`，probe 位置全错 |
| `countX/Y/Z` | 16-24 | ✅ | ✅ | 正常 |
| `frameIndex` | 28 | ✅ `m_frameIndex++` | ❌ 恒 0 | 若 Fixup shader 改成读 `g_Probe.frameIndex`，**每帧都会走 `frameIndex==0` 全量替换分支**，hysteresis 失效 |
| `probeCount` | 32 | ✅ | ❌ | 目前无影响 |
| `rayCount` | 36 | ✅ 硬写 64 | ❌ | shader 从不读，无效字段 |
| `objectIndex` | 40 | ✅ | ❌ | 无影响（`TraceRadiance` 走的是 b2） |
| `irradianceUav` | 44 | ✅ | ❌ 留 0 | ⚠️ **若 Fixup shader 误用该字段，会拿 Slice 0 的描述符槽** |
| `distanceUav` | 48 | ❌ 留 0 | ❌ 留 0 | ⚠️ `ProbeDistanceCS` 一旦接线，会去索引**描述符堆第 0 槽**（当前是 gbuffer albedo 之类），越权写别人资源 |
| `borderTargetUav` | 52 | ❌ 留 0 | ✅ | 正常（但 shader 侧写成 `misc.w`，H11） |
| `borderTileStride` | 56 | ❌ 留 0 | ✅ | 正常（但被 `ProbeDistanceCS` 挪用后又覆盖，H6） |
| `reserved0` | 60 | ❌ | ❌ | 死字段 |

> **风险定级**：`distanceUav = 0` 与 `irradianceUav` 的 0 默认值是**安静的越权写**——不会报错，只会在将来某天"探针距离矩把某个 G-Buffer 写花了"时花掉半天排查。

### 4.4 顺带发现：`b2 LightingParams` 的注释与实现脱节（不影响编译）

| 位置 | 注释声称 | 实际 |
|---|---|---|
| `LightingTypes.h:5` | `// Lighting pass 参数：root constants b2（8 DWORD = 32 B）` | 实际 **16 DWORD = 64 B**（`static_assert` 在 `:24`） |
| `ShadersCommon.hlsl:232` | `// ---- b2：Lighting pass 参数（12 DWORD = 48 B）----` | 实际 **16 DWORD = 64 B**（8 uint + float4 + uint4） |
| `LightingPass.cpp:77` | `SetGraphicsRoot32BitConstants(2, 16, &lp, 0);   // 12 → 16` | 16 是对的，注释是改到一半的痕迹 |

结论：`b2` 的 CPU/HLSL 布局**实际一致且正确**（64 B 对齐，`App.cpp:187 Add32BitConstants(16, 2)` 也一致），只是三处注释都停留在旧尺寸。**这是文档债，不是 bug**，但会误导后来人，建议随本次改造一并订正。

---

## 5. 线程映射与 Dispatch 核对

### 5.1 `ProbeUpdateCS`：**匹配，正确**

```hlsl
// ShadersProbe.hlsl:16-17, 24-28
[numthreads(8, 8, 1)]
void ProbeUpdateCS(uint3 id : SV_DispatchThreadID)
{
    uint probeIndex = id.x / 8;              // :24
    ...
    uint2 texel = uint2(id.x % 8, id.y);     // :28
```

```cpp
// ProbeUpdatePass.cpp:58-59
    // 每个 group = 一个探针（8×8 线程 = 64 个 octahedral texel）
    cmdList->Dispatch(grid.GetProbeCount(), 1, 1);
```

推导（D3D 语义：`SV_DispatchThreadID.x == SV_GroupID.x * 8 + SV_GroupThreadID.x`）：

| 量 | 取值范围 | 说明 |
|---|---|---|
| `SV_GroupID.x` | `[0, probeCount)` | 由 `Dispatch(probeCount,1,1)` 决定 |
| `SV_GroupThreadID.x` | `[0, 8)` | 由 `numthreads(8,8,1)` 决定 |
| `id.x` | `[0, 8*probeCount)` | 合成值 |
| `id.x / 8` | `[0, probeCount)` | **恒等于 `SV_GroupID.x`** → `probeIndex` 正确，一个 group 恰好一个探针 |
| `id.x % 8` | `[0, 8)` | tile 内 x |
| `id.y` | `[0, 8)` | tile 内 y |

→ **`probeIndex` 与 `texel` 都正确，一 group 一探针，64 texel / 探针。这条不是 bug。** 任务点里"是否匹配"的答案是：匹配。

两点非致命的观察：

1. `ShadersProbe.hlsl:25` 的 `if (probeIndex >= probeCount) return;` 是**死代码**——按上面的推导 `probeIndex` 恒 `< probeCount`。留着无害，但说明作者当时对映射不确定。
2. 改用 `SV_GroupID` 直接取 `probeIndex` 会更清晰（`ProbeDistanceCS` 已经声明了 `SV_GroupID` 形参 `:61` 却没用，说明作者本来就想这么写）。

### 5.2 `ProbeDistanceCS`：**两套映射模型互相打架（且 dispatch 根本不存在）**

```hlsl
// ShadersProbe.hlsl:57-62, 71-72, 81
// 距离矩更新：每个 group = 一个探针，16×16 线程 = 256 个方向 texel
[numthreads(16, 16, 1)]
void ProbeDistanceCS(uint3 id : SV_DispatchThreadID,
                     uint3 groupId : SV_GroupID,          // :61  声明了但全程未使用
                     uint3 gtid : SV_GroupThreadID)
{
    ...
    uint atlasW = counts.x * stride;      // :69
    uint atlasH = counts.y * counts.z * stride;   // :70
    if (id.x >= atlasW || id.y >= atlasH)  // :71  ← 模型 A：id 当【图集绝对坐标】
        return;
    ...
    uint2 texel = gtid.xy; // 0..15        // :81  ← 模型 B：gtid 当【tile 内坐标】
```

两套模型**同时存在**：

| 模型 | 用在 | 隐含 dispatch |
|---|---|---|
| A：`id.xy` = 图集绝对像素坐标 | `:71-72` 的边界检查 | `Dispatch(ceil(atlasW/16), ceil(atlasH/16), 1)` |
| B：一 group 一探针，`gtid.xy` = tile 内 texel | `:81`、注释 `:57` | `Dispatch(probeCount, 1, 1)` |

按注释 `:57` 采用模型 B 时会发生什么（假设 `stride=18`, counts=(8,4,8)：

| 量 | 值 |
|---|---|
| `atlasW = 8*18` | 144 |
| `atlasH = 4*8*18` | 576 |
| `id.x` 范围 | `[0, 16*256) = [0, 4096)` |
| `id.y` 范围 | `[0, 16)` |
| `if (id.x >= 144)` | **杀掉 4096-144 = 3952 个线程（96.5%）** |
| `if (id.y >= 576)` | 永不成立（`id.y < 16`） |
| 幸存的 144×16 个线程 | 跑 `probeIndex`（未定义）→ H1~H3 |

→ **即使把 11 条编译错误全修完，`ProbeDistanceCS` 的线程模型仍然是错的。** 正确写法应是：`probeIndex` 取自 `groupId.x`，`texel` 取自 `gtid.xy`，**删掉 `:71-72` 的图集空间边界检查**（tile 内 16×16 天然不越界）。

另外，`ProbeDistanceCS` **没有任何 C++ 侧调用者**：没有 `ProbeDistancePass` 类、没有 PSO、没有 `Dispatch`。`App.cpp` 只编译 `ProbeUpdateCS`(`:194`) 和 `BorderFixupCS`(`:207`) 两个入口。

### 5.3 `BorderFixupCS`：Dispatch 匹配，但 stride 写死

```hlsl
// ShadersProbe.hlsl:109-119
[numthreads(8, 8, 1)]
void BorderFixupCS(uint3 id : SV_DispatchThreadID)
{
    ...
    const uint stride = 10;                       // :113 ← 写死 10
    uint atlasW = counts.x * stride;              // :116
    uint atlasH = counts.y * counts.z * stride;   // :117
    if (id.x >= atlasW || id.y >= atlasH) return; // :118-119
```

```cpp
// ProbeBorderFixupPass.cpp:44
    cmdList->Dispatch((m_atlasWidth + 7) / 8, (m_atlasHeight + 7) / 8, 1);
```

| 检查项 | 结论 |
|---|---|
| `Dispatch(ceil(w/8), ceil(h/8))` × `numthreads(8,8)` | ✅ 覆盖全图集，边界检查 `:118` 兜住尾部 |
| `stride = 10` 写死 | ⚠️ **无法服务 18-stride 的距离矩图集**。CPU 明明传了 `borderTileStride`（`:39`），shader 却不用 |
| `counts` 来源 | ❌ `g_Probe.gridCountsAndFrame`（H10），应为 `g_Probe.countX/countY/countZ` |
| `atlasWidth/atlasHeight` CPU 侧传参 | ⚠️ 传了（`ProbeBorderFixupPass.cpp:44` 直接用 `m_atlasWidth/Height`），但 shader 不用，属于**同一信息存了两份**（CPU 算一遍、shader 再算一遍），一旦 stride 不一致就错位 |
| gutter 填充算法 | 见 §6.6 |

### 5.4 gutter 填充的竞争分析（顺带确认）

`ShadersProbe.hlsl:121-154` 的核心是：

```hlsl
    int2 tile = int2(p) / int(stride);            // :123
    int2 local = int2(p) % int(stride);           // :124
    bool gutterX = (local.x == 0) || (local.x == int(stride) - 1);  // :126
    bool gutterY = (local.y == 0) || (local.y == int(stride) - 1);  // :127
    if (!gutterX && !gutterY) return;             // :128-129 内部不动
    ...
    maskX = (local.x == 0) ? 1 : -1;              // :142
    maskY = (local.y == 0) ? 1 : -1;              // :146
    int2 dstPix = tileOrigin + local;             // :150
    int2 srcPix = dstPix + int2(maskX, maskY);    // :151
    atlas[dstPix] = atlas[srcPix];                // :154
```

| 分析项 | 结论 |
|---|---|
| 读写是否重叠（数据竞争） | ✅ **无竞争**。写只落在 gutter（`local == 0` 或 `stride-1`），读的 `srcPix` 在 gutter 时偏移 ±1 必然落在内部 `[1, stride-2]`。写集合 ∩ 读集合 = ∅ |
| 是否越界 | ✅ 不会。`local.x==0 → +1`、`local.x==9 → -1`，都在同一 tile 内 |
| `[0..9]` 与 `int(stride)` 混用 | ⚠️ 不一致但结果正确：`local.x == int(stride)-1` 即 `==9`，与 `local.x == 0` 对称。表达式里 `int(stride)`（`:126,127,141,142,146,147`）和裸字面 `0/1/-1` 混排，可读性差、`stride` 若改 18 需逐处检查 |
| 四角 gutter（`gutterX && gutterY`） | ⚠️ `maskX` 与 `maskY` 同时非 0，做的是**对角复制**；论文用的是八面体展开后重建，不是对角复制（详见 §6.6） |

---

## 6. 与 DDGI 论文要求的功能差距

### 6.0 关于"论文要求"一列的说明（诚实标注）

| 标记 | 含义 |
|---|---|
| **【论文】** | 依据 DDGI 论文（Majercik et al., JCGT 8(2), 2019）的公开主张。已核对 JCGT 摘要原文，确认其核心是 "compact encoding of the full irradiance field" 与 "a robust filtered irradiance query, using a **novel visibility-aware moment-based interpolant**"（[jcgt.org/published/0008/02/01](https://jcgt.org/published/0008/02/01/)）。**未逐条核对论文正文的章节号与公式编号**，如需精确引用请复核原文 |
| **【任务】** | 取自本次任务说明所列的 7 个关注点 |
| **【未确认】** | 需要作者确认设计意图，或需要运行/测量才能判定 |

### 6.1 总览表

| # | 关注点 | 状态 | 一句话判定 |
|---|---|---|---|
| 1 | 辐照度 8×8 octahedral + gutter | 🟡 **结构齐全，内容错** | tile/stride/gutter 都搭好了，但 shader 编译不过；且存的是"无 cos 加权的 radiance 平均"，不是 irradiance |
| 2 | 距离矩 16×16 | 🔴 **只建了资源，逻辑全废** | 纹理建了却从未注册/绑定；生产者 shader 编译不过、无 pass、无 dispatch |
| 3 | 能量守恒权重 + 可见性混合（Chebyshev） | 🔴 **完全未实现** | 无 Chebyshev、无 probe 可见性权重、无背面权重 |
| 3b | probe relocation | ⚪ **不是与本论文的差距** | 论文本身**不做** relocation（属于 Unity 等后续变体）。本工程也没做，属于"与论文一致"。见 §6.3 澄清 |
| 4 | 采样时三线性 + 四面体插值权重 | 🔴 **完全未实现** | 着色路径根本不采样探针；只有 debug 视图里的最近邻单探针查找 |
| 5 | 探针分类（active/inactive） | 🔴 **完全未实现** | 无 probe state buffer、无分类 pass、无相关字段 |
| 6 | 背面 / 无效方向回退 | 🔴 **完全未实现** | gutter 只做邻居复制；无八面体接缝重建；无"背面命中回退上一帧" |
| 7 | 时间累积 hysteresis | 🟡 **有骨架，语义偏** | 固定 α=0.05 存在；但每 texel 每帧永远同一条确定性射线，EMA 累积的是同一个样本，起不到论文的去 banding 作用 |
| 8 | 随机旋转方向避免 banding | 🔴 **完全未实现** | `StochasticFibonacci` 等工具函数已写好，**零调用者** |

### 6.2 逐条展开

#### ① 辐照度 8×8 octahedral + gutter — 🟡 半成品

已具备的（`ProbeGrid.h:11-13`）：

```cpp
    static constexpr uint32_t kTileSize = 8;
    static constexpr uint32_t kGutter = 1;
    static constexpr uint32_t kTileStride = kTileSize + 2 * kGutter; // 10
```

| 检查项 | 结论 | 位置 |
|---|---|---|
| tile 8×8 | ✅ | `ProbeGrid.h:11` |
| 1 px gutter | ✅ | `ProbeGrid.h:12-13` |
| 图集尺寸 | ✅ `80 × 320`（8×10 宽，4×8×10 高） | `ProbeGrid.h:34-35` |
| 格式 | ✅ `R16G16B16A16_FLOAT` | `ProbeGrid.cpp:8` |
| 无 mip（避免降采样/滤波） | ✅ `MipLevels = 1` | `RenderTexture.cpp:82` |
| texel→方向 | ✅ 八面体中心采样 `OctDecode((texel+0.5)/8)` | `ShadersProbe.hlsl:38-39` |
| **存的是不是 irradiance** | ❌ **不是** | 见下 |

**核心问题：存的是"方向辐亮度平均"，不是辐照度。**

```hlsl
// ShadersProbe.hlsl:42-54
    float3 radiance = TraceRadiance(probePos + dir * 0.02f, dir);   // :42
    ...
    float4 old = atlas[pixel];                                       // :52
    float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;                 // :53
    atlas[pixel] = float4(lerp(old.rgb, radiance, alpha), 1.0f);     // :54
```

缺三样东西：

| 缺失项 | 为什么必须有 | 后果 |
|---|---|---|
| `max(0, dot(dir, texelDir))` 余弦加权 | 辐照度定义是 `∫ L(ω)·cosθ dω`；均匀平均只是 `∫ L dω / N` | 值在物理上不是辐照度，量纲/归一化都错 |
| 八面体立体角 Jacobian | 八面体映射**不是等面积**的（角部 texel 覆盖的立体角远大于中心）。等权平均等于按"texel 个数"而不是"立体角"加权 | 图集角部/边缘方向被系统性高估或低估，重建出的辐照度场有方向性偏色 |
| `1/π` 规范化 | 漫反射 BRDF `ρ/π` 中的 π 因子 | 若将来直接乘 albedo 使用，亮度会差一个 π |

> 另外 `TraceRadiance`（`ShadersCommon.hlsl:306-348`）返回的是**有偏的** 1-bounce 近似：命中点直接光 + `SkyColor()*0.3`（`:347`），没有除以 pdf、也没有对 albedo 做能量守恒处理，且 `EvaluateDirectLighting` 里对所有 `k>0` 的点光不投阴影（`:298` 只有 `type==0` 才 `TraceSunShadow`）。这不是 DDGI 的错，但意味着探针里的值本身就有系统偏差。

**gutter 是否被真正使用**：没有。唯一的读取路径是 debug 视图的最近邻 `.Load`：

```hlsl
// ShadersLighting.hlsl:92
        return float4(atlas.Load(int3(pixel, 0)).rgb, 1.0f);
```

`.Load` 是 **texel 精确读取**，不插值，**不经过 gutter**。也就是说 gutter 现在被算出来（如果它能编译的话）但没人消费——因为真正的采样路径还不存在（§6.5）。

#### ② 距离矩 16×16 — 🔴 资源建好了，链路 0% 可用

已具备（`ProbeGrid.h:14-16, 37-42` + `ProbeGrid.cpp:10-13`）：

```cpp
    static constexpr uint32_t kDistanceTileSize = 16;
    static constexpr uint32_t kDistanceTileStride = kDistanceTileSize + 2 * kGutter; // 18
    ...
    m_distanceAtlas.InitUAV(device, bindless,
        GetDistanceAtlasWidth(), GetDistanceAtlasHeight(),     // 144 × 576
        DXGI_FORMAT_R16G16B16A16_FLOAT);
```

**断开的地方（逐项证据）**：

| 环节 | 状态 | 证据 |
|---|---|---|
| 纹理创建 | ✅ | `ProbeGrid.cpp:11-13`，144×576 RGBA16F（8×18 宽，4×8×18 高），含 SRV+UAV 描述符 |
| 注册进 RenderGraph | ❌ **从未注册** | `App.cpp:180-182` 只注册了 `m_probeGrid.GetResource()`（辐照度）；全文检索 `GetDistanceResource()` 命中数 = **0** |
| 被任何 pass 当作 UAV 绑定 | ❌ | 全文检索 `GetDistanceUAVIndex()` 命中数 = **0**（仅定义处 `ProbeGrid.h:40`） |
| 被任何 pass 当作 SRV 采样 | ❌ | 全文检索 `GetDistanceSRVIndex()` 命中数 = **0** |
| 生产者 shader | ❌ 编译不过 | `ShadersProbe.hlsl:60-104`，含 H1~H9 共 9 条 error |
| 生产者 pass / PSO / Dispatch | ❌ **全不存在** | `ProbeDistancePass.h` 只有 1 行 `#pragma once`；无 `ProbeDistancePass.cpp`；`App.cpp` 无对应 `CompileShaderSM66` |
| `ProbeUpdateParams.distanceUav` | ❌ CPU 从不写 | §4.3 |
| 距离矩 gutter 修补 | ❌ 无法复用 | `BorderFixupCS:113` 写死 `stride = 10` |
| 填充函数 | 🟡 已写好但无人用 | `TraceHitDistance`（`ShadersCommon.hlsl:213-229`），唯一调用点是死代码 `ShadersProbe.hlsl:86` |
| `ProbeDistanceCS.hlsl` | ❌ 0 字节空文件 | 已被排除出构建（`.vcxproj:207-212`） |

> 记忆点：**距离矩 = 100% 死资源**。它占用了 144×576×8 B ≈ 648 KB 显存和 2 个 bindless 槽位，产出为零。Chebyshev 可见性测试（论文的 "visibility-aware moment-based interpolant"）所需的全部数据这条链路一个字节都提供不了。

#### ③ 能量守恒权重与可见性混合（Chebyshev）— 🔴 完全未实现

| 论文要素 | 本工程 |
|---|---|
| 距离矩 → Chebyshev 上界：`p(d > t) ≤ σ²/(σ² + (t-μ)²)` | ❌ 无任何 Chebyshev 实现。全文检索 `Chebyshev` 命中 0 |
| 探针可见性权重 `w_vis` 与辐照度插值加权混合 | ❌ 无（因为连插值都没有，§6.5） |
| 背面权重 `backfaceWeight`（缓解几何薄片/漏光） | ❌ 无 |
| 采样时按距离矩做遮挡衰减 | ❌ 无 |

> 现有代码里唯一的"距离"痕迹是 `TraceHitDistance`（`ShadersCommon.hlsl:213`），它本身写得是对的（未命中返回 `tMax`，注释 `:212` 也说明是为了 Chebyshev 的"很远"），但**没有任何消费者**。属于"零件已备、装配未开始"。

#### ③b probe relocation — ⚪ 澄清：这不是与本论文的差距

任务说明里写的是"能量守恒的权重与可见性混合（probe relocation 未做）"。这里需要**主动纠偏**，否则会误导作者去做不该做的事：

- **DDGI 论文（Majercik et al. 2019）本身不做 probe relocation。** 论文固定均匀网格，靠距离矩 + 可见性权重来抑制漏光。
- 把探针"搬"出几何体的 relocation 是后续工程变体（Unity HDRP 的 DDGI 实现、以及一些生产化扩展）引入的。
- 因此本工程"没做 relocation"**不构成与论文的差距**，属于与论文一致。真正该补的是 §6.3（Chebyshev 可见性）和 §6.4（插值），而不是 relocation。

> 【未确认】如果作者的意图是对标 Unity 版 DDGI（而非论文原版），那么 relocation 确实缺失，应单独立项。请作者确认对标目标。

#### ④ 三线性 + 四面体插值权重 — 🔴 完全未实现（且这是最致命的）

**证据：着色主路径根本不读探针。**

```hlsl
// ShadersLighting.hlsl:116-135  —— 最终着色的全部内容
    // ---- 纯光追 GI：每像素 2 条余弦半球射线 ----
    const uint giSamples = 2;                                    // :117
    float3 indirect = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0; i < giSamples; ++i) {
        uint sampleIndex = g_Frame.frameIndex * giSamples + i;   // :122
        float2 u = QuasiRandom2D(pixel, sampleIndex);            // :123
        indirect += TraceGI(world.xyz, N, u);                    // :124
    }
    indirect /= giSamples;                                       // :126
    ...
    float3 direct = EvaluateDirectLighting(world.xyz, N);        // :133
    float3 color = albedo.rgb * (direct + ambient.rgb + indirect); // :134
    return float4(color, 1.0f);
```

- 探针图集在 `LightingPS` 里**一次都没被采样**。`g_Lighting.gridCountsAndProbeIndex.w`（探针 SRV 槽）只出现在 debug 分支 `:91` 和 `:107`。
- 也就是说：**DDGI 目前是一个"调试可视化功能"，不是光照功能。** 这是本次审计认为最严重的问题（§0 结论 5）。

**缺失的具体环节**：

| 环节 | 状态 | 备注 |
|---|---|---|
| 世界坐标 → 归一化网格坐标 `(p - origin)/spacing` | 🟡 仅 debug 分支里有 | `ShadersLighting.hlsl:79`（用 `round` 取整） |
| 取 8 个邻域探针索引 | ❌ | 无 |
| 三线性权重（`frac` 三轴乘积，8 个权重） | ❌ | 无 |
| 四面体插值（4 个非零权重，比三线性更平滑、无对角线伪影） | ❌ | 无 |
| 每探针的 octahedral 双线性采样 + half-texel 校正 | ❌ | 无 |
| 邻接 tile 间的 gutter 过渡 | ❌ | 无（因为没插值，也就没用到 gutter） |

**唯一"形似插值"的代码**是 debug 视图的最近邻单点查找：

```hlsl
// ShadersLighting.hlsl:79-92
        int3 idx = (int3) round((world.xyz - origin) / spacing);          // :79 最近邻取整
        idx = clamp(idx, int3(0, 0, 0), int3(counts) - 1);                // :80
        uint probeIndex = (idx.z * counts.y + idx.y) * counts.x + idx.x;  // :81 z-major 打包
        float2 uv = OctEncode(N);                                         // :83
        uint2 texel = uint2(min(uv * 8.0f, 7.0f));                        // :84 nearest texel
        const uint stride = 10;                                           // :86 又写死
        uint2 pixel = uint2(
            (probeIndex % counts.x) * stride + 1 + texel.x,               // :88
            (probeIndex / counts.x) * stride + 1 + texel.y);              // :89
        Texture2D<float4> atlas = ResourceDescriptorHeap[g_Lighting.gridCountsAndProbeIndex.w];
        return float4(atlas.Load(int3(pixel, 0)).rgb, 1.0f);              // :92
```

**顺带确认（这里的打包/解包是一致的，不是 bug）**：

| 侧 | `px` | `py` | `pz` | tile 行 |
|---|---|---|---|---|
| `ProbeUpdateCS:30-32` | `i % cx` | `(i/cx) % cy` | `i/(cx*cy)` | `(py + pz*cy)*stride` `:47-48` |
| `ShadersLighting.hlsl:81` debug 查表 | `i % cx` | `(i/cx) % cy` | `i/(cx*cy)` | `(i/cx)*stride` `:88-89` |
| 一致性 | ✅ | ✅ | ✅ | ✅（`i/cx == py + pz*cy` 在 `py<cy` 时恒成立） |

→ 三种打包方式自洽，图集布局没有错位。**这点可以让作者放心**：图集寻址是对的，问题只在"没人用它着色"。

#### ⑤ 探针分类 active/inactive — 🔴 完全未实现

| 论文要素 | 本工程 |
|---|---|
| 探针分类 pass（判断哪些探针在几何体内部/无效） | ❌ 无。`App.cpp:360-364` 的 pass 列表里只有 `GBuffer → ComputeSmoke → ProbeUpdate → ProbeBorderFixup → Lighting` |
| probe state buffer（active/inactive 标记） | ❌ 无。`ProbeTypes.h` 全部字段见 §4.1，无任何 state 字段 |
| inactive 探针跳过更新 | ❌ 无（`ProbeUpdateCS` 对每个探针无条件打 64 条射线） |
| inactive 探针在采样时被剔除/降权 | ❌ 无（因为没有采样） |

> 影响：探针长在几何体内部时会采到"体内"的辐亮度，通过插值泄漏到表面上，表现为大面积漏光。这是 DDGI 最典型的伪影，而当前代码对它**零防护**。

#### ⑥ 背面 / 无效方向的回退 — 🔴 完全未实现

**论文侧**：命中背面（`dot(hitNormal, rayDir) > 0`）时该方向的辐亮度不可信，应回退到上一帧值而不是写入错误数据。

**本工程侧的两个"像但不对"的地方**：

| 位置 | 代码 | 为什么不是论文要的回退 |
|---|---|---|
| `ShadersCommon.hlsl:344-345` | `if (dot(hitN, dir) > 0.0f) hitN = -hitN;` | 这是**翻转法线**让后续着色不出现负值，不是"检测到背面就丢弃本帧样本、沿用历史"。翻转后仍然照常写入 avg，错误样本照进 EMA |
| `ShadersProbe.hlsl:150-154` | gutter 邻域复制 | 这是图集**空间**上的补齐，不是八面体**方向域**上的接缝重建。八面体展开后，tile 边界两侧的方向在该映射下是"相邻但不等价"的，正确做法是把邻居 texel 反解回方向、再按当前 texel 的方向重新投影（或直接复用八面体 wrap 寻址），而不是简单 ±1 复制 |

**八面体接缝缺失的具体后果**：八面体的四条对角折痕处（`|u|+|v|=1` 附近），相邻 texel 对应的方向跨度并不均匀；线性复制会让这些折痕处的插值结果在**跨折痕方向**上产生不连续 → 条形/菱形伪影。

#### ⑦ 时间累积 hysteresis — 🟡 有骨架，但语义被削弱

```hlsl
// ShadersProbe.hlsl:52-54
    float4 old = atlas[pixel];
    float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;    // :53  固定 α
    atlas[pixel] = float4(lerp(old.rgb, radiance, alpha), 1.0f);  // :54
```

| 检查项 | 结论 |
|---|---|
| 首帧全量替换 | ✅ `frameIndex == 0` → `α=1` |
| 后续 EMA | ✅ `α=0.05` ≈ 20 帧滑动平均 |
| α 是否自适应（随 rayCount / 收敛速度） | ❌ 固定常数。论文的经验是"射线越多 → α 越大 → 收敛越快"；这里 `rayCount` 硬写 64（`ProbeUpdatePass.cpp:40`）且 shader 不读 |
| 是否 per-probe 独立的首帧判断 | ❌ `frameIndex` 是**全局单调帧号**（`ProbeUpdatePass.cpp:38 m_frameIndex++`），只在程序第一帧为 0。探针网格是静态的，所以暂时不出问题；一旦将来做动态探针/relocation，新激活探针会立刻从 α=0.05 开始混合"内存里的垃圾" |
| 距离矩侧是否有同样逻辑 | 🟡 写了（`ShadersProbe.hlsl:98`），但在死代码里，且 `frameIndex` 未定义（H8） |
| **实际效果是否等价于论文** | ❌ **不等价**，见下 |

**最关键的一点**：hysteresis 的意义是"把每帧的**不同随机样本**平均掉"。但本工程每个 texel 每帧打的是**同一条**射线（`OctDecode((texel+0.5)/8)` 是纯函数，`ShadersProbe.hlsl:38-39`，无任何随机扰动），所以 EMA 累积的是**同一个确定性值**：

$$\text{avg}_{n} = \alpha \sum_{i}(1-\alpha)^{n-i} L = L \cdot (1-(1-\alpha)^n) \to L$$

即"第 1 帧填满后，之后的所有帧都在原地空转"——**时间累积退化成了纯粹的算力浪费**，且完全不产生论文想要的去 banding 效果。

#### ⑧ 随机旋转方向避免 banding — 🔴 完全未实现（工具已备好）

**已存在的工具函数（`ShadersCommon.hlsl`）**：

| 函数 | 行 | 作用 |
|---|---|---|
| `FibonacciSphere(i, n, rot)` | `:121-131` | 斐波那契球面均匀采样，`rot.x` 控制方位相位 |
| `RotateY(v, a)` | `:134-139` | 绕 Y 旋 |
| `RotateX(v, a)` | `:142-147` | 绕 X 旋 |
| `StochasticFibonacci(i, n, rot)` | `:150-156` | 固定螺旋 + 随机旋转（论文的 stochastically-rotated Fibonacci spiral） |
| `Hash01` / `R2Sequence` / `QuasiRandom2D` | `:76-104` | 随机/低差异序列 |

**调用者数量：0。** 全文检索 `FibonacciSphere|StochasticFibonacci` 只命中定义处（`:121`、`:150`、`:152`）。

`ProbeUpdateCS` 实际用的是：

```hlsl
// ShadersProbe.hlsl:38-42
    float2 uv = (float2(texel) + 0.5f) / 8.0f;      // :38  固定 texel 中心
    float3 dir = OctDecode(uv);                     // :39  纯函数，无扰动
    float3 radiance = TraceRadiance(probePos + dir * 0.02f, dir);  // :42
```

→ **每个 texel 永远打同一条射线，跨帧完全重复。** 论文的做法是每帧对每个探针用旋转过的斐波那契螺旋打 N 条射线，让样本在时间上抖动、由 hysteresis 收敛，从而把 banding 化成噪声再抹平。本工程的做法必然产生**锁死的结构化 banding**（尤其探针间距与表面细节成拍频时）。

补充：`ProbeTypes.h:12` 的 `rayCount` 注释写着"预留（Fibonacci 射线数）"，`ProbeUpdatePass.cpp:40` 也确实填了 64——**说明作者本来计划的就是这套方案，只是没接上**。CPU 侧填了、shader 侧不读，是这个半成品的典型特征。

### 6.3 差距汇总：按"改动量 / 收益"排序

| 优先级 | 事项 | 涉及文件 | 依赖 | 预估收益 |
|---|---|---|---|---|
| P0 | 修 7 条 MSVC 错误（C1~C7） | `ProbeBorderFixupPass.h` / `App.cpp` | 无 | 恢复可构建 |
| P0 | 修 11 条 DXC 错误（H1~H11），或把 `ProbeDistanceCS` 从 `ShadersProbe.hlsl` 摘出去 | `ShadersProbe.hlsl` | 无 | 恢复可启动 |
| P1 | 把探针采样真正接进 `LightingPS` 的着色路径 | `ShadersLighting.hlsl` | P0 | **画面首次出现 DDGI 效果** |
| P1 | 实现三线性（或四面体）+ 每探针 octahedral 双线性采样 | `ShadersLighting.hlsl` + `ShadersCommon.hlsl` | P1 | 消除块状感 |
| P2 | 辐照度改为真正 irradiance（cos 加权 + 立体角 + 1/π） | `ShadersProbe.hlsl:42-54` | P0 | 物理正确，避免亮度偏差 |
| P2 | 接上 `StochasticFibonacci` + 多帧抖动 | `ShadersProbe.hlsl:38-42` | P0 | 消除 banding |
| P2 | 距离矩整条链路（pass / PSO / 注册 / dispatch / stride 18 修补） | 新建 `ProbeDistancePass.cpp` + `ProbeTypes.h` 填充 | P0 | Chebyshev 的前提 |
| P3 | Chebyshev 可见性权重 + 背面权重 | `ShadersLighting.hlsl` | P2 | 消除漏光 |
| P3 | 探针分类 active/inactive | 新 pass + 新 buffer | P2 | 消除体内探针漏光 |
| P4 | 八面体接缝重建（替代 ±1 复制） | `ShadersProbe.hlsl:150-154` | P0 | 消除折痕伪影 |

---

## 7. 其它风险（非编译错误，但会在改造中咬人）

### 7.1 `RenderTexture::m_state` 与 `RenderGraph` 状态双账本（高危隐患）

| 事实 | 位置 |
|---|---|
| `RenderGraph::Execute` 只更新自己的 `entry.state` | `RenderGraph.cpp:102-108` |
| `RenderTexture::TransitionTo` 用 `m_state` 当 `StateBefore` | `RenderTexture.cpp:58-66` |
| 两者互不同步 | 无任何回写代码 |
| 探针图集在初始化时是 `UNORDERED_ACCESS` | `RenderTexture.cpp:93-95` |
| `LightingPass` 把它声明为 `PIXEL_SHADER_RESOURCE` | `LightingPass.cpp:49` |

**后果**：`m_atlas.m_state` 永远停在 `UNORDERED_ACCESS`，而 GPU 侧的真实状态每帧在 `UNORDERED_ACCESS` ↔ `PIXEL_SHADER_RESOURCE` 之间来回。当前因为没人对探针 atlas 调 `TransitionTo`，所以不炸；**但一旦有人想"手动补个 barrier"而调了 `TransitionTo`，就会用错的 `StateBefore` 触发 D3D12 校验错误或丢帧。**

**建议**（不改代码，仅告知）：探针图集的状态迁移一律走 RenderGraph 的 `ReadWrite`/`Read` 声明，**永远不要**调 `ProbeGrid::m_atlas.TransitionTo`。

### 7.2 UAV barrier 的实际覆盖范围

| pass | barrier | 位置 | 评价 |
|---|---|---|---|
| `ProbeUpdatePass` | UAV barrier on `m_probeGrid->GetResource()` | `ProbeUpdatePass.cpp:61-64` | ✅ 正确，保证 `BorderFixupCS` 能读到 `ProbeUpdateCS` 刚写的内部 texel |
| `ProbeBorderFixupPass` | UAV barrier on `m_atlasResource` | `ProbeBorderFixupPass.cpp:46-49` | ✅ 正确，保证后续 `LightingPass` 的 SRV 读取看到 gutter |
| 两 pass 之间 | 无状态转换 barrier（都是 `UNORDERED_ACCESS`） | `RenderGraph.cpp:103` 因 `state` 相同而跳过 | ✅ 合理，UAV barrier 已足够 |
| `LightingPass` 前 | `UNORDERED_ACCESS → PIXEL_SHADER_RESOURCE` | `RenderGraph.cpp:106` | ✅ 隐式包含 UAV 刷写 |

结论：**同步这一块目前是对的**，改造时保持"每个写探针图集的 pass 结尾打一个 UAV barrier"即可。

### 7.3 越权写风险：`distanceUav` 默认 0

`ProbeUpdatePass.cpp:30` 用 `ProbeUpdateParams p{}` 值初始化 → 未赋值字段为 0。`distanceUav` 从没被赋值（§4.3）。

若将来按"最省事的办法"把 `ProbeDistanceCS` 接上（直接复用 `m_probeRootSignature`，不填 `distanceUav`），shader 里 `ResourceDescriptorHeap[g_Probe.distanceUav]` 就会索引**描述符堆第 0 槽**。当前第 0 槽是第一个被分配的 bindless 描述符（按 `RenderTexture::Init` 顺序，是 G-Buffer albedo 的 SRV），`RWTexture2D` 会把它当 UAV 解释 → **静默写坏别人的资源**，且不报错。

### 7.4 语义污染：`borderTileStride` 一字段两义

| 用途 | 位置 |
|---|---|
| CPU 语义："本次 fixup 的目标 atlas stride" | `ProbeTypes.h:21` 注释 `// 10（irradiance）或 18（distance）` |
| `ProbeDistanceCS` 借用为"距离矩 atlas 的 stride" | `ShadersProbe.hlsl:66` |
| 随后被局部 `const uint stride = 18` 覆盖 | `ShadersProbe.hlsl:89`（H6） |

→ 一个字段承担两个 pass 的 stride，且其中一处在函数中途被覆盖。改造时建议**拆成两个字段**（`irradianceStride` / `distanceStride`），否则 `stride` 到底指谁永远说不清。

### 7.5 `ProbeGrid::Init` 缺少失败处理

```cpp
// ProbeGrid.cpp:3-14
void ProbeGrid::Init(ID3D12Device* device, GPUDescriptorAllocator& bindless, const Desc& desc)
{
    m_desc = desc;
    m_atlas.InitUAV(...);          // 失败时 InitUAV 内部 THROW_IF_FAILED，OK
    m_distanceAtlas.InitUAV(...);
}
```

`InitUAV` 内部有 `THROW_IF_FAILED`（`RenderTexture.cpp:91-94`），所以失败会抛异常——这点是好的。但：
- 没有 `device` 空指针检查（工程风格上其它地方也没查，属一致性范围内，可不管）；
- 两次 `InitUAV` 各消耗 **2 个** bindless 描述符（SRV+UAV），共 4 个。距离矩那两个是**纯浪费**（§6.2②），在 10000 容量（`BindlessDescriptorHeap.h:24`）下不算问题，但属于"为不存在的功能付费"。

### 7.6 常量缓冲注释错误（文档债）

见 §4.4。三处注释 `32 B` / `48 B` / `12 → 16` 均与实际的 64 B 不符。

### 7.7 `#include "Pass.h"` 大小写

| 位置 | 写法 | 磁盘实际 |
|---|---|---|
| `ProbeUpdatePass.h:1` | `#include "Pass.h"` | `pass.h` |
| `ProbeBorderFixupPass.h:2` | `#include "Pass.h"` | `pass.h` |
| `RenderGraph.cpp:2` | `#include "Pass.h"` | `pass.h` |

NTFS 大小写不敏感 → 当前不报错。但这是**跨平台/远程构建/大小写敏感挂载**的定时炸弹，且属于零成本的修正项。

---

## 8. 未确认事项（诚实标注，不猜测）

| # | 事项 | 为什么无法确认 |
|---|---|---|
| U1 | 论文正文的章节号/公式编号 | 只抓取到 JCGT 摘要页，未取得论文正文全文；本报告对"论文要求"的表述保持在摘要可支撑的粒度 |
| U2 | 作者是否以 Unity HDRP 的 DDGI 变体为对标 | 影响 §6.3b 的定性。代码里没有 relocation 相关痕迹，但也没有任何说明文档 |
| U3 | `ProbeDistanceCS` 的预期 dispatch 形态 | 注释（`:57`"每个 group = 一个探针"）与代码（`:71` 图集坐标边界检查）互相矛盾，无法判定作者本意。§5.2 给出的两种模型都是"从代码反推"，非作者声明 |
| U4 | 修复后画面是否正确 | 需要实际编译 + 运行 + RenderDoc/PIX 抓帧才能验证。本次为静态审计 + 编译诊断，**未运行程序** |
| U5 | `m_probeGrid.GetUAVIndex()` 返回的槽位在运行时是否有效 | 逻辑上 `RenderTexture::InitUAV` 一定分配了（`RenderTexture.cpp:108-109`），但槽位值取决于 `GPUDescriptorAllocator` 的分配顺序，静态不可知；不构成问题 |
| U6 | 距离矩一旦接线后的数值范围 | `tMax=100`（`ShadersProbe.hlsl:86`）配 RGBA16F：`mean2 = t²` 最大 10000，`half` 最大值 65504，**勉强不溢出**；但 `half` 在 10000 附近的相对精度约 0.02%，moment 的方差计算 `mean2 - mean²` 会有精度损失。需要运行时验证，静态无法定论 |

---

## 9. 附：本次审计的产物清单

| 类别 | 内容 |
|---|---|
| 修改的源码文件 | **无**（严格只读） |
| 新建的文件 | 仅本报告 `docs/ddgi/01-probe-core-audit.md` |
| 编译产物位置 | 全部输出到 `%TEMP%\ddgi-audit\`，**未污染 `x64/` 与源码目录** |
| 实测覆盖 | 27 个 `.cpp` 全部单独编译；`ShadersProbe.hlsl` 的 3 个入口 × 2 个 DXC 版本 |

---

*报告结束。所有 `file:line` 均基于审计时的磁盘内容；行号与 `dxc` / `cl` 的原始诊断输出已交叉校验一致。*
