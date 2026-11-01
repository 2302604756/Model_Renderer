# DDGI 帧内集成审计（只读）

- 审计对象：`D:\C++files\RenderingProgrameByHand`
- 审计范围：探针系统与主渲染流程的接线现状（pass 顺序 / RenderGraph 语义 / FrameContext / 根签名 / Lighting 消费 / RenderTexture 能力 / 资源状态与跨帧累积）
- 审计方式：**纯静态阅读源码**。未执行构建、未运行程序（原因见 §9）。
- 所有结论均标注 `file:line`；凡是推断而非直接读到的内容，均显式标注「推断」。

---

## 0. 结论摘要

| # | 问题 | 结论 | 关键证据 |
|---|---|---|---|
| 1 | 探针 pass 是否接进 RenderGraph | **接进了。已实例化 + 已 AddPass，位置在 LightingPass 之前。** 但整条链路目前跑不起来（见 #4） | `App.cpp:200-216`（Init）、`App.cpp:362-363`（AddPass） |
| 2 | FrameContext 是否有 `lightCount` / `lightingConstantsAddress` / `lightBufferAddress` | **三个字段都真实存在**，且 App 每帧都在填。`ProbeUpdatePass.cpp` 用到的 4 个 ctx 字段全部存在，**没有悬空字段** | `RenderGraph.h:20-23`、`App.cpp:350-353` |
| 3 | 根签名槽位是否对得上 | **完全对得上**。探针根签名 5 个槽位，`ProbeUpdatePass` 逐个 `SetComputeRoot*` 0..4 与声明顺序一一匹配 | `App.cpp:185-191` vs `ProbeUpdatePass.cpp:49-55` |
| 4 | 最致命的接线断裂点 | **两层，逐层递进**：① C++ 编译期：`App.cpp:214-216` 与 `ProbeBorderFixupPass::Init` 的 11 参签名不匹配 + 头文件缺 `m_countX/Y/Z`；② 运行时 DXC：`ShadersProbe.hlsl` 的 `BorderFixupCS`（以及同文件的 `ProbeDistanceCS`）引用了 `ProbeUpdateParams` 里不存在的字段 | `App.cpp:214-216`、`ProbeBorderFixupPass.h:10-18`、`ProbeBorderFixupPass.cpp:18`、`ShadersProbe.hlsl:112,131` |
| 5 | Lighting 是否消费探针 | **不消费（对最终画面零影响）**。只在 debugMode 8（探针辐照度）和 9（atlas 布局）里读 atlas；`debugMode==0` 的最终着色走的是逐像素 ray query `TraceGI`，与探针无关 | `ShadersLighting.hlsl:73-114` vs `ShadersLighting.hlsl:116-135` |
| 6 | CPU→GPU 的探针采样常量是否已铺好 | **已经铺好了**。origin/spacing/counts/SRV 索引经 `LightingParams` 全程送达 shader，缺的只是「最终着色里用它」这一步 | `App.cpp:249-254` → `LightingPass.cpp:71-75` → `ShadersCommon.hlsl:243-244` |
| 7 | 跨帧累积会不会被 TransientHeap 破坏 | **不会**。探针 atlas 是 `CreateCommittedResource` 提交式资源，不进 TransientHeap；且全工程从未调用过 `TransientHeap::Reset()` | `RenderTexture.cpp:91-94`、`TransientHeap.cpp:37`（仅定义，无调用点） |
| 8 | 距离矩 atlas | **完全孤儿**：创建了、分配了 SRV/UAV，但既没进 RenderGraph、没有被任何 pass 写、App 也从没把它的 UAV 索引传出去 | `ProbeGrid.cpp:10-13`、`ProbeGrid.h:38-42` |

---

## 1. 当前单帧 pass 顺序（以 `App.cpp` 实际代码为准）

### 1.1 初始化期的接线（`App::init`）

| 行号 | 动作 | 说明 |
|---|---|---|
| `App.cpp:154-156` | 建 `m_computeRootSignature` | **只有 1 个根参数**：`b0`，8 DWORD。这是「冒烟 compute」专用的 |
| `App.cpp:167` | `m_computeTexture.InitUAV(...)` | compute 冒烟输出纹理 |
| `App.cpp:169-171` | 注册 `m_computeTextureId` → 初始态 `UNORDERED_ACCESS` | |
| `App.cpp:173-175` | `m_computeSmokePass.Init(...)` | |
| `App.cpp:179` | `m_probeGrid.Init(device, m_gpuAllocator, ProbeGrid::Desc{})` | 用默认 Desc：origin(-2.2,0.3,-2.2)、spacing 0.63、8×4×8 |
| `App.cpp:180-182` | 注册 `m_probeGridId` → 初始态 `UNORDERED_ACCESS` | 注册的是**辐照度 atlas**（`ProbeGrid::GetResource()` → `m_atlas`） |
| `App.cpp:185-191` | 建 `m_probeRootSignature` | 5 个根参数，详见 §3 |
| `App.cpp:193-195` | 编译 `ProbeUpdateCS` | |
| `App.cpp:197-198` | 建 `m_probePSO` | |
| `App.cpp:200-204` | **`m_probeUpdatePass.Init(...)`** | 6 参，签名匹配 ✓ |
| `App.cpp:206-208` | 编译 `BorderFixupCS` | ← 运行时会在这里抛异常，见 §7 |
| `App.cpp:210-212` | 建 `m_probeBorderPSO` | |
| `App.cpp:214-216` | **`m_probeBorderFixupPass.Init(...)`** | ← **4 参调用 11 参函数，编译错误**，见 §7 |
| `App.cpp:220-234` | 注册 GBuffer / Depth / Backbuffer | backbuffer 按 3 帧各注册一个 ResourceId |
| `App.cpp:237-240` | `m_gbufferPass.Init(...)` | |
| `App.cpp:242-254` | `m_lightingPass.Init(...)` | **传入探针参数**：`m_probeGridId`、`origin`、`spacing`、`countX/Y/Z`、`GetSRVIndex()` |

探针相关成员在 `App.h:117-123`：
```cpp
117: 	ProbeGrid        m_probeGrid;
118: 	ProbeUpdatePass  m_probeUpdatePass;
119: 	ComPtr<ID3D12RootSignature> m_probeRootSignature;
120: 	ComPtr<ID3D12PipelineState> m_probePSO;
121: 	RenderGraph::ResourceId     m_probeGridId = RenderGraph::kInvalidId;
122: 	ProbeBorderFixupPass m_probeBorderFixupPass;
123: 	ComPtr<ID3D12PipelineState> m_probeBorderPSO;
```

### 1.2 每帧的 pass 顺序（`App::RenderTick`）

`App.cpp:359-375` 是唯一决定顺序的地方：

```cpp
359: 	m_graph.BeginFrame();
360: 	m_graph.AddPass(m_gbufferPass);
361: 	m_graph.AddPass(m_computeSmokePass);
362: 	m_graph.AddPass(m_probeUpdatePass);  
363: 	m_graph.AddPass(m_probeBorderFixupPass);
364: 	m_graph.AddPass(m_lightingPass);
365: 	
366: 
367: 	// 收尾：backbuffer 切回 PRESENT（空执行，只用于插屏障）
368: 	const RenderGraph::ResourceId backbufferId = m_backbufferIds[ctx.frameIndex];
369: 	m_graph.AddPass("BackbufferToPresent",
370: 		[backbufferId](RenderGraph::PassBuilder& builder) {
371: 			builder.Transition(backbufferId, D3D12_RESOURCE_STATE_PRESENT);
372: 		},
373: 		[](RenderGraph::FrameContext&) {});
374: 
375: 	m_graph.Execute(ctx);
```

执行序（`RenderGraph::Execute` 严格按 `m_passes` 插入顺序执行，见 `RenderGraph.cpp:87`）：

| 序 | Pass | 声明（Declare） | 实际资源操作 | 行号 |
|---|---|---|---|---|
| 1 | `GBuffer` | Write albedo/normal = RENDER_TARGET；Write depth = DEPTH_WRITE | 写 2 RTV + DSV | `GBufferPass.cpp:27-32` |
| 2 | `ComputeSmoke` | ReadWrite computeTex = UNORDERED_ACCESS | 写 compute 纹理 + UAV barrier | `ComputeSmokePass.cpp:18-21, 46-49` |
| 3 | **`ProbeUpdate`** | ReadWrite probeAtlas = UNORDERED_ACCESS | 写辐照度 atlas + UAV barrier | `ProbeUpdatePass.cpp:20-23, 61-64` |
| 4 | **`ProbeBorderFixup`** | ReadWrite probeAtlas = UNORDERED_ACCESS | 写 atlas gutter + UAV barrier | `ProbeBorderFixupPass.cpp:25-28, 46-49` |
| 5 | `Lighting` | Read albedo/normal/depth = PIXEL_SHADER_RESOURCE；Write backbuffer = RENDER_TARGET；**Read probeAtlas = PIXEL_SHADER_RESOURCE** | 全屏三角形 | `LightingPass.cpp:43-50` |
| 6 | `BackbufferToPresent` | Transition backbuffer → PRESENT | 空 | `App.cpp:369-373` |

**结论：`ProbeUpdatePass` 与 `ProbeBorderFixupPass` 不是「已定义但未接线」，而是「已接线但跑不起来」。** 两者都已实例化（`App.h:118,122`）、已 `Init`（`App.cpp:200-216`）、已 `AddPass`（`App.cpp:362-363`），且顺序正确地排在 `LightingPass` **之前**，所以「同帧产出、同帧消费」的时序前提是满足的。

> 注意：`App.cpp:362-363` 是**无条件** AddPass，没有「每隔 N 帧更新探针」之类的节流，也没有 `enableDDGI` 开关。

---

## 2. RenderGraph 的真实语义

### 2.1 `PassBuilder` 到底支持什么

`RenderGraph.h:31-46`：

```cpp
33:         PassBuilder& Read(ResourceId id,
34:             D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
35:         PassBuilder& Write(ResourceId id,
36:             D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET);
37:         PassBuilder& ReadWrite(ResourceId id,
38:             D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
39:         PassBuilder& Transition(ResourceId id, D3D12_RESOURCE_STATES state); // 只切状态，不读写
```

**关键事实：`Read` / `Write` / `ReadWrite` / `Transition` 四个函数体完全一样**，都只是 `m_usages.push_back({id, state})`（`RenderGraph.cpp:4-26`）。三者之间**没有任何语义差别**——既不区分读写、不推导依赖、不做 hazard 分析。「读写意图」纯粹是写给开发者看的注释。

真正生效的只有 `state` 一个字段（`PassBuilder::Usage{id, state}`，`RenderGraph.h:41`）。

### 2.2 是否真的插入 barrier

**是，但只插入 transition barrier，且只在状态真的变化时插入。**

`RenderGraph.cpp:85-113`：

```cpp
 87:     for (auto& pass : m_passes)
 88:     {
 89:         // 同一 pass 里同一资源声明多次时，以最后一次为准
 90:         for (size_t i = 0; i < pass.usages.size(); ++i)
 91:         {
 92:             const auto& usage = pass.usages[i];
 94:             bool overridden = false;
 95:             for (size_t j = i + 1; j < pass.usages.size(); ++j)
 96:             {
 97:                 if (pass.usages[j].id == usage.id) { overridden = true; break; }
 98:             }
 99:             if (overridden) continue;
101:             //能进行到下面一步说明是最后一次声明
102:             ResourceEntry& entry = m_resources[usage.id];
103:             if (entry.state != usage.state)
104:             {
106:                 Transition(ctx.cmdList, entry.resource, entry.state, usage.state);
107:                 entry.state = usage.state;
108:             }
109:         }
111:         pass.execute(ctx);
112:     }
```

由此可确认 RenderGraph 的真实能力边界：

| 能力 | 是否具备 | 说明 |
|---|---|---|
| 状态追踪 | ✅ | `m_resources[].state`，跨帧保留（`BeginFrame` 只清 pass，`RenderGraph.cpp:50-53`） |
| Transition barrier | ✅ | `RenderGraph.cpp:102-108`，状态相同则跳过（`RenderGraph.cpp:31` 有 `if (before == after) return;`） |
| UAV barrier 自动插入 | ❌ | 图完全不产生 UAV barrier，全部由各 pass 手工发（`ProbeUpdatePass.cpp:61-64`、`ProbeBorderFixupPass.cpp:46-49`、`ComputeSmokePass.cpp:46-49`） |
| Pass 重排序 / 拓扑排序 | ❌ | 严格按 `AddPass` 顺序，`RenderGraph.cpp:87` 一个 for 循环 |
| Pass 剔除 | ❌ | 所有 pass 必执行 |
| 别名 / 内存复用 | ❌ | `D3D12_RESOURCE_BARRIER_TYPE_ALIASING` 未使用 |
| 资源生命周期管理 | ❌ | 只存裸 `ID3D12Resource*`，不持有引用（`RenderGraph.h:69`） |
| 多队列 / 异步 compute | ❌ | 单 DIRECT 队列 |
| 名字查找 / 重复注册检查 | ❌ | `RegisterResource` 直接 push_back 返回下标（`RenderGraph.cpp:42-47`），`name` 只用于调试 |
| 非法 ResourceId 校验 | ❌ | `m_resources[usage.id]` 无 bounds check（`RenderGraph.cpp:102`）；传入 `kInvalidId`(=UINT32_MAX) 会直接越界 |

**一句话：这个 RenderGraph 现在是一个「按插入顺序逐 pass 刷 transition barrier 的 barrier 注入器」，不是自动依赖调度器。**

### 2.3 ResourceId 如何注册 atlas 资源

`App.cpp:180-182`：

```cpp
180: 	m_probeGridId = m_graph.RegisterResource(
181: 		"ProbeGrid", m_probeGrid.GetResource(),
182: 		D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
```

- `ProbeGrid::GetResource()` → `m_atlas.GetResource()`（`ProbeGrid.h:29`），**只注册辐照度 atlas**。
- 初始状态填 `UNORDERED_ACCESS`，与 `RenderTexture::InitUAV` 创建时给的状态一致（`RenderTexture.cpp:91-95`），所以首帧不会产生一次多余的 barrier。
- **距离矩 atlas（`m_distanceAtlas`）从未注册到 graph**（`ProbeGrid.cpp:11-13` 创建后无任何引用）。

### 2.4 FrameContext 的实际字段

`RenderGraph.h:16-26`：

```cpp
16:     struct FrameContext {
17:         uint32_t                    frameIndex = 0;
18:         ID3D12GraphicsCommandList*  cmdList = nullptr;
20:         D3D12_GPU_VIRTUAL_ADDRESS   frameConstantsAddress = 0;    // b1
21:         D3D12_GPU_VIRTUAL_ADDRESS   lightingConstantsAddress = 0; // b3
22:         D3D12_GPU_VIRTUAL_ADDRESS   lightBufferAddress = 0;       // t5
23:         uint32_t                    lightCount = 0;
24:         uint32_t                    debugMode = 0;
25:         D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv{};
26:     };
```

`App.cpp:347-356` 每帧填充：

```cpp
347: 	RenderGraph::FrameContext ctx{};
348: 	ctx.frameIndex = m_context.GetFrameIndex();
349: 	ctx.cmdList = cmdList;
350: 	ctx.frameConstantsAddress = cbvGpuAddr;
351: 	ctx.lightingConstantsAddress = lightingCbAddr;
352: 	ctx.lightBufferAddress = lightBlock.gpuAddress;
353: 	ctx.lightCount = static_cast<uint32_t>(m_gpuLights.size());
354: 	ctx.debugMode = m_debugMode;
355: 	ctx.backbufferRtv = rtvHandle;
```

### 2.5 ProbeUpdatePass 用到的 ctx 字段逐个核对

`ProbeUpdatePass.cpp:25-55` 用到的 ctx 字段：

| `ProbeUpdatePass.cpp` 用法 | 行号 | `FrameContext` 中是否存在 | 结论 |
|---|---|---|---|
| `ctx.cmdList` | 27 | ✅ `RenderGraph.h:18` | 存在 |
| `ctx.lightCount` | 47 | ✅ `RenderGraph.h:23` | 存在 |
| `ctx.lightingConstantsAddress` | 54 | ✅ `RenderGraph.h:21` | 存在 |
| `ctx.lightBufferAddress` | 55 | ✅ `RenderGraph.h:22` | 存在 |

**没有任何悬空字段。** 任务预设的「若 ProbeUpdatePass 用到的字段在 RenderGraph.h 里找不到，那是关键发现」这一情况**不成立**——`FrameContext` 与 `ProbeUpdatePass` 之间是自洽的。

> 但有一个**设计层面**的问题值得记下：`FrameContext` 里**没有**「探针 atlas 的 UAV/SRV 索引」这样的字段。`ProbeUpdatePass` 是通过持有 `ProbeGrid*` 成员自行取（`ProbeUpdatePass.cpp:42` `m_probeGrid->GetUAVIndex()`）绕过去的，这与 `ComputeSmokePass` 持有 `RenderTexture*` 取 UAV 索引（`ComputeSmokePass.cpp:35`）是同一套模式，**工程内是自洽的**，不算断裂。

> 另一个隐患：`ctx.frameIndex` 是环形槽位号（`RenderContext::GetFrameIndex()`，0..2 循环，`RenderContext.cpp:159`），而 `PerFrameData::frameIndex` 用的是 `App::m_frameCounter++`（`App.cpp:319`，单调不回头）。`ProbeUpdatePass` 又用了自己私有的 `m_frameIndex`（`ProbeUpdatePass.cpp:38`）。**全工程存在 3 个互不相同的「帧号」**，做时序累积（探针 alpha 衰减、采样抖动）时必须确认用的是哪一个，否则会出现「每 3 帧回到 0」的 bug。

---

## 3. 根签名槽位现状

### 3.1 图形根签名 `m_rootSignature`（`App.cpp:106-114`）

```cpp
106: 	m_rootSignature = RootSignatureBuilder()
107: 		.Add32BitConstants(40, 0)    // Root Parameter [0] -> register(b0): Push Constants    索引
108: 		.AddRootCBV(1)               // Root Parameter [1] -> register(b1): 逐帧瞬态 CBV	  传入Constant Buffer
109: 		.Add32BitConstants(16, 2)    // [2] b2 lighting 参数（16 DWORD）
110: 		.AddRootDescriptorTable(3)   // [3] t3 TLAS
111: 		.AddRootCBV(3)               // [4] b3 光照常量
112: 		.AddRootSRV(5)               // [5] t5 光源数组
113: 		.AddDefaultStaticSamplers()  // Static Sampler    -> register(s0), register(s1)       sampler
114: 		.Build(device);
```

| 槽 | 类型 | 绑定 | 大小(DWORD) | 可见性 | 谁设置 |
|---|---|---|---|---|---|
| 0 | 32BIT_CONSTANTS | `b0` | 40 | ALL | `GBufferPass.cpp:68` |
| 1 | CBV | `b1` | 2 | ALL | `App.cpp:344`（**每帧只设一次**）+ `GBufferPass.cpp:50` |
| 2 | 32BIT_CONSTANTS | `b2` | 16 | ALL | `LightingPass.cpp:77` |
| 3 | DESCRIPTOR_TABLE | `t3` | 1 | ALL | `LightingPass.cpp:80` |
| 4 | CBV | `b3` | 2 | ALL | `LightingPass.cpp:78` |
| 5 | SRV | `t5` | 2 | ALL | `LightingPass.cpp:79` |
| — | 静态采样器 | `s0`(WRAP) / `s1`(CLAMP) | — | ALL | `RootSignature.cpp:24-42` |

> 根签名总 DWORD 预算：40+2+16+1+2+2 = **63**，紧贴 D3D12 的 64 上限。后续再加根参数会直接失败。

### 3.2 compute 根签名 `m_computeRootSignature`（`App.cpp:154-156`）

```cpp
154: 	m_computeRootSignature = RootSignatureBuilder()
155: 		.Add32BitConstants(8, 0)          // b0: ComputeParams
156: 		.Build(device, /*allowInputLayout*/ false);
```

| 槽 | 类型 | 绑定 | 大小(DWORD) |
|---|---|---|---|
| 0 | 32BIT_CONSTANTS | `b0` | 8 |

**这是「冒烟 compute」专用的、只有一个根参数的签名**，与探针无关。对应 shader 是 `ShadersCompute.hlsl:15` 的 `g_Compute : register(b0)`。

### 3.3 探针根签名 `m_probeRootSignature`（`App.cpp:185-191`）

```cpp
185: 	m_probeRootSignature = RootSignatureBuilder()
186: 		.Add32BitConstants(16, 4)   // b4: ProbeUpdateParams
187: 		.Add32BitConstants(16, 2)   // b2: LightingParams
188: 		.AddRootDescriptorTable(3)  // t3: TLAS
189: 		.AddRootCBV(3)              // b3: LightingConstants
190: 		.AddRootSRV(5)              // t5: GPULight[]
191: 		.Build(device, /*allowInputLayout*/ false);
```

`RootSignatureBuilder::Add32BitConstants(num32BitValues, shaderRegister, ...)`（`RootSignature.h:12`、`RootSignature.cpp:3-12`）——第一个参数是 DWORD 数，第二个是寄存器号。因此：

| 根参数槽 | 类型 | 绑定 | 大小(DWORD) | 对应 HLSL |
|---|---|---|---|---|
| 0 | 32BIT_CONSTANTS | `b4` | 16 | `g_Probe : register(b4)`（`ShadersProbe.hlsl:13`） |
| 1 | 32BIT_CONSTANTS | `b2` | 16 | `g_Lighting : register(b2)`（`ShadersCommon.hlsl:247`） |
| 2 | DESCRIPTOR_TABLE | `t3` | 1 | `g_TLAS : register(t3)`（`ShadersCommon.hlsl:61`） |
| 3 | CBV | `b3` | 2 | `LightingCB : register(b3)`（`ShadersCommon.hlsl:47`） |
| 4 | SRV | `t5` | 2 | `g_Lights : register(t5)`（`ShadersCommon.hlsl:258`） |

### 3.4 与 `SetComputeRoot32BitConstants(0..4)` 的逐项比对

`ProbeUpdatePass.cpp:49-55`：

```cpp
49:     cmdList->SetComputeRootSignature(m_rootSignature);
50:     cmdList->SetPipelineState(m_pso);
51:     cmdList->SetComputeRoot32BitConstants(0, 16, &p, 0);
52:     cmdList->SetComputeRoot32BitConstants(1, 16, &lp, 0);
53:     cmdList->SetComputeRootDescriptorTable(2, m_tlasDescriptor);
54:     cmdList->SetComputeRootConstantBufferView(3, ctx.lightingConstantsAddress);
55:     cmdList->SetComputeRootShaderResourceView(4, ctx.lightBufferAddress);
```

| 调用 | 槽 | 实际推到 shader 的东西 | 是否与 §3.3 一致 |
|---|---|---|---|
| `SetComputeRoot32BitConstants(0, 16, &p, 0)` | 0 | `ProbeUpdateParams`（64 B = 16 DWORD，`ProbeTypes.h:24` 有 static_assert）→ `b4` | ✅ |
| `SetComputeRoot32BitConstants(1, 16, &lp, 0)` | 1 | `LightingParams`（64 B = 16 DWORD，`LightingTypes.h:24`）→ `b2` | ✅ |
| `SetComputeRootDescriptorTable(2, m_tlasDescriptor)` | 2 | TLAS SRV 表 → `t3` | ✅ |
| `SetComputeRootConstantBufferView(3, ctx.lightingConstantsAddress)` | 3 | `LightingConstants` → `b3` | ✅ |
| `SetComputeRootShaderResourceView(4, ctx.lightBufferAddress)` | 4 | `GPULight[]` → `t5` | ✅ |

**结论：`ProbeUpdatePass` 的 5 个槽位全部对得上，且数量/字节数精确匹配。这是目前整条链路上最干净的一环。**

依赖闭环核对（`ProbeUpdateCS` → `TraceRadiance` → `EvaluateDirectLighting` / `SkyColor`）：

| shader 依赖 | 来源 | 是否已绑 |
|---|---|---|
| `g_Lighting.objectIndex`（`ShadersCommon.hlsl:321`） | `ProbeUpdatePass.cpp:46` `lp.objectIndex = m_objectDataBindlessIndex` | ✅ |
| `g_Lighting.lightCount`（`ShadersCommon.hlsl:265`） | `ProbeUpdatePass.cpp:47` | ✅ |
| `g_Lights[]`（`ShadersCommon.hlsl:267`） | 根参数 4 | ✅ |
| `skyColor`（`ShadersCommon.hlsl:192`） | 根参数 3 | ✅ |
| `g_TLAS`（`ShadersCommon.hlsl:315`） | 根参数 2 | ✅ |
| 描述符堆（`ResourceDescriptorHeap[...]`） | `App.cpp:335-336` 每帧 `SetDescriptorHeaps` 一次 | ✅（但**依赖 App 的全局调用，pass 自己没设**） |

### 3.5 `ProbeBorderFixupPass` 的根参数使用

`ProbeBorderFixupPass.cpp:41-44`：

```cpp
41:     cmdList->SetComputeRootSignature(m_rootSignature);
42:     cmdList->SetPipelineState(m_pso);
43:     cmdList->SetComputeRoot32BitConstants(0, 16, &p, 0);
44:     cmdList->Dispatch((m_atlasWidth + 7) / 8, (m_atlasHeight + 7) / 8, 1);
```

- 只设槽 0（`b4`），槽 1..4 不设。若 `BorderFixupCS` 最终只引用 `g_Probe`，D3D12 允许（未引用的根参数可以不设），Debug Layer 至多报提示。
- 但**当前 `BorderFixupCS` 引用了 `g_Probe.gridCountsAndFrame` 和 `g_Probe.misc.w`（`ShadersProbe.hlsl:112,131`），这两个字段在 `ProbeUpdateParams`（`ProbeTypes.h:5-23`）里根本不存在** —— 见 §7。
- 另一个隐患：`ProbeBorderFixupPass` 会把 `p{}` 里除 `countX/Y/Z/borderTargetUav/borderTileStride` 之外的字段全部写成 0（`ProbeBorderFixupPass.cpp:34-39`），其中 `irradianceUav`、`distanceUav`、`originX/Y/Z`、`spacing`、`frameIndex` 都是 0。目前 `BorderFixupCS` 不用这些，但一旦将来按 `borderTargetUav` 的语义扩展，要注意 0 号描述符是**有效槽位**（见 §5.3），绑错会是「把 SRV 当 UAV 用」的 GPU 崩溃。

---

## 4. Lighting 是否消费探针

### 4.1 事实：只在 debug 视图里消费

`ShadersLighting.hlsl` 中 `probe/Probe/atlas` 的**全部**出现位置（grep 结果）都落在 `debugMode == 8` 与 `debugMode == 9` 两个分支内：

```hlsl
 72:         // debug 8：探针 octahedral 图（按法线方向采样）
 73:     if (g_Lighting.debugMode == 8)
 74:     {
 75:         float3 origin = g_Lighting.gridOriginSpacing.xyz;
 76:         float3 spacing = g_Lighting.gridOriginSpacing.w;
 77:         uint3 counts = g_Lighting.gridCountsAndProbeIndex.xyz;
 78: 
 79:         int3 idx = (int3) round((world.xyz - origin) / spacing);
 80:         idx = clamp(idx, int3(0, 0, 0), int3(counts) - 1);
 81:         uint probeIndex = (idx.z * counts.y + idx.y) * counts.x + idx.x;
 82: 
 83:         float2 uv = OctEncode(N);
 84:         uint2 texel = uint2(min(uv * 8.0f, 7.0f));
 85:         //从8*8变成10*10添加一圈保护边
 86:         const uint stride = 10;
 87:         uint2 pixel = uint2(
 88:             (probeIndex % counts.x) * stride + 1 + texel.x,
 89:             (probeIndex / counts.x) * stride + 1 + texel.y);
 90: 
 91:         Texture2D<float4> atlas = ResourceDescriptorHeap[g_Lighting.gridCountsAndProbeIndex.w];
 92:         return float4(atlas.Load(int3(pixel, 0)).rgb, 1.0f);
 93:     }
```

而**最终着色路径**（`ShadersLighting.hlsl:116-135`）里没有任何探针引用：

```hlsl
116:     // ---- 纯光追 GI：每像素 2 条余弦半球射线 ----
117:     const uint giSamples = 2;
120:     for (uint i = 0; i < giSamples; ++i)
121:     {
122:         uint sampleIndex = g_Frame.frameIndex * giSamples + i;
123:         float2 u = QuasiRandom2D(pixel, sampleIndex);
124:         indirect += TraceGI(world.xyz, N, u);
125:     }
126:     indirect /= giSamples;
...
132:     // ---- final：直射 * 阴影 + 环境 + 间接 ----
133:     float3 direct = EvaluateDirectLighting(world.xyz, N);
134:     float3 color = albedo.rgb * (direct + ambient.rgb + indirect);
135:     return float4(color, 1.0f);
```

**结论：DDGI 探针对最终画面贡献为 0。** 现在按 `1..9` 键切到的 `debugMode 8/9` 是唯一能看到探针数据的入口，而且这两条路径**不依赖边框修正**（用的是 `atlas.Load()` 点采样，不是 `SampleLevel` 线性采样，`ShadersLighting.hlsl:92,108`），所以即使 `ProbeBorderFixupPass` 修好了，画面也不会有任何变化。

### 4.2 好消息：CPU→GPU 的采样常量已经全部铺好

最终着色要用的 4 组常量**已经在管线里了**，不需要新增根签名槽位：

| 常量 | C++ 定义 | 传递链 | HLSL 侧 |
|---|---|---|---|
| origin(3) + spacing(1) | `LightingTypes.h:18` `gridOriginSpacing` | `App.cpp:249-250` → `LightingPass.cpp:71` → `lp` | `ShadersCommon.hlsl:243` |
| counts(3) + SRV 索引(1) | `LightingTypes.h:19-22` | `App.cpp:251-254` → `LightingPass.cpp:72-75` → `lp` | `ShadersCommon.hlsl:244` `gridCountsAndProbeIndex` |

> ⚠ 命名漂移：C++ 侧叫 `gridCountX/Y/Z` + `probeSrvIndex` 四个独立 `uint32_t`（`LightingTypes.h:19-22`），HLSL 侧叫一个 `uint4 gridCountsAndProbeIndex`（`ShadersCommon.hlsl:244`）。**布局完全兼容**（都是 64 B 结构体里的第 48..63 字节，`static_assert` 在 `LightingTypes.h:24` 兜住了总大小），但**字段名不一致**，改的时候容易只改一边。
> 另外 `LightingTypes.h:5` 的注释「root constants b2（8 DWORD = 32 B）」和 `ShadersCommon.hlsl:232` 的注释「12 DWORD = 48 B」**都已经过期**（实际 16 DWORD / 64 B）。

### 4.3 需要在哪里、插什么

**插入位置**：`ShadersLighting.hlsl` 的 `LightingPS`，在 `world` 世界坐标重建之后（第 56 行 `if (g_Lighting.debugMode == 4)` 之后）、`giSamples` 循环之前（第 116 行之前），并把结果在 `ShadersLighting.hlsl:134` 处并入 `indirect`。

**映射公式**（必须与写入端 `ShadersProbe.hlsl:46-48` 完全一致，否则 tile 会错位）：

```
probeIndex = (idx.z * countY + idx.y) * countX + idx.x          // 与 ShadersProbe.hlsl:30-32 的 px/py/pz 分解互逆
idx        = clamp(floor((worldPos - origin) / spacing), 0, counts - 1)
atlasX     = (probeIndex % countX) * stride + 1 + texel.x        // stride = 10
atlasY     = (probeIndex / countX) * stride + 1 + texel.y        // (probeIndex/countX) == py + pz*countY
texel      = min(uint2(OctEncode(N) * 8.0), 7)
```

> 一致性已验证：写入端 `ShadersProbe.hlsl:46-48` 用 `(py + pz * counts.y) * stride`，读取端 `ShadersLighting.hlsl:89` 用 `(probeIndex / counts.x) * stride`；因为 `probeIndex / countX == py + pz*countY`，两式等价 ✅。`ShadersProbe.hlsl:30-32` 与 `ShadersLighting.hlsl:81` 的索引分解也互逆 ✅。

**最小改动形态**（示意，未落地）：

```hlsl
// 放在 world 重建之后
float3 irr = float3(0, 0, 0);
{
    uint3 counts = g_Lighting.gridCountsAndProbeIndex.xyz;
    float3 origin  = g_Lighting.gridOriginSpacing.xyz;
    float  spacing = g_Lighting.gridOriginSpacing.w;
    int3 idx = clamp((int3)floor((world.xyz - origin) / spacing), 0, (int3)counts - 1);
    uint  pIdx = (idx.z * counts.y + idx.y) * counts.x + idx.x;
    const uint stride = 10;
    uint2 texel = min((uint2)(OctEncode(N) * 8.0f), 7u);
    uint2 px = uint2((pIdx % counts.x) * stride + 1 + texel.x,
                     (pIdx / counts.x) * stride + 1 + texel.y);
    Texture2D<float4> atlas = ResourceDescriptorHeap[g_Lighting.gridCountsAndProbeIndex.w];
    irr = atlas.Load(int3(px, 0)).rgb;
}
```

**还需要注意的两点（不是接线问题，是算法层，这里只提示）**：
1. 当前 `ProbeUpdateCS` 累积的是**方向性辐射亮度**（`ShadersProbe.hlsl:42,54` 直接存 `radiance`），不是 DDGI 论文里的**余弦加权辐照度 + 可见性矩归一化**。直接拿它当 `indirect` 用会偏亮/偏色。这一点属于算法审计范围，本报告不展开。
2. 若将来想用 `SampleLevel` 做探针间三线性插值，需要 **8 个邻接探针** + 有效的 gutter。而当前 `BorderFixupCS` 的 gutter 只是「把本 tile 的边缘 texel 复制出去」（`ShadersProbe.hlsl:140-154`），并不处理 octahedral 接缝跨 tile 的方向翻转，所以跨 tile 线性采样仍会有缝。

---

## 5. RenderTexture 的能力边界

### 5.1 `InitUAV` 到底创建了什么

`RenderTexture.cpp:72-115`：

| 属性 | 值 | 行号 |
|---|---|---|
| Dimension | `TEXTURE2D` | 78 |
| Width/Height | 由参数传入（探针 = 80×320 / 144×576） | 79-80 |
| DepthOrArraySize | 1 | 81 |
| **MipLevels** | **1（无 mip 链）** | 82 |
| Format | 由参数传入（探针 = `R16G16B16A16_FLOAT`，`ProbeGrid.cpp:8`） | 83 |
| SampleDesc | {1, 0}（无 MSAA） | 84 |
| Layout | `TEXTURE_LAYOUT_UNKNOWN` | 85 |
| **Flags** | **仅 `ALLOW_UNORDERED_ACCESS`** | 86 |
| Heap | `DEFAULT`，`CreateCommittedResource`（**不是** placed/TransientHeap） | 88-94 |
| **初始状态** | **`D3D12_RESOURCE_STATE_UNORDERED_ACCESS`** | 93 |
| ClearValue | **`nullptr`（无清屏值）** | 93 |
| RTV | **不创建** | — |

### 5.2 是否被 bindless 同时注册 SRV 与 UAV

**是，两个都注册了。**

```cpp
 98:     GPUDescriptorHandle srv = bindlessAllocator.Allocate();
 99:     m_bindlessIndex = srv.index;          // → GetBindlessIndex()
105:     device->CreateShaderResourceView(m_resource.Get(), &srvDesc, srv.cpuHandle);
108:     GPUDescriptorHandle uav = bindlessAllocator.Allocate();
109:     m_uavBindlessIndex = uav.index;       // → GetUAVBindlessIndex()
114:     device->CreateUnorderedAccessView(m_resource.Get(), nullptr, &uavDesc, uav.cpuHandle);
```

- SRV：`Texture2D`，MipLevels=1（`RenderTexture.cpp:100-105`）→ 供 Lighting 的 `atlas.Load()` 使用。
- UAV：`TEXTURE2D`，MipSlice=0（`RenderTexture.cpp:110-114`）→ 供 compute 的 `RWTexture2D` 使用。
- 两者在**同一个 bindless 堆**里、**不同槽位**，因此 `ResourceDescriptorHeap[idx]` 可以用同一张资源以两种方式访问。这正是探针 pass 的 `irradianceUav`（`ProbeUpdatePass.cpp:42`）与 Lighting 的 `probeSrvIndex`（`App.cpp:254`）能共存的原因 ✅。

### 5.3 描述符槽位的一个陷阱（与 §3.5 呼应）

`GPUDescriptorAllocator::Init` 的 freelist 是「反序压栈」（`BindlessDescriptorHeap.cpp:11-13` push `capacity-i-1`），`Allocate` 取 `back()`（`BindlessDescriptorHeap.cpp:24`），所以**下标从 0 开始按升序分配**。

按 `App::init` 的执行顺序，第一个拿到 bindless 槽位的是 `m_gbufferAlbedo.Init`（`App.cpp:36-37` → `RenderTexture.cpp:44-45`），因此 **0 号槽是 GBuffer albedo 的 SRV，不是任何 UAV**。依次是 normal SRV=1、depth SRV=2（`App.cpp:45-53`）、……、探针 atlas 的 SRV/UAV 在更后面。

因此：
- `ProbeUpdatePass` 显式设置了 `irradianceUav`（`ProbeUpdatePass.cpp:42`，✅ 正确）；
- 但 `distanceUav` / `borderTargetUav` 在 `ProbeUpdatePass` 里保持 `{}` 零初始化（`ProbeUpdatePass.cpp:30-42`）→ 值 **0**；
- 若哪天有 shader 拿 `g_Probe.distanceUav` 去 `ResourceDescriptorHeap[...]` 当 UAV 用，就会**把 GBuffer albedo 的 SRV 描述符当 UAV 绑定**，典型后果是 device removed（`0x887A0005`）或写入被静默丢弃。
- `ProbeBorderFixupPass` 的 `borderTargetUav` 由 `Init` 传入（`ProbeBorderFixupPass.cpp:38`），只要调用点传的是 `GetUAVIndex()` 就不会踩到这个坑——但当前调用点（`App.cpp:214-216`）压根没传（见 §7.1.1）。

### 5.4 有没有状态追踪可供 RenderGraph 推导

**有，但是死代码。**

```cpp
// RenderTexture.h:29
    void TransitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES target);
// RenderTexture.h:34
    D3D12_RESOURCE_STATES GetState() const noexcept { return m_state; }
```

`RenderTexture::TransitionTo`（`RenderTexture.cpp:56-68`）**在全工程中没有任何调用点**（grep 仅命中定义本身）。真正的状态机在 `RenderGraph::m_resources[].state`（`RenderGraph.h:70`），而它改状态时**直接对裸 `ID3D12Resource*` 发 barrier**（`RenderGraph.cpp:102-107`），**完全不会同步 `RenderTexture::m_state`**。

后果：
- `RenderTexture::m_state` 从创建那一刻起就再也没被更新过，**永远是初始值**；
- 一旦将来有人调用 `TransitionTo`，它会用一个错误的 `StateBefore` 发 barrier → D3D12 Debug Layer 直接报 `STATE_BEFORE` 不匹配，正式版是未定义行为。

**这是两套状态真相（two sources of truth）并存**，是探针接入 Rendering 前应当清理的隐患。

### 5.5 其它边界

- `InitUAV` 无格式能力校验：传入一个不支持 UAV 的格式会在 `CreateCommittedResource`/`CreateUnorderedAccessView` 处失败（这里由 `THROW_IF_FAILED` 兜住，`RenderTexture.cpp:91`）。
- 无 resize 接口：atlas 尺寸在 `ProbeGrid::Init` 时定死，`Desc` 改了必须重启。
- 无「clear」接口：`InitUAV` 用 `nullptr` 作 clear value（`RenderTexture.cpp:93`），资源创建后内容未定义。

---

## 6. 资源状态与同步现状

### 6.1 探针 atlas 每帧的状态轨迹

| 时刻 | atlas 状态 | 触发者 |
|---|---|---|
| 创建 | `UNORDERED_ACCESS` | `RenderTexture.cpp:93` |
| 注册到 graph | `UNORDERED_ACCESS` | `App.cpp:182` |
| 帧 N：`ProbeUpdate` 前 | 若上一帧结束是 `PIXEL_SHADER_RESOURCE` → 过渡到 `UNORDERED_ACCESS` | `RenderGraph.cpp:106`，因 `ProbeUpdatePass.cpp:22` 声明 UAV |
| 帧 N：`ProbeBorderFixup` 前 | `UNORDERED_ACCESS` → 无过渡（状态相同，`RenderGraph.cpp:103` 判定跳过） | `ProbeBorderFixupPass.cpp:27` |
| 帧 N：`Lighting` 前 | `UNORDERED_ACCESS` → `PIXEL_SHADER_RESOURCE` | `LightingPass.cpp:49` |
| 帧 N 结束 | 保持 `PIXEL_SHADER_RESOURCE` | 无后续 pass 触碰 |

跨帧状态链是**闭合且正确**的（`BeginFrame` 只清 pass 不清资源，`RenderGraph.h:51` 注释与 `RenderGraph.cpp:50-53` 实现一致）✅。

### 6.2 UAV barrier 用法是否正确

两个探针 pass 都手工发了 UAV barrier：

```cpp
// ProbeUpdatePass.cpp:61-64
61:     D3D12_RESOURCE_BARRIER barrier{};
62:     barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
63:     barrier.UAV.pResource = m_probeGrid->GetResource();
64:     cmdList->ResourceBarrier(1, &barrier);

// ProbeBorderFixupPass.cpp:46-49
46:     D3D12_RESOURCE_BARRIER barrier{};
47:     barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
48:     barrier.UAV.pResource = m_atlasResource;
49:     cmdList->ResourceBarrier(1, &barrier);
```

评价：

| 检查项 | 结论 |
|---|---|
| Update → Fixup 之间的 UAV 写-写 hazard | ✅ 被 `ProbeUpdatePass.cpp:61-64` 覆盖 |
| Fixup → Lighting 之间的 UAV 写→SRV 读 hazard | ✅ 双重保险：显式 UAV barrier + `RenderGraph` 的 UAV→SRV transition barrier |
| 上一帧 Lighting(SRV 读) → 本帧 Update(UAV 写) hazard | ✅ 由 transition barrier 覆盖（`PIXEL_SHADER_RESOURCE` → `UNORDERED_ACCESS`） |
| 同一 dispatch 内读-写同一 UAV | ⚠ `BorderFixupCS` 在同一个 dispatch 里 `atlas[dstPix] = atlas[srcPix]`（`ShadersProbe.hlsl:154`）。当前读集合（tile 内部 1..8）与写集合（gutter 0/9）**不相交**，所以没有实际竞争；但这是「靠代码布局保证、编译器/驱动不保证」的写法，属于脆弱设计 |
| 跨 command list（3 帧在途）的 hazard | ✅ 单 DIRECT 队列，GPU 按提交序执行，barrier 在同一队列内全局生效。**若将来引入第二条队列（如异步 compute）就会立刻失效** |

### 6.3 atlas 是否需要跨帧保持 / TransientHeap 会不会破坏累积

**需要跨帧保持，且当前实现是安全的。**

证据链：

1. 累积确实存在：
   ```cpp
   // ProbeUpdatePass.cpp:38
   38:     p.frameIndex = m_frameIndex++;      // 单调帧号（累积用）
   ```
   ```hlsl
   // ShadersProbe.hlsl:52-54
   52:     float4 old = atlas[pixel];
   53:     float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;
   54:     atlas[pixel] = float4(lerp(old.rgb, radiance, alpha), 1.0f);
   ```
   这是指数滑动平均（α=0.05 ⇒ 约 20 帧收敛）。

2. atlas **不是**瞬态资源：`RenderTexture::InitUAV` 走的是 `CreateCommittedResource`（`RenderTexture.cpp:91-94`），**没有走 TransientHeap**。

3. **`TransientHeap::Reset()` 在全工程中从未被调用**（`TransientHeap.cpp:37` 只有定义；grep 无调用点）。每帧被重置的只有 `UploadHeap`：
   ```cpp
   // RenderContext.cpp:133  （BeginFrame 内，fence 等待之后）
   133:     frame.frameUpload.Reset();
   ```
   而 `frameUpload` 只承载每帧常量/光源数组（`App.cpp:321,325,329`），与 atlas 无关。

**结论：任务预设的「TransientHeap 每帧 Reset 会破坏累积」这一风险在本工程中不存在。** 探针 atlas 的累积是安全的。

### 6.4 但有一个真实的初始化缺陷

atlas 创建时 clear value 为 `nullptr`（`RenderTexture.cpp:93`），内容**未定义**；而 shader 第一帧仍然会先读 `old` 再做 lerp：

```hlsl
52:     float4 old = atlas[pixel];
53:     float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;
54:     atlas[pixel] = float4(lerp(old.rgb, radiance, alpha), 1.0f);
```

`alpha == 1` 在数学上等价于「丢弃 old」，但在 IEEE 浮点里 `lerp(NaN, x, 1.0)` = `NaN + 1.0*(x - NaN)` = **NaN**。`R16G16B16A16_FLOAT` 的未初始化显存完全可能是 NaN/Inf 位型。

**建议**：首帧直接赋值而不是 lerp，或者在 `ProbeGrid::Init` 后录一次 clea​r（需要一次 GPU 时间）。

### 6.5 距离矩 atlas：完全孤儿

| 检查点 | 结论 |
|---|---|
| 创建 | ✅ `ProbeGrid.cpp:11-13`，RGBA16F，144×576 |
| SRV/UAV 描述符 | ✅ `ProbeGrid.h:39-40` |
| 注册到 RenderGraph | ❌ 无 |
| 被任何 pass 写入 | ❌ 无（`ProbeDistanceCS` 是死代码，见 §7） |
| UAV 索引是否传出 | ❌ App 从未调用 `GetDistanceUAVIndex()` |
| 是否有对应 Pass 类 | ❌ `ProbeDistancePass.h` 只有一行 `#pragma once`（1 行文件） |
| 是否有对应 shader 文件 | `ProbeDistanceCS.hlsl` 是**0 字节空文件**，且在 vcxproj 里被 `ExcludedFromBuild=true` |

它现在只是占了 2 个 bindless 槽位和一块显存。

---

## 7. 断裂点清单（按失效层次排序）

### 层 1：C++ 编译期（当前根本编译不过）

#### 7.1.1 `ProbeBorderFixupPass::Init` 参数个数/类型不匹配

声明（`ProbeBorderFixupPass.h:10-18`，**11 个参数**）：

```cpp
10:     void Init(ID3D12RootSignature* rootSignature,
11:         ID3D12PipelineState* pso,
12:         RenderGraph::ResourceId atlasId,
13:         ID3D12Resource* atlasResource,   // UAV barrier 用
14:         uint32_t countX, uint32_t countY, uint32_t countZ,
15:         uint32_t uavIndex,
16:         uint32_t stride,
17:         uint32_t atlasWidth,
18:         uint32_t atlasHeight);
```

调用（`App.cpp:214-216`，**4 个实参**）：

```cpp
214: 	m_probeBorderFixupPass.Init(
215: 		m_probeRootSignature.Get(), m_probeBorderPSO.Get(),
216: 		&m_probeGrid, m_probeGridId);
```

不仅个数不同，**第 3 个实参类型也不对**（传 `ProbeGrid*`，而第 3 形参是 `RenderGraph::ResourceId`）。→ `C2660: 函数不接受 4 个参数` / `C2664` 类型转换错误。

#### 7.1.2 `ProbeBorderFixupPass` 头文件缺成员

`ProbeBorderFixupPass.cpp:18`：

```cpp
18:     m_countX = countX; m_countY = countY; m_countZ = countZ;
```

但 `ProbeBorderFixupPass.h:24-33` 的成员列表里**没有 `m_countX/m_countY/m_countZ`**：

```cpp
24: private:
25:     ID3D12RootSignature* m_rootSignature = nullptr;
26:     ID3D12PipelineState* m_pso = nullptr;
27:     ID3D12Resource* m_atlasResource = nullptr;
28:     RenderGraph::ResourceId m_atlasId = RenderGraph::kInvalidId;
29:     uint32_t m_uavIndex;
30:     uint32_t m_stride;
31:     uint32_t m_atlasWidth;
32:     uint32_t m_atlasHeight;
```

→ `C2039: "m_countX": 不是 "ProbeBorderFixupPass" 的成员`。

> 旁证：`ProbeBorderFixupPass.cpp` 是全工程**唯一**引用 `m_countX` 的地方（`ProbeBorderFixupPass.cpp:18,35-37`）。这说明头文件是从 `ProbeUpdatePass` 复制过来后没跟着改。

### 层 2：链接期 —— 经复核，**无疑似问题**（记录一条曾疑似、已排除的项）

> 这一节保留一条「看起来像 bug、复核后确认不是」的记录，避免后续审计重复踩坑。

`RenderingProgrameByHand.vcxproj:169-174`：

```xml
158:   <ItemGroup>
159:     <ClInclude Include="AccelerationStructure.h" />
...
168:     <ClInclude Include="dds_loader.h" />
169:     <ClCompile Include="DepthBuffer.cpp">
170:       <ExcludedFromBuild Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'">false</ExcludedFromBuild>
...
174:     </ClCompile>
175:     <ClInclude Include="DepthBuffer.h" />
```

**第一眼容易误判**：`<ClCompile>` 被夹在一堆 `<ClInclude>` 中间，看起来像是「`DepthBuffer.cpp` 被当成头文件、不参与编译」，进而推出 `DepthBuffer::Init` / `DepthBuffer::Clear`（唯一实现在 `DepthBuffer.cpp:5,43`）会 LNK2019。

**复核结论：误判，不是缺陷。** MSBuild 的 `<ItemGroup>` 只是**无类型的容器元素**，项目项的 ItemType 完全由**元素名**决定。因此第 169 行的 `DepthBuffer.cpp` 就是一个货真价实的 `ClCompile` 项，`Microsoft.Cpp.targets` 会把它和其它 `.cpp` 一样传给 cl.exe；四个配置的 `ExcludedFromBuild` 也都显式写成了 `false`。

这个反常的嵌套只是 **Visual Studio 在用户通过「属性 → 项类型」就地改写 item 时产生的位置残留**——它改元素名但不搬移所在的 ItemGroup。功能上完全正常。

（本次审计最初把它列为 🔴 阻断项，经复核后已剔除。保留此段是为了让后续读者不再重复这一误判。）


### 层 3：运行时 DXC 编译（`App::init` 抛异常）

#### 7.3.1 `BorderFixupCS` 引用不存在的结构体字段

`ShadersProbe.hlsl:109-117`：

```hlsl
109: [numthreads(8, 8, 1)]
110: void BorderFixupCS(uint3 id : SV_DispatchThreadID)
111: {
112:     uint3 counts = g_Probe.gridCountsAndFrame.xyz;      // ← 字段不存在
113:     const uint stride = 10;
```

`ShadersProbe.hlsl:131`：

```hlsl
131:     RWTexture2D<float4> atlas = ResourceDescriptorHeap[g_Probe.misc.w];   // ← 字段不存在
```

而 `ProbeUpdateParams` 的真实字段（`ShadersProbe.hlsl:3-12` / `ProbeTypes.h:5-23`）是：

```hlsl
 3: struct ProbeUpdateParams
 4: {
 5:     float originX, originY, originZ, spacing;           // 0
 6:     uint countX, countY, countZ, frameIndex;            // 16
 7:     uint probeCount, rayCount, objectIndex, irradianceUav; // 32
 8:     uint distanceUav;                                    // 48
 9:     uint borderTargetUav;                                // 52
10:     uint borderTileStride;                               // 56
11:     uint reserved0;                                      // 60
12: };
```

**没有 `gridCountsAndFrame`，也没有 `misc`。** → DXC 报 `error: 'gridCountsAndFrame' is not a member`。

触发点：`App.cpp:206-208`

```cpp
206: 	auto borderCsBlob = d3d12lite::CompileShaderSM66(
207: 		L"ShadersProbe.hlsl", L"BorderFixupCS", L"cs_6_6");
208: 	assert(!borderCsBlob.empty());
```

`CompileShaderSM66` 在编译失败时**是 `throw` 而不是返回空 vector**（`d3d12lite.cpp:331-334`），所以 `App.cpp:208` 的 `assert` 根本走不到，`App::init` 会直接抛 `std::runtime_error("DXC Shader Compilation Failed: ...")`，被 `main.cpp:134-138` 接住弹 MessageBox。

> 顺带：`App.cpp:122-123`（图形 shader）和 `App.cpp:160`（compute 冒烟）的 `assert(!blob.empty())` 都是同一个失效模式——`assert` 是无效断言，因为失败早已 throw。这属于既有的小瑕疵。

#### 7.3.2 `ProbeDistanceCS` 整个函数是坏的（且会连累同文件其它入口）

`ShadersProbe.hlsl:57-104` 存在**至少 6 处**硬错误：

| 行号 | 代码 | 问题 |
|---|---|---|
| 66 | `uint stride = g_Probe.borderTileStride;` | 声明 `stride` |
| 89 | `const uint stride = 18;` | **同一作用域重复定义 `stride`** |
| 74 | `uint px = probeIndex % counts.x;` | **`probeIndex` 未声明**（它只在 `ProbeUpdateCS` 里定义过） |
| 78 | `g_Probe.gridOriginSpacing.xyz` | **字段不存在**（应为 `originX/Y/Z` + `spacing`） |
| 97 | `float4 old = distAtlas[pixel];` | **`distAtlas` 未声明**（第 95 行定义的是 `atlas`） |
| 98 | `float alpha = (frameIndex == 0u) ...` | **`frameIndex` 未声明** |

`ProbeDistanceCS` **不是**当前的编译入口（`App.cpp:193-194` 只编译 `ProbeUpdateCS`，`App.cpp:206-207` 只编译 `BorderFixupCS`，全工程无任何地方引用 `ProbeDistanceCS`）。

但是：**DXC 对整份转译单元做语义分析，未作为入口的函数体同样会被检查。** 也就是说，即使只编译 `ProbeUpdateCS`，`ProbeDistanceCS` 里的未定义标识符**大概率同样报错**。
（此为静态判读，标注为**高置信度推断**——本次审计未运行 DXC 验证。建议第一次构建时优先确认。）

不论如何，**`ShadersProbe.hlsl` 当前是本工程最脏的一个文件**，三个入口里只有一个（`ProbeUpdateCS`）是干净的。

### 7.4 层 3 补充：`BorderFixupPass` 即便能跑，语义也不完整

- `ShadersProbe.hlsl:113` 把 stride **硬编码为 10**，忽略了 `g_Probe.borderTileStride`（`ProbeTypes.h:21` 注释明确说它是「10（irradiance）或 18（distance）」）。所以距离矩 atlas 的 gutter 修正**在当前 shader 里根本不可能实现**。
- `ProbeBorderFixupPass.cpp:38-39` 设置了 `borderTargetUav` / `borderTileStride`，但 shader 一个都没用（用了不存在的 `misc.w`）。**CPU 侧已经把参数准备好了，shader 侧没接上。**
- `ProbeBorderFixupPass.cpp:44` 的 Dispatch 用 `(m_atlasWidth + 7)/8, (m_atlasHeight + 7)/8`，这两个值在 `App.cpp` 的调用里根本没传（见 7.1.1）。按 `ProbeGrid` 的默认 Desc，应为 `(80+7)/8=10`、`(320+7)/8=40`。

### 7.5 断裂点总表

| 编号 | 层次 | 位置 | 症状 | 严重度 |
|---|---|---|---|---|
| B1 | C++ 编译 | `App.cpp:214-216` vs `ProbeBorderFixupPass.h:10-18` | C2660/C2664 | 🔴 阻断 |
| B2 | C++ 编译 | `ProbeBorderFixupPass.cpp:18` vs `ProbeBorderFixupPass.h:24-33` | C2039 | 🔴 阻断 |
| B4 | 运行时 DXC | `ShadersProbe.hlsl:112,131` | `App::init` 抛异常 | 🔴 阻断 |
| B5 | 运行时 DXC | `ShadersProbe.hlsl:66,74,78,89,97,98` | 同上（高置信度） | 🔴 阻断 |
| B6 | 功能 | `ShadersLighting.hlsl:116-135` | 探针不参与最终着色，DDGI 无视觉收益 | 🟠 功能缺失 |
| B7 | 功能 | `ProbeGrid.cpp:11-13` | 距离矩 atlas 完全孤儿 | 🟠 功能缺失 |
| B8 | 语义 | `ShadersProbe.hlsl:113` | stride 硬编码 10，距离矩无法修正 | 🟡 正确性 |
| B9 | 状态 | `RenderTexture.cpp:56-68` | `TransitionTo`/`m_state` 死代码，双状态真相 | 🟡 隐患 |
| B10 | 初始化 | `RenderTexture.cpp:93` + `ShadersProbe.hlsl:52-54` | 首帧 lerp 未初始化数据，可能产生 NaN | 🟡 隐患 |
| B11 | 健壮性 | `RenderGraph.cpp:102` | `m_resources[usage.id]` 无 bounds check | 🟡 隐患 |
| B12 | 文档 | `LightingTypes.h:5,26,38`；`FrameConstants.h:13`；`ShadersCommon.hlsl:232` | 注释与实现不符（8 DWORD vs 64 B 等） | ⚪ 低 |

> **编号 B3 已空缺**：它原本被列为「链接期 LNK2019（`DepthBuffer.cpp` 不参与编译）」，经复核确认为**误判**，已从表中剔除，复核过程见 §7「层 2」。

---

## 8. 修复路径建议（最小改动、按依赖排序）

> 以下仅为建议，本次审计**未修改任何源码**。

**第 1 步（让 C++ 编过，且不引入新抽象）**

1. `ProbeBorderFixupPass.h`：删掉 `Init` 里的 7 个标量参数，改为与 `ProbeUpdatePass` 一致的「持有 `ProbeGrid*`」模式：
   ```cpp
   void Init(ID3D12RootSignature* rootSignature,
             ID3D12PipelineState* pso,
             ProbeGrid* probeGrid,
             RenderGraph::ResourceId atlasId,
             uint32_t uavIndex);
   ```
   理由：`ProbeGrid` 已经提供 `GetDesc()` / `GetAtlasWidth()` / `GetAtlasHeight()`（`ProbeGrid.h:32-35`），`atlasWidth/atlasHeight/stride/countX/Y/Z` 全都能现算，不必在 pass 里再存一份（避免 `m_countX` 这类重复状态）。`atlasResource` 也不需要单独存，用 `probeGrid->GetResource()` 即可。
2. `App.cpp:214-216` 相应改成 5 参调用，`m_uavIndex` 传 `m_probeGrid.GetUAVIndex()`。→ 消除 B1、B2。

**第 2 步（让 shader 编过）**

3. `ShadersProbe.hlsl` `BorderFixupCS`：
   - `112:` `g_Probe.gridCountsAndFrame.xyz` → `uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ)`
   - `113:` `const uint stride = 10;` → `const uint stride = g_Probe.borderTileStride;`
   - `131:` `g_Probe.misc.w` → `g_Probe.borderTargetUav`
   → 消除 B4。此时 CPU 侧 `ProbeBorderFixupPass.cpp:35-39` 已经把这 5 个字段填好了，正好对上。
4. `ShadersProbe.hlsl` `ProbeDistanceCS`：要么整段注释掉/删除，要么按上面同样的方式修（推荐先删，等 `ProbeDistancePass` 真正开写时再补）。→ 消除 B5、B8。

**第 3 步（让 DDGI 真正生效）**

5. `ShadersLighting.hlsl` 第 116 行之前插入 §4.3 的采样代码，并在第 134 行把结果并入（例如 `direct + ambient.rgb + indirect + irr * kProbeWeight`）。→ 消除 B6。**这一步不需要任何新的根签名槽位或新的常量传递**，因为数据链路（§4.2）已经完整。
6. `RenderTexture.cpp:93` 之后补一次 clear，或把 `ShadersProbe.hlsl:54` 改成 `(frameIndex == 0u) ? float4(radiance, 1.0f) : lerp(...)`。→ 消除 B10。

**第 4 步（清理）**

7. 决定 `RenderTexture::TransitionTo` 的去留：要么删掉 `m_state`/`TransitionTo`（让 RenderGraph 成为唯一真相），要么让 RenderGraph 在 `Transition` 时回调资源的 `TransitionTo`。二选一，不要并存。→ 消除 B9。
8. `RegisterResource` 加 `assert(id < m_resources.size())`。→ 消除 B11。
9. 统一注释（§7.5 B12）。

**关于距离矩 atlas（B7）**：在 DDGI 主链路（irradiance → lighting）接通之前不要动它。它需要的是一整条新链路（新 Pass 类 + 新 shader 函数 + 新 RenderGraph 资源注册 + Lighting 侧的 Chebyshev 遮挡测试），属于独立里程碑。

---

## 9. 验证情况说明

| 项 | 状态 |
|---|---|
| 静态阅读源码 | ✅ 完成。覆盖任务要求的所有文件，另有 `RenderContext` / `BindlessDescriptorHeap` / `SceneGpuData` / `AccelerationStructure` / `d3d12lite` / `PipelineBuilder` / `main.cpp` / `vcxproj` |
| 构建 | ❌ **未执行**。本次是只读审计，只允许写入 `docs/ddgi/`；构建会产生 `x64/` 中间产物，因此刻意未跑 |
| 运行程序 | ❌ **未执行**（同上，且需要 D3D12 设备） |
| 行号准确性 | ✅ 全部行号来自本次实际读取的文件内容 |

**因此需要注意的置信度差异：**

- §7.1.1 / §7.1.2（C++ 编译错误）、§7.3.1（`gridCountsAndFrame` / `misc` 不存在）：**直接从源码对比得出，置信度极高**，无需运行即可确认。
- §7.3.2（`ProbeDistanceCS` 的 6 处错误）：**错误本身是确定的**；不确定的是「DXC 是否会因非入口函数报错而整份失败」。标注为**高置信度推断**，需首次构建/运行时确认。
- §6.2「跨 command list hazard 在单队列下安全」、§6.4「lerp 可能产生 NaN」：**基于 D3D12 规则与 IEEE 语义的推断**，未做运行时验证。
- §7「层 2」的 `DepthBuffer.cpp` 一项：**已被本轮复核推翻**（详见该节），保留记录仅为防止后续审计重复误判。

---

## 10. 附：三套「帧号」对照（做时序累积时容易踩）

| 帧号 | 定义处 | 语义 | 使用者 |
|---|---|---|---|
| `m_context.GetFrameIndex()` | `RenderContext.cpp:159` | **环形槽位 0..2**，每帧 `(i+1)%3` | `ctx.frameIndex`（`App.cpp:348`）、backbuffer 选择（`App.cpp:368`） |
| `App::m_frameCounter++` | `App.cpp:319` | **单调递增**，写入 `PerFrameData::frameIndex` | `g_Frame.frameIndex`（`ShadersCommon.hlsl:28`），供 `QuasiRandom2D` 抖动（`ShadersLighting.hlsl:122`） |
| `ProbeUpdatePass::m_frameIndex++` | `ProbeUpdatePass.cpp:38` | **单调递增**，独立计数器 | `g_Probe.frameIndex`（`ShadersProbe.hlsl:22`），做 α 衰减 |

三者语义完全不同，且 `ctx.frameIndex` **不能**用于任何跨帧累积（会每 3 帧归零，导致 `alpha` 每 3 帧变成 1.0 把历史冲掉）。当前 `ProbeUpdatePass` 用的是自己的私有计数器，**这一点是正确的**，但这是个很容易在未来改动中被破坏的约定，建议加注释固化。
