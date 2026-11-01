# DDGI 实施方案：修改方向与修改方法

> 面向：`RenderingProgrameByHand`（D3D12 + DXR，VS2022 / v143 / C++latest / SM 6.6）
> 依据：
> - 论文 *Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields*（Majercik et al., JCGT 8(2), 2019）—— 我已通读全文
> - **官方补充材料源码**（`supplement.zip` 里的 `sampleIrradianceField.pix` / `IrradianceField_updateIrradianceProbe.pix` / `IrradianceField_generateRandomRays.pix` / `IrradianceField.h/.cpp`）—— 见 `04-reference-formulas.md`
>
> 配套：`01-probe-core-audit.md`、`02-integration-audit.md`、`03-build-verify.md`、`04-reference-formulas.md`
> **本文只给方向与方法，不含已落地的源码改动。**

---

## 0. 一页速览

### 0.1 一句话现状

**骨架搭对了，算法还没对上，最后一公里没接。**

探针的**数据结构、tile 布局、图集寻址、pass 接线、根签名绑定**都是对的，而且对得很讲究（§2.3 有逐项核对表）。
真正缺的是三件事，按严重度排序：

| # | 缺什么 | 后果 |
|---|---|---|
| 1 | `LightingPS` 的**最终着色不读探针** | 修完所有编译错误，**画面也不会有任何变化**。DDGI 目前等于一个 debug 可视化 |
| 2 | `ProbeUpdateCS` 存的是"**沿方向的一个 radiance 样本**"，不是论文 Eq(1) 的"**余弦加权归一化辐照度**" | 数值物理意义错误；且因为每帧同一条射线，**时间累积退化成原地空转**，banding 永远消不掉 |
| 3 | 距离矩整条链路是**死资源** | 论文的核心创新（visibility-aware moment interpolant）一个字节都用不上，漏光零防护 |

另外两层**纯工程**阻断：C++ 侧 7 个编译错误、HLSL 侧 11 个编译错误。这两层是止血，不涉及算法。

### 0.2 DDGI 各环节完成度

| 环节 | 论文/官方实现要求 | 本工程 | 完成度 |
|---|---|---|---|
| 均匀网格探针放置 | 2 的幂、可独立缩放 | `ProbeGrid::Desc`，8×4×8，spacing 0.63 | **100%** |
| 图集打包 | tile + 1px gutter | `8 / 1 / stride 10` | **100%** |
| 距离矩图集 | `16 / 1 / stride 18` | `16 / 1 / 18` | 资源 100% / 逻辑 0% |
| 图集寻址 | tile 行列 = (px, py+pz·cy) | update / debug 两侧互逆，自洽 | **100%** |
| 探针更新：射线生成 | 每帧**全局随机旋转**的 Fibonacci 螺旋 | 每 texel 一条**固定**射线；`StochasticFibonacci` 零调用 | **0%** |
| 探针更新：余弦加权 + `/Σw` | `texel = Σ L·w / Σ w` | 无累加，直存单射线 radiance | **0%** |
| 探针更新：hysteresis | `(1-h)·新 + h·旧`，h=0.98 | α=0.05 的 EMA 骨架，方向**正确**，但射线固定使其失效 | 骨架 100% / 效果 0% |
| 距离矩更新 | `cos^50` 权重 + 阈值 | 死代码，6 处语法错 + 线程模型错 | **0%** |
| gutter 修补 | 1px 边缘复制 | 算法正确、无竞争，但 stride 写死 10 | 60% |
| 采样：8 探针三线性 | `x + y·cx + z·cx·cy` | **不存在** | **0%** |
| 采样：octahedral 双线性 | `tileOrigin + 1 + oct01·S` | **不存在**（只有 `.Load` 最近邻 debug） | **0%** |
| backface wrap 权重 | `((dot+1)/2)² + 0.2` | **不存在** | **0%** |
| Chebyshev 可见性 | `σ²/(σ²+(t-μ)²)` 再 `³` | **不存在** | **0%** |
| 低辐照度 crush | `w<0.2 → 25w³` | **不存在** | **0%** |
| normal bias | `(n + 3·ω_o)·0.25` | **不存在** | **0%** |
| 多弹射 | 跨帧递归 seeding | **不存在** | **0%** |

### 0.3 六个关键判断（如果只记六条）

1. **探针更新必须是"每个 texel 遍历 N 条射线、余弦加权、除以权重和"**。官方参考实现就是这么做的（gather 结构）。§4.1、§4.2。
2. **归一化用 `/Σw`，不要 `/N`**。得到的是 `È = E/π`，直接套 `albedo * 探针值` 就是 Lambert 出射辐亮度，和你现有 `albedo * indirect` 约定兼容。§4.2。
3. **两套 normaLization 绝不能混用**。官方 texel 语义是 `È`（已含余弦卷积），你现在的 texel 语义是 `L(dir)`。要么改成 gather（A 路线），要么在采样侧补球面加权求和（B 路线），**选一条走到底**。§4.1 末尾。
4. **`maxDistance` 不是随手拍的 100，而是 `1.5 × 网格单元对角线`**。你的网格算下来 ≈ **1.64**。§4.3。
5. **每个探针是独立 tile，采样 UV 不跨 tile**，gutter 只服务 tile 内双线性。"跨 tile 有缝"在当前布局下不存在，**别去做八面体接缝重建**。而且论文里**没有**四面体插值，就是八点三线性。§4.4。
6. **要做多弹射，单张 atlas 不够**，必须 ping-pong 两张。§4.6。

---

## 1. 我读到的工程骨架（先确认我理解对了）

### 1.1 分层

```
App                          应用层：持有全部子系统 + PSO + 场景 + 光照
├─ RenderContext             设备 / 交换链 / 单 DIRECT 队列 / Fence / 3 帧在途
├─ RenderGraph               线性 pass 列表 + 状态追踪 + transition barrier 注入
│   └─ RenderPass (pass.h)   Declare / Execute 两段式
├─ ProbeGrid                 CPU 描述 + 两张 UAV 图集（辐照度 80×320 / 距离矩 144×576）
├─ RenderTexture             资源 + RTV/SRV/UAV + m_state
├─ TransientHeap             256MB placed heap（注意：Reset 从未被调用）
├─ UploadHeap                64MB 持久映射（每帧 Reset）
├─ GPUDescriptorAllocator    bindless 堆（10000 槽）
├─ Scene / SceneGpuData      GPUObjectData 的 bindless SRV（供 TraceRadiance 取 albedo/normal）
├─ rt::AccelerationScene     BLAS / TLAS
└─ Camera / Input / MeshLibrary / Texture2D / dds_loader / PipelineBuilder
```

### 1.2 单帧数据流（以 `App::RenderTick` 为准）

```
UpdateFrameStats → Camera → Input(debugMode 键)
  ↓
m_context.BeginFrame()        等本帧 fence，Reset allocator/cmdList，Reset frameUpload
  ↓
填 PerFrameConstants(b1) / LightingConstants(b3) / GPULight[](t5) → 全写进 frame.frameUpload
  ↓
SetDescriptorHeaps(1, bindless)
  ↓
m_graph.BeginFrame() → AddPass × 6 → m_graph.Execute(ctx)
  ├─ 1. GBuffer          Write  albedo/normal = RENDER_TARGET, depth = DEPTH_WRITE
  ├─ 2. ComputeSmoke     RW     computeTex   = UNORDERED_ACCESS
  ├─ 3. ProbeUpdate      RW     probeAtlas   = UNORDERED_ACCESS   ← Dispatch(probeCount,1,1)
  ├─ 4. ProbeBorderFixup RW     probeAtlas   = UNORDERED_ACCESS   ← Dispatch(ceil(w/8),ceil(h/8))
  ├─ 5. Lighting         Read   albedo/normal/depth/probeAtlas, Write backbuffer
  └─ 6. BackbufferToPresent Transition → PRESENT
  ↓
m_context.EndFrame()  Close → ExecuteCommandLists → Signal → Present
```

### 1.3 根签名预算（重要约束）

**图形根签名 `m_rootSignature`**（`App.cpp:106-114`）：40 + 2 + 16 + 1 + 2 + 2 = **63 / 64 DWORD**。

> ⚠ **只剩 1 个 DWORD**。这决定了后面加 DDGI 参数时**不能往图形根签名里塞新的 root constant / CBV / SRV**，必须复用已有的 b2。好消息：采样侧需要的东西（origin、spacing、counts、atlas SRV 索引）**已经全在 b2 里了**（`LightingTypes.h:18-22` ↔ `ShadersCommon.hlsl:243-244`）。

**探针 compute 根签名**（`App.cpp:185-191`）：5 槽，b4(16) / b2(16) / t3(1) / b3(2) / t5(2)，与 `ProbeUpdatePass.cpp:51-55` 的 `SetComputeRoot*(0..4)` **逐个精确匹配**，连 DWORD 数都对。这是全工程最干净的一环，改造时不要动。

---

## 2. 现在到底断在哪：四层

### 2.1 L0 · 构建层（7 个 C++ error）

| 位置 | 错误 | 根因 |
|---|---|---|
| `App.cpp:214` | C2660 不接受 4 个参数 | 头文件 `ProbeBorderFixupPass::Init` 声明 **11 个**形参，调用点只给 4 个，且第 3、4 个实参类型不符 |
| `ProbeBorderFixupPass.cpp:18,35,36,37` | C2065 × 6 | 头文件私有区缺 `m_countX / m_countY / m_countZ` |

其余 25/27 个 TU 单独编译全部通过（已实测）。探针改造是当前唯一挡住构建的地方。

### 2.2 L1 · 启动层（11 个 HLSL error，同一编译单元）

| 函数 | 错误数 | 归属 |
|---|---|---|
| `ProbeUpdateCS` | **0** | 自身干净 |
| `ProbeDistanceCS` | 9 | `probeIndex` 未声明、`gridOriginSpacing` 不存在、`stride` 重定义、`distAtlas`/`frameIndex` 未声明 |
| `BorderFixupCS` | 2 | `gridCountsAndFrame` / `misc` 不存在 |

DXC 对整个 TU 做语义分析 ⇒ `App.cpp:193` 只想编译 `ProbeUpdateCS` 也会失败 ⇒ `CompileShaderSM66` 抛 `std::runtime_error` ⇒ 程序起不来。

> **机制性教训**：`.hlsl` 在 vcxproj 里全部 `ExcludedFromBuild=true`，MSBuild 一次都不调用 dxc，仓库里也没有 `.cso`。**"生成成功"对 shader 零保证**。修完必须手工跑 dxc（§7.6）。

### 2.3 L2 · 数据生产层（算法错）

先给结论：**结构体没毛病**。`ProbeTypes.h` 与 `ShadersProbe.hlsl` 的 `ProbeUpdateParams` 是 64 B / 16 DWORD 逐字段一致，`static_assert` 也过。错的在函数体：它引用了旧版字段名（`gridOriginSpacing` / `gridCountsAndFrame` / `misc`，那是 `LightingParams` 的命名）。

**图集寻址三处互逆，是对的**，可以放心：

| 侧 | px | py | pz | tile 行 |
|---|---|---|---|---|
| `ProbeUpdateCS:30-32` | `i%cx` | `(i/cx)%cy` | `i/(cx·cy)` | `(py+pz·cy)·stride` |
| `ShadersLighting.hlsl:81` debug | `i%cx` | `(i/cx)%cy` | `i/(cx·cy)` | `(i/cx)·stride` |
| 一致性 | ✅ | ✅ | ✅ | ✅（`i/cx == py+pz·cy`） |

**线程映射也是对的**：`Dispatch(probeCount,1,1)` + `numthreads(8,8,1)` ⇒ `id.x/8 ≡ SV_GroupID.x` ⇒ 一 group 一探针、64 texel/探针。这条不是 bug。

真正的算法错在 `ShadersProbe.hlsl:38-54` 这三行：

```hlsl
float2 uv = (float2(texel) + 0.5f) / 8.0f;      // 固定的 texel 中心
float3 dir = OctDecode(uv);                     // 纯函数，无任何扰动
float3 radiance = TraceRadiance(probePos + dir * 0.02f, dir);   // 每个 texel 一条射线
```

见 §4.1。

### 2.4 L3 · 数据消费层（最致命）

`ShadersLighting.hlsl:116-135` 的最终着色**完全不引用探针**：

```hlsl
const uint giSamples = 2;
for (uint i = 0; i < giSamples; ++i) { ... indirect += TraceGI(world.xyz, N, u); }
indirect /= giSamples;
float3 direct = EvaluateDirectLighting(world.xyz, N);
float3 color = albedo.rgb * (direct + ambient.rgb + indirect);
```

探针图集只在 `debugMode == 8` / `== 9` 两个分支里被 `.Load` 读出来看一眼。而且 `.Load` 是 texel 精确读取，**不经过 gutter**——所以 gutter 现在算了也没人消费。

### 2.5 L4 · 质量层（可见性 / 多弹射）

- 距离矩 atlas 是**完全孤儿**：`GetDistanceResource()` / `GetDistanceSRVIndex()` / `GetDistanceUAVIndex()` 全工程零调用。
- `ProbeUpdateParams.distanceUav`（偏移 48）**没有任何人写**，恒为 0。这是定时炸弹：一旦有人拿它去 `ResourceDescriptorHeap[0]`，会把 **第 0 号描述符（G-Buffer albedo 的 SRV）当 UAV 写**，静默写坏别人资源。
- `TraceRadiance`（`ShadersCommon.hlsl:306-348`）在命中点只算"直射光 + `SkyColor()*0.3`"，**不采样探针** ⇒ 探针射线只能看到一次弹射。

---

## 3. 论文 ↔ 官方代码 ↔ 本工程 三方映射

| 论文概念 | 论文 | 官方代码 | 本工程 | 状态 |
|---|---|---|---|---|
| 轴均匀 3D 网格探针 | §4.1 | `probeCounts` 2 的幂 | `ProbeGrid::Desc` | ✅ |
| 8×8 octahedral 辐照度 | §3 | `irradianceOctResolution=8` | `kTileSize=8` | ✅ |
| 16×16 距离矩 | §3 | `depthOctResolution=16` | `kDistanceTileSize=16` | ✅ 资源 |
| 1px gutter | Fig.3 | `stride = S+2` | `kGutter=1` | ✅ |
| 每帧 n 条射线 | §4.2 | `irradianceRaysPerProbe=64` | `rayCount=64`（shader 不读） | ❌ |
| stochastically-rotated Fibonacci | §4.2 | `randomOrientation` 每帧一次 | `StochasticFibonacci` 零调用 | ❌ |
| Eq(1) 余弦加权累加 | §4.4 | `Σ L·max(0,cos)` **再 `/Σw`** | 无累加 | ❌ |
| hysteresis | §4.4 | `(1-h)·新 + h·旧`，h=0.98，首帧 0 | `lerp(old,new,α)`，α=0.05 ⇒ h=0.95 | 🟡 骨架（方向对） |
| cosine lobe（辐照度） | §4.4 | `w = max(0,cos)` | 无 | ❌ |
| depth sharpening | §4.4 | `w = max(0,cos)^50` | 无（等价 k→∞） | ❌ |
| 权重阈值 | 0.001 | **1e-6** | 无 | ❌ |
| 8 探针 cage + 三线性 | §5.2 | `for i in 0..7` | 无 | ❌ |
| backface 软阈值 | §5.2 | `((dot+1)/2)² + 0.2` | 无 | ❌ |
| 低辐照度抑制 | §5.2 | `w<0.2 → 25w³` | 无 | ❌ |
| Chebyshev 可见性 | §5.2 | `σ²/(σ²+(t-μ)²)` 再 `³` | 无 | ❌ |
| normal bias | §5.2 | `(n+3ω_o)·0.25` | 无 | ❌ |
| 跨帧多弹射 | §5.3 | `singleBounce=false` | 无 | ❌ |
| 四面体**插值** | — | **不存在** | — | 不必做 |
| probe relocation | — | **不存在**（论文原版） | — | 不必做 |

---

## 4. 关键算法决策（这一节是"方向"，也是最需要想清楚的部分）

### 4.1 【核心】探针更新：每 texel 遍历 N 条射线，余弦加权，除以权重和

#### 官方参考实现（`IrradianceField_updateIrradianceProbe.pix:88-117`）

```glsl
for (int r = 0; r < RAYS_PER_PROBE; ++r) {
    vec3  texelDirection = octDecode(normalizedOctCoord(ivec2(gl_FragCoord.xy)));
#if OUTPUT_IRRADIANCE
    float weight = max(0.0, dot(texelDirection, rayDirection));
#else
    float weight = pow(max(0.0, dot(texelDirection, rayDirection)), depthSharpness); // 50
#endif
    if (weight >= epsilon) {
        result += vec4(rayHitRadiance * weight, weight);      // rgb 累加 ΣL·w，a 累加 Σw
    }
}
if (result.w > epsilon) {
    result.xyz /= result.w;          // ← 除以 Σw，不是除以 RAYS_PER_PROBE
    result.w = 1.0f - hysteresis;    // ← alpha 槽位改写成混合系数
}
```

**texel 不是"一条射线的容器"，而是一个余弦叶瓣的积分器。**

#### 你现在的写法错在哪

每个 texel 沿自己的方向打**一条**射线，把那条射线直接写进 texel。三个独立后果：

**① 语义错（L ≠ È）**
texel 里存的是 `L(d)`，不是 `È(d) = Σ L·cos / Σ cos = E(d)/π`。
用 `albedo * L(d)` 当间接光，等价于假设"入射光完全来自法线方向"——间接光缺方向混合、色溢偏弱、暗部偏黑。

**② 抗噪差一个数量级**
参考实现里 64 条射线**全部参与每个 texel**，天然低通。你的写法是 64 条射线各管一个 texel，每个 texel 只有 1 个样本。

**③ 时间累积彻底失效（最要命）**
`OctDecode((texel+0.5)/8)` 是纯函数，`TraceRadiance` 对确定性输入返回确定值。EMA 序列：

```
avg_n = α·Σ (1-α)^(n-i)·L  =  L·(1 - (1-α)^n)  →  L
```

"第 1 帧填满，之后所有帧原地空转"。**hysteresis 完全没用，banding 一条都不会消。**

#### 正确形态（D3D12 compute 版）

```hlsl
// 方向示意，不是最终代码
[numthreads(8, 8, 1)]
void ProbeUpdateCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    uint probeIndex = gid.x;                       // 比 id.x/8 更直白

    uint3  counts = uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ);
    uint3  pc     = uint3(gid.x % counts.x, (gid.x / counts.x) % counts.y,
                          gid.x / (counts.x * counts.y));
    float3 probePos = float3(g_Probe.originX, g_Probe.originY, g_Probe.originZ)
                    + float3(pc) * g_Probe.spacing;

    // 官方做法：每帧一个全局随机旋转（见下方"旋转源"）
    float2 rot = g_Probe.frameRotation;             // 新增字段，或由 frameIndex 哈希出来

    float3 texelDir = OctDecode((float2(gtid.xy) + 0.5) / 8.0);

    float3 accRGB = 0; float accW = 0;
    const float kRayMinDistance = 0.08;             // 官方 rayMinDistance
    const float kEnergy          = 0.95;             // 官方 energyConservation

    uint n = max(g_Probe.rayCount, 1u);
    for (uint r = 0; r < n; ++r) {
        float3 rayDir  = StochasticFibonacci(r, n, rot);
        float  w       = max(0.0, dot(texelDir, rayDir));
        if (w < 1e-6) continue;                       // 官方 epsilon
        float3 rad     = TraceRadiance(probePos, rayDir, kRayMinDistance, kMaxDistance);
        accRGB += rad * kEnergy * w;
        accW   += w;
    }
    if (accW < 1e-6) return;                          // 本 texel 无有效样本 → 保留旧值
    float3 irr = accRGB / accW;

    // ……写回（首帧直接赋值，见 §6.5）
}
```

#### 旋转源：官方是**每帧一个全局旋转**，不是每探针一个

```cpp
// 官方 C++ 侧（每帧 setUniform 一次）
args.setUniform("randomOrientation",
    Matrix3::fromAxisAngle(Vector3::random(), Random::common().uniform(0.f, 2 * pif())));
```

即：随机轴 + `[0,2π)` 随机角，**整个探针场共用**。所以 n 条射线的相对分布始终是 Fibonacci 螺旋，只是整体朝向每帧变化。

> ⚠ 自研时**必须显式归一化随机轴**，否则 `fromAxisAngle` 得到的矩阵带缩放，`FibonacciSphere` 的结果不再是单位向量。
> 也可以退化成"两个角度的旋转"（你现成的 `RotateY` + `RotateX`），但要注意均匀性稍差。

#### 三个容易做错的细节

**a) 旋转必须在**时间**上变**
这是让 EMA 收敛到"球面积分"而不是"单射线值"的唯一手段。旋转不动 = 白算。

**b) `w < ε` 的丢弃不能省**
官方 `epsilon = 1e-6`（论文写 0.001）。对辐照度分支两者等价（都≈90° 半角）；对距离分支差别是 40.7° vs 29.4° 的锥角（§4.3）。

**c) `accW < ε` 时要保留旧值，不能写 0**
写 0 会在这个 texel 上留下永久黑洞（下一帧从 0 开始 lerp，收敛极慢）。

#### 代价，以及一个 64× 的优化机会

`probeCount × 64 texel × N ray`。你的网格 8×4×8 = **256 探针**：

| N | 射线查询数 | 粗估（按论文 1.5 GRays/s） |
|---|---|---|
| 32 | 524,288 | ~0.4 ms |
| 64 | 1,048,576 | ~0.7 ms |

但注意 `TraceRadiance` 内部还会打一条阴影射线（`EvaluateDirectLighting` 对方向光调 `TraceSunShadow`），实际查询数翻倍。

> **可选优化（先别做）**：64 个 texel 各 trace 一遍**同一批**射线，浪费了 64 倍。可用 `groupshared float4 sRadiance[64]` 让 64 个线程各 trace 一条、`GroupMemoryBarrierWithGroupSync()`，然后每个 texel 只做累加。射线数直接降到 1/64。
> **但正确性优先**：先按官方形态跑通、确认画面对了再上。它要求 `rayCount == 64 == 线程数`，会和调 N 冲突。

#### ⚠ A 路线 vs B 路线：归一化不能混用

官方参考实现报告（`04-reference-formulas.md` §7.1）明确指出，两种结构**不能混**：

| | A 路线（官方 gather） | B 路线（你现在的 scatter） |
|---|---|---|
| texel 含义 | `È = E/π`（已含余弦卷积） | `L(dir)`（方向辐亮度） |
| 采样侧额外操作 | **无** | **必须**做球面加权求和 `E ≈ (4π/64)·Σ L_i·max(0,n·ω_i)` |
| 噪声 | 天然低通 | 单样本，高噪声 |

B 路线看起来"改动小"，但采样侧要重做一次余弦卷积（等价于把 texel 当八面体的光源图重新积分），工作量并不比 A 小，而且**如果只是把 texel 平均一下当辐照度，得到的不是 E/π，而是球面平均辐亮度**——在强侧光下会差数倍，且误差**不是常数**，无法靠一个系数校正。

**建议走 A 路线**（也就是本节给的形态）。

---

### 4.2 【核心】归一化：`/Σw` 得到 `E/π`

#### 推导（我已与官方实现交叉验证）

N 条射线在球面上均匀分布（Fibonacci 的性质），对 texel 方向 `d`：

```
S = Σ_i max(0, d·ω_i)·L_i
W = Σ_i max(0, d·ω_i)

蒙特卡洛：∫f dω ≈ (4π/N)·Σf(ω_i)
S ≈ (4π/N)·∫L·max(0,d·ω)dω = (4π/N)·E(d)
W ≈ (4π/N)·∫max(0,d·ω)dω   = (4π/N)·π = N/4

⇒ S/W = E(d)/π
```

**`S/W = E/π`。** 而 `L_o = (ρ/π)·E = ρ·(E/π)`，所以 `albedo * probeValue` 直接就是出射辐亮度 —— 与你现有 `ShadersLighting.hlsl:134` 的 `albedo.rgb * (direct + ambient + indirect)` 约定**完全兼容**（`TraceGI` 那条路径估出来的也正好是 `E/π`：余弦加权采样、pdf=cos/π、`(1/N)ΣL`）。

#### 但最后那个 `×π` 要自己标定

官方参考实现自己在最后一行的常数是**管线相关的**：

| 实现 | 末行 | 等效 |
|---|---|---|
| G3D 官方 | `E_lambertianIndirect = 0.5 * pi * netIrradiance` | ≈ `E/2` |
| 物理正确（本文约定） | 不乘，直接 `albedo * È` | `E/π` → 出射 `ρ·E/π` |
| RTXGI（第三方） | `irradiance *= RTXGI_2PI` | `2π` |

三者差 4×，属于**各自渲染管线的能量约定**（G3D 的直射强度约定 + 递归 bounce 衰减；RTXGI 的 gamma 编码）。**不要直接抄常数**，用下面的 white furnace 标定。

#### 验证：white furnace test（5 分钟，一次性验证归一化 + cos 权重 + oct 编解码）

把环境设成"处处 radiance = 1"（`SkyColor()` 返回 `(1,1,1)`，所有物体 albedo=1，**关掉直射光**），则解析解：

- 任何法线方向 `E = π`；
- 探针 texel 应读出 `È = E/π = 1.0`；
- 最终像素应等于 `albedo × 1 = 1`。

| 读到 | 说明 |
|---|---|
| **1.0** | ✅ 归一化、cos 权重、oct 编解码全对 |
| 0.25 | 你除以了 N（应为 `/Σw`） |
| π | 你多乘了一个 π |
| 0.5 | 你抄了 G3D 的 `0.5*π`（那是他们管线的约定） |

> ⚠ 测试前要把 `TraceRadiance` 里的 `SkyColor()*0.3` 这个 hack 和直射项一起关掉，否则测的不是探针逻辑。

---

### 4.3 【核心】距离矩：`cos^50` 权重 + 正确的 `maxDistance`

#### 为什么颜色与距离用不同指数

| 通道 | 权重 | 理由 |
|---|---|---|
| 辐照度 | `max(0, cosθ)` | Lambert 余弦卷积**本身就是辐照度的定义**，只需在更新时做一次 |
| 距离矩 | `max(0, cosθ)^50` | 距离场不是可积的辐射度量，必须锐化成近似 delta 的 lobe，否则一个 texel 会混合整个半球的深度 → 均值被远处几何拉大 → 可见性判断失效 |

`depthSharpness = 50` 是**论文正文没给、只有代码给的唯一权威值**。由 `ε ≤ cos^k` 可反解 lobe 半角：

| k | ε | 半角 | 说明 |
|---|---|---|---|
| 1 | 1e-6 | 90° | 辐照度，几乎整个前半球 |
| 50 | **1e-6（代码）** | **40.7°** | 距离，默认实际孔径 |
| 50 | **0.001（论文）** | **29.4°** | 论文正文阈值对应孔径 |
| 100 | 1e-6 | 29.4° | 等价于"k=50 + 论文阈值" |

> 建议把两个阈值分开命名（`kIrradianceWeightEps` / `kDepthWeightEps`），论文值 1e-3 可作为保守默认。

#### `maxDistance` 不是 100，是 `1.5 × 网格单元对角线`

```cpp
// 官方 .cpp:164-166
const Vector3 boundingBoxLengths(spec.probeDimensions.high() - spec.probeDimensions.low());
m_maxDistance = (boundingBoxLengths / spec.probeCounts).length() * 1.5f;
```

对你的网格（spacing 0.63，8×4×8，origin (-2.2, 0.3, -2.2)）：

```
包围盒 = (4.41, 1.89, 4.41)  ÷  (8,4,8)  =  (0.55, 0.47, 0.55)
|·| = 0.906   ×1.5  ⇒  maxDistance ≈ 1.36
```

（若严格按 "cell diagonal" 用 spacing 算：`0.63·√3 = 1.09 ×1.5 = 1.64`。两种算法都在 1.3~1.7 量级，**远小于 100**。）

**为什么必须这么小**：
1. `E[d] ≤ maxDistance`、`E[d²] ≤ maxDistance²` ⇒ RGBA16F 里 `d² ≤ 3`，half 精度充足。用 100 的话 `d² = 10⁴`，而 `variance = |μ² - E2|` 是两个接近大数相减，**精度损失会把方差算成 0**（→ 可见性恒 1，等于没有遮挡）或算爆。
2. `maxDistance` 的作用是**截断尾部**：超过一格半的距离差被压平，远处几何与 miss 在统计上趋于不可区分 —— 这正是"避免远处几何被当成遮挡"的手段。

**miss 写 `maxDistance`，不是 inf、不是 0。**

> 实现小技巧：既然超过 `maxDistance` 一律当 miss，直接把这个值当 `RayDesc.TMax` 即可——等价且不用额外 clamp。

#### 从矩到可见性（含"为什么是 ³ 次幂"）

```hlsl
float2 m   = SampleBilinear(depthAtlas, p, OctEncode(dir));   // 双线性过滤后的矩
float  mu  = m.x, e2 = m.y;
float  var = abs(mu * mu - e2);              // abs：bias 可导致 E2 略小于 μ²
float  t   = distToProbe;                    // 用**加了 normalBias 的**距离（§4.5）

float cheb = var / (var + pow2(max(t - mu, 0.0)));   // 单边 Chebyshev（VSM eq.5）
cheb = max(pow3(cheb), 0.0);                          // 经验锐化，不是更严格的界
weight *= (t <= mu) ? 1.0 : cheb;                     // 比平均面还近 ⇒ 必然可见
```

推导：记 `X` = 探针沿 `dir` 到最近表面的距离，`μ = E[X]`、`σ² = E[X²] - μ²`。单边 Chebyshev：对 `t > μ`，`P(X ≥ t) ≤ σ²/(σ² + (t-μ)²)`。语义上 `X ≥ t` ⇔ 距离 t 处没有遮挡 ⇔ 着色点可见，所以右侧是**可见性的上界**。直接用它是**保守**的（宁可信其可见，避免过度变暗）。

`pow3` 是经验锐化。若发现间接阴影"太软"，先查这一项。

> ⚠ 采样的 `r` 通道用的是**双线性过滤后**的矩——这正是论文说 "moment-based filtered shadow query" 的含义：模糊发生在**矩空间**，不是 visibility 空间。

---

### 4.4 【核心】采样：8 探针 cage + 三线性 + 每探针 octahedral 双线性

#### cage 与三线性（官方 `sampleIrradianceField.pix:111-148`）

```glsl
ivec3  baseGridCoord = clamp(floor((wsPosition - start) / step), 0, counts - 1);
Point3 baseProbePos  = gridCoordToPosition(baseGridCoord);
Vector3 alpha = clamp((wsPosition - baseProbePos) / probeStep, 0, 1);

for (int i = 0; i < 8; ++i) {
    ivec3 offset = ivec3(i, i >> 1, i >> 2) & ivec3(1);                       // 三轴各取 bit0/1/2
    ivec3 c      = clamp(baseGridCoord + offset, 0, probeCounts - 1);          // 再次 clamp
    ProbeIndex p = c.x + c.y*countX + c.z*countX*countY;                       // 行优先，与你一致
    Vector3 trilinear = lerp(1.0 - alpha, alpha, offset);
    ...
    weight *= trilinear.x * trilinear.y * trilinear.z;
}
```

两个要点：

- **边界处 8 个探针会重复**（因为二次 clamp），同一个探针被多次加权。官方**不做去重**，靠后面的 `weight = max(1e-6, …)` 和归一化吸收。你照做即可，不用特意处理。
- **三线性就是全部**。论文里没有四面体插值（论文 §4.1 提到的 "tetrahedral" 是指探针**布置**可以是四面体网格，不是插值方式）。

#### 每探针的 oct 双线性：`tileOrigin + 1 + oct01·S`

官方 `textureCoordFromDirection()`（`sampleIrradianceField.pix:60-79`）：

```glsl
vec2 oct01   = (octEncode(normalize(dir)) + 1.0) * 0.5;   // [-1,1] → [0,1]
float stride = S + 2.0;
int probesPerRow = (W - 2) / int(stride);
vec2 probeTopLeft = vec2(mod(p, probesPerRow) * stride, (p / probesPerRow) * stride) + vec2(2.0);
vec2 uv = (probeTopLeft + oct01 * S) / vec2(W, H);
```

`+2` = 1px 全局外边框 + 1px 探针 gutter。**你的工程没有全局外边框**，所以是 `+1`：

```hlsl
// 本工程等价形式
float2 oct01  = OctEncode(n);                              // 你的实现直接返回 [0,1]
float2 t_pix  = tileOriginPx + 1.0 + oct01 * kTileSize;     // tileOriginPx = (tileX*10, tileY*10)
float2 uv     = t_pix / float2(atlasW, atlasH);
float3 irr    = atlas.SampleLevel(g_ClampSampler, uv, 0.0).rgb;
```

**为什么是 `1 + oct01*S` 而不是 `1.5 + oct01*S`**：tile stride 10，内部 texel 索引 1..8 的中心在像素坐标 1.5..8.5。`1 + oct01*8` 在 `oct01=0` 时落在像素 1.0（gutter texel 0 与 texel 1 之间），因为 texel 0 是 texel 1 的复制品，双线性结果恰好等于 texel 1；`oct01=1` 时落在 9.0，同理等于 texel 8。**`oct01 ∈ [0.0625, 0.9375]` 时落在真正的 texel 中心**——这正是标准的 half-texel inset。

#### ⚠ 必须用 CLAMP 采样器

`ShadersCommon.hlsl:44` 只声明了 `sampler g_LinearSampler : register(s0)`，而 `RootSignature.cpp:25-32` 里 **s0 是 WRAP**。用 s0 采图集，最外层探针的采样会**环绕到图集另一侧**，读到完全不相干的探针。

`RootSignature.cpp:34-39` 已经准备好 **s1 = CLAMP**，官方参考实现用的也是 **bilinear + clamp**（`Sampler::video()`）。所以需要在 `ShadersCommon.hlsl` 补一行：

```hlsl
sampler g_ClampSampler : register(s1);
```

#### node: 采样方向是**法线**，不是反射方向

```glsl
vec3 irradianceDir = wsN;    // 官方 sampleIrradianceField.pix:201
```

因为 texel 已经存了余弦卷积的结果，直接沿 `n` 取值即可。

#### ⚠ 关于"跨 tile 有缝"—— 不存在，别去做

每探针是独立 tile，UV 按 `tileOrigin` 计算，**永远不会跨 tile 读取**。gutter 的唯一职责是让 tile **内部**的双线性在边界处不外溢，±1 复制正是这件事的正确做法（等价 clamp-to-edge）。

八面体折痕（`|u|+|v|=1`）在 tile 内部确实有轻微不连续，但那是八面体参数化的固有近似，官方也不处理。**不要为它立项。**

---

### 4.5 采样侧完整权重链（官方 `sampleIrradianceField.pix:116-238`，按代码真实顺序）

```hlsl
float3 SampleIrradianceField(float3 x, float3 n, float3 wo)   // wo = 指向相机的单位向量
{
    int3   base  = clamp(floor((x - origin) / spacing), 0, counts - 1);
    float3 alpha = saturate((x - GridToPos(base)) / spacing);
    float3 sumE = 0; float sumW = 0;

    for (int i = 0; i < 8; ++i) {
        int3 off = int3(i, i >> 1, i >> 2) & 1;
        int3 c   = clamp(base + off, 0, counts - 1);
        int   p  = c.x + c.y * counts.x + c.z * counts.x * counts.y;
        float3 P = GridToPos(c);

        // ---- ① 两种方向：backface 用真实方向，可见性用 biased 方向 ----
        float3 toP    = normalize(P - x);                            // 无 bias
        float3 biased = (x - P) + (n + 3.0 * wo) * kNormalBias;       // kNormalBias = 0.25
        float3 dirVis = normalize(-biased);                          // 查距离矩的方向
        float  t      = length(biased);                              // 可见性查询用的距离

        float w = 1.0;

        // ---- ② backface wrap shading ----
        w *= pow2( max(1e-4, (dot(toP, n) + 1.0) * 0.5) ) + 0.2;

        // ---- ③ Chebyshev 可见性 ----
        float2 m = SampleBilinear(depthAtlas, p, OctEncode(dirVis));
        float  var  = abs(m.x * m.x - m.y);
        float  cheb = var / (var + pow2(max(t - m.x, 0.0)));
        cheb = max(pow3(cheb), 0.0);
        w *= (t <= m.x) ? 1.0 : cheb;

        // ---- ④ 低辐照度 crush（必须在三线性之前） ----
        w = max(1e-6, w);
        if (w < 0.2) w *= w * w * 25.0;                     // = w³ / 0.2²

        // ---- ⑤ 三线性 ----
        float3 tri = lerp(1.0 - alpha, alpha, (float3)off);
        w *= tri.x * tri.y * tri.z;

        // ---- ⑥ 取辐照度（沿法线）+ 感知空间混合 ----
        float3 E = SampleBilinear(irrAtlas, p, OctEncode(n));
        sumE += w * sqrt(max(E, 0.0));                       // 平方根空间加权
        sumW += w;
    }

    if (sumW == 0) return 0;
    float3 outE = pow2(sumE / sumW) * kEnergyPreservation;    // 0.85，回到线性
    return outE * kFinalScale;                                // ← 用 white furnace 标定（§4.2）
}
```

**逐条解释（每一条都对应论文 Figure 7 的一格）**

| 步 | 名称 | 式子 | 作用 / 为什么是这个形式 |
|---|---|---|---|
| ② | backface wrap | `((dot(n,toP)+1)/2)² + 0.2` | 朴素做法 `max(0, dot)` 在房间内有小物件时，物件的法线可能把**所有**互可见的探针都判成背面 → 全黑。wrap shading 保证每个探针至少保留 0.2 |
| ③ | Chebyshev | `σ²/(σ²+(t-μ)²)` 再 `³` | §4.3。保守（偏漏光而非偏暗） |
| ④ | crush | `w<0.2 → 25w³` | 人眼对暗处的微量漏光极敏感。相对衰减是 `(w/0.2)²`：`w=0.1 → 0.025`。**必须在三线性之前**，否则会连空间过渡一起压掉 |
| ⑤ | 三线性 | 三轴 `lerp(1-α, α, off)` 相乘 | 摊平探针间的台阶 |
| ⑥ | sqrt 混合 | 逐探针 `sqrt(È)`，归一化后再平方 | 论文没写的实现细节。在近似感知空间做加权平均，软化探针间的亮度台阶。**必须"先逐项变换、再平均、再变回来"**，先平均再变换是错的 |

> 官方的 `weight *= ...` 全链路之后**没有** `/= sumW` 之外的额外归一化；`sumE/sumW` 那一步就是你要的归一化（我 §4.4 提到的 `irr /= wsum`）。

#### 三个常数（论文没给，只有代码给）

| 常数 | 值 | 影响 |
|---|---|---|
| `normalBias` | **0.25** | 太小 → 明暗漏光；太大 → 采样能穿过薄遮挡物 |
| `energyPreservation` | **0.85** | 多帧递归的能量补偿，避免多弹射越积越亮 |
| `energyConservation`（更新侧） | **0.95** | 每条射线辐亮度 ×0.95，抑制递归累积 |

后两个都是**经验值**，移植时当可调参数。

---

### 4.6 【架构约束】多弹射必须 ping-pong 两张 atlas

#### 问题

论文 §5.3：多次弹射靠"用上一帧的辐照度场去点亮这一次的 surfel"。也就是 `TraceRadiance` 命中点着色时需要**采样探针图集**。

而现在的流程是 `ProbeUpdate(写 atlas) → BorderFixup(写 atlas) → Lighting(读 atlas)`。`ProbeUpdate` **一边写一边又要读**同一张 UAV 纹理 —— 同一个 dispatch 里对同一资源的读写是未定义行为（无法保证邻居探针本帧已更新完）。

#### 两个方案

| 方案 | 做法 | 评价 |
|---|---|---|
| **A. ping-pong（推荐）** | 两张 atlas，`cur = frame & 1`。`ProbeUpdate` 读 `atlas[1-cur]`、写 `atlas[cur]`；`Lighting` 读 `atlas[cur]` | 物理正确；显存代价极小（辐照度 80×320×8B = **200 KB**，翻倍 +200 KB） |
| B. 单图 + 容忍撕裂 | 单图，read-write 同图 | 不接受，会出现闪烁的空间图案 |

#### 对现有结构的影响（提前知道就不会踩）

- `ProbeGrid` 要持有 **2 × `RenderTexture`** 的辐照度图集，接口改成 `GetResource(slot)` / `GetSRVIndex(slot)` / `GetUAVIndex(slot)`
- `App` 现在在 `Init` 时把 `m_probeGrid.GetSRVIndex()` 一次性传给 `LightingPass`（`App.cpp:254`）。ping-pong 之后 SRV 索引**每帧不同**，必须改成每帧通过 `FrameContext` 或 `SetProbeAtlas()` 传递
- `RenderGraph` 里要为两张图各注册一个 `ResourceId`（现在只注册了一张，`App.cpp:180-182`）

> **P5 才需要做。** P1~P4 用单图完全够（探针射线只看直射光）。但**请在 P4 结束前把 `ProbeGrid` 的接口设计成"按 slot 取"**，否则 P5 会牵动一大片。可以照抄你现有的 `m_lightingPass.SetBackbuffer(...)` 模式（`App.cpp:357`）。

---

### 4.7 官方常数总表（可直接抄）

| 参数 | 值 | 出处 |
|---|---|---|
| 辐照度 tile 边长 `S` | **8** | `IrradianceField.h:16` |
| 距离 tile 边长 `S` | **16** | `:17` |
| `normalBias` | **0.25** | `:37` |
| `hysteresis` | **0.98**（论文 0.85~0.98） | `:44` |
| `depthSharpness`（`cos^k` 的 k） | **50.0** | `:49` |
| 每探针每帧射线数 | **64** | `:52` |
| `recursiveEnergyPreservation`（采样侧） | **0.85** | `.cpp:10` |
| `energyConservation`（更新侧） | **0.95** | `.pix:70` |
| `epsilon`（权重阈值） | **1e-6**（论文写 1e-3） | `.pix:43` |
| `crushThreshold` | **0.2** | `.pix:214` |
| `rayMinDistance`（tMin） | **0.08** | `generateRandomRays.pix:101` |
| `hitBias`（命中点沿法线外推） | **0.01** | `.pix:84` |
| 权重下限 | **1e-6** | `.pix:199` |
| `maxDistance` | `1.5 × |bbox / probeCounts|` ≈ 1.4~1.6 | `.cpp:166` |

本工程对照：`hysteresis` 实际 0.95（α=0.05）✅ 落在论文区间；`maxDistance` 现在是 **100** ❌；其余均未实现。

---

### 4.8 hysteresis 的方向：论文 Eq(1) 写反了，你的代码是对的

官方：

```cpp
args.setUniform("hysteresis", m_firstFrame ? 0.0f : m_specification.hysteresis);   // 首帧 0
rd->setBlendFunc(BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA);
```
```glsl
result.xyz /= result.w;
result.w = 1.0f - hysteresis;      // 输出的 alpha
```
⇒ `new = (1 - h)·新值 + h·旧值`，`h = 0.98` 时新值只占 2%。

论文正文说"alpha-blending the new results **at a rate of 1−α**"——与代码一致。
但论文 **Eq(1)** 写作 `lerp(old, Σ…, hysteresis)`，按 `lerp(a,b,t)=(1-t)a+tb` 语义新值权重应为 `hysteresis`，**与正文和代码都相反**。以代码为准。

**你的代码**：`atlas[pixel] = lerp(old.rgb, radiance, alpha)`，`alpha = 0.05` ⇒ `0.95·old + 0.05·new`，即 `h = 0.95`。**方向完全正确**，落在论文区间内。改造时只要保留这个方向、首帧仍然是"直接覆盖"即可。

> D3D12 里推荐在 compute 里显式 `lerp`，不要用 blend state（UAV 和 blend 不能并存；官方那套依赖 ROP 的 render-to-texture）。

---

## 5. 分阶段实施计划

原则：**每个阶段结束时都能编译、能跑、能用画面或数值判断对错**。不要跨阶段并行。

### P0 · 止血：让它编过、跑得起来，画面与现在完全一致

**目标**：MSBuild 通过 → 程序启动 → `debugMode 8/9` 能看到探针图集 → 最终画面仍是现在的纯光追 GI（对照组）。

**改动点**

1. **统一 `ProbeBorderFixupPass::Init` 的契约**（消除 C2660 + 6×C2065）
   现在头文件 11 参、`.cpp` 用 `m_countX/Y/Z`、`App.cpp` 传 4 参，三方各说各话。
   **建议改成持有 `ProbeGrid*`**：`ProbeGrid` 已经提供 `GetDesc()` / `GetAtlasWidth()` / `GetAtlasHeight()`，count 和宽高都能现算，pass 里不必再存副本（"同一信息存两份"正是这次不一致的成因）。

   ```cpp
   // 建议形态：既能服务辐照度图集（stride 10），也能服务距离矩图集（stride 18）
   void Init(ID3D12RootSignature* rootSig, ID3D12PipelineState* pso,
             ProbeGrid* probeGrid,
             RenderGraph::ResourceId atlasId,
             ID3D12Resource* atlasResource,   // UAV barrier 用，两张图集不同
             uint32_t uavIndex,
             uint32_t stride,                 // 10 或 18
             uint32_t tileSize);              // 8 或 16
   ```

2. **`App.cpp:214-216` 对齐调用点**：传 `m_probeGrid.GetUAVIndex()`、`ProbeGrid::kTileStride`。

3. **`ShadersProbe.hlsl` 修字段名**（消除 11 条 DXC 错误）
   - `BorderFixupCS:112`：`g_Probe.gridCountsAndFrame.xyz` → `uint3(g_Probe.countX, g_Probe.countY, g_Probe.countZ)`
   - `BorderFixupCS:113`：`const uint stride = 10;` → `const uint stride = g_Probe.borderTileStride;`
   - `BorderFixupCS:131`：`g_Probe.misc.w` → `g_Probe.borderTargetUav`
   - `ProbeDistanceCS`：**整段删除**（不是注释掉）。没有 C++ 调用者，留着只会继续毒化这个 TU。P4 时按新线程模型重写。

4. 顺手修掉零成本项
   - `#include "Pass.h"` → `"pass.h"`（`ProbeUpdatePass.h:1`、`ProbeBorderFixupPass.h:2`、`RenderGraph.cpp:2`）
   - `LightingTypes.h:5` / `ShadersCommon.hlsl:232` / `LightingPass.cpp:77` 三处过期注释（32B / 48B / "12→16"，实际都是 64B）

**验证**

```powershell
# 1) 构建
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  RenderingProgrameByHand.sln /t:Build /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
# 期望 exit=0

# 2) 手工验 shader（必须！MSBuild 不编译 hlsl）
$dxc = "packages\Microsoft.Direct3D.DXC.1.9.2609.5\build\native\bin\x64\dxc.exe"
$inc = "RenderingProgrameByHand"
& $dxc -I $inc -E ProbeUpdateCS -T cs_6_6 "$inc\ShadersProbe.hlsl"    # 期望 exit=0
& $dxc -I $inc -E BorderFixupCS -T cs_6_6 "$inc\ShadersProbe.hlsl"    # 期望 exit=0
```

```
# 3) 运行时
按 9 → debugMode 8：看一眼探针辐照度（现在是"每 texel = 该方向一条射线的 radiance"，
       会有明显方向性条纹，正常）
按 0 → debugMode 9：看 atlas 布局，magenta 线应正好落在每个 tile 边界
按 1 → debugMode 0：画面应与改造前一致
```

**风险**：不动算法，画面**不应该**有任何变化。变了说明摸到了别的东西。

---

### P1 · 让探针真正产出"辐照度"

**目标**：探针图集的物理意义正确、数值可验证。**画面仍不变**（还没人消费）。

**改动点**

1. **`ShadersProbe.hlsl` 的 `ProbeUpdateCS` 按 §4.1 重写**
   - `numthreads` 保持 `(8,8,1)`，改用 `SV_GroupThreadID` + `SV_GroupID` 取 `texel` / `probeIndex`
   - 内层 `for` 遍历 `g_Probe.rayCount` 条 `StochasticFibonacci`
   - 余弦加权累加 + `/= accW`（§4.2）
   - `w < 1e-6` 跳过；`accW < eps` 时 `return`（保留旧值）
   - 首帧**直接赋值**而不是 `lerp`（§6.5）
   - 射线起点用 `tMin = 0.08`（见下面的小重构）
   - 每条射线辐亮度 × `0.95`（`energyConservation`）

2. **`ShadersCommon.hlsl` 的小重构**：三个光追辅助函数都把 `ray.TMin = 0.0f` 写死了（`TraceSunShadow:201`、`TraceHitDistance:218`、`TraceRadiance:311`），`TMax` 分别是 `100.0f` / 入参 / `100.0f`。探针射线需要 `TMin = 0.08`、`TMax = maxDistance`。**给 `TraceRadiance` / `TraceHitDistance` 各加 `tMin`、`tMax` 两个参数**，灯光那边的调用点传原来的值即可（`TraceSunShadow` 可不动）。

3. **旋转源**：官方是每帧一个全局随机旋转。最省事的实现是给 `ProbeUpdateParams` 加一个 `float2 rayRotation`（或 `float4`，还要随机轴），CPU 侧每帧用 `std::mt19937` 生成。也可以直接由 `frameIndex` 哈希出两个角，省一个字段：
   ```hlsl
   float2 rot = float2(Hash01(uint2(0,0), g_Probe.frameIndex),
                       Hash01(uint2(0,1), g_Probe.frameIndex));
   ```

4. **`ProbeUpdatePass.cpp`**：`p.rayCount` 从硬写 64 改成成员 `m_rayCount`，方便调参。

**验证**

```
① white furnace：SkyColor()=(1,1,1)，关直射光，albedo 全 1
   → debugMode 9 里 atlas 内部 texel 应全接近 1.0（§4.2 的表）
② 单色环境：SkyColor()=(0.2,0.3,0.5) → atlas 应均匀呈该颜色，无方向条纹
③ 恢复 Cornell Box + 直射光
   → debugMode 8：朝向地板/朝向天花板的 texel 明暗应不同，符合场景
   → 连跑 200 帧，纹路应几乎不变（已收敛），而不是持续抖动
```

**风险**：射线数从 64 涨到 `64 texel × 64 ray` = 4096/探针，比现在慢 64 倍。256 探针 × 4096 = 104 万射线/帧，粗估 0.7~1.5 ms。
掉帧不可接受时**不要退回单射线**，先降 `rayCount` 到 32/16（噪声变大但语义仍正确），或提前做 §4.1 的 `groupshared` 优化。

---

### P2 · 让画面用上探针（第一次看到 DDGI）

**目标**：最终着色读探针。先做**最近 1 个探针**，把链路打通，P3 再升级到 8 探针三线性。

**为什么先 1 个**：8 探针 + 归一化 + 5 项权重一次性写进去，一旦画面不对，你无法判断是权重错、UV 错、还是 cage 索引错。分两步，每步只有一个变量。

**改动点**

1. **新增 `SampleProbeIrradiance()`**，放 `ShadersCommon.hlsl` 末尾（`OctEncode/OctDecode` 已在里面，天然归属）。

2. **`ShadersCommon.hlsl` 补一行** `sampler g_ClampSampler : register(s1);`（§4.4）

3. **`ShadersLighting.hlsl` 的 `LightingPS` 接入**
   - 插入位置：`world` 重建之后（`:56` 之后）、`giSamples` 循环之前（`:116` 之前）
   - 合并位置：`:134` 的 `indirect` 项
   - **保留原来的 `TraceGI` 并加开关**，否则会失去唯一的对照基准
   - 新增 `debugMode == 10`："只显示探针间接光"，与 `debugMode == 6`（光追 GI）并排对比

4. **stride 常量**：`ShadersLighting.hlsl:86,99` 各写了一遍 `const uint stride = 10;`。正式采样也要用。**图形根签名只剩 1 个 DWORD**（§1.3），所以只能硬编码。建议在 `ShadersCommon.hlsl` 定义 `static const uint kProbeTileStride = 10;`，并与 `ProbeGrid::kTileStride` 成对维护、加互相指认的注释。

**验证**

```
① debugMode 10：应看到"低频的、跟场景大形状一致"的亮度分布。
   块状立方格纹是正常的（P3 解决）
② 与 debugMode 6 并排按：两者整体亮度量级应该接近
   差 4 倍 → 回 P1 查归一化
   几乎全黑 → SRV 索引错 / 图集状态没切到 PIXEL_SHADER_RESOURCE
③ 相机移动时探针 GI 不应随视角变化（世界空间量）。跟着变说明误用了 view 相关量
```

---

### P3 · 8 探针三线性 + 权重链（消除块状）

**改动点**（全部在采样函数里，C++ 侧零改动）

按 §4.4 + §4.5 实现完整链路：cage → 三线性 → backface wrap → Chebyshev（P4 前可先跳过）→ crush → oct 双线性 → sqrt 混合 → `/= sumW`。

P3 可以先跳过 Chebyshev（距离矩还没通），其余全上。

**验证**

```
① 块状格纹消失
② 两面墙夹角处不应出现"发黑的一圈"（没归一化 sumW 就会有）
③ 地板上的间接光随离墙距离平滑衰减
④ debugMode 10 与 debugMode 6 的间接光**空间分布形状**应该相似（不是逐像素相同，但低频结构一致）
```

---

### P4 · 距离矩 + Chebyshev 可见性（消除漏光）

**改动点**

1. **重写 `ProbeDistanceCS`**（`ShadersProbe.hlsl`，先删后写）
   - `numthreads(16,16,1)`、`probeIndex = gid.x`、`texel = gtid.xy`（0..15）
   - **删掉 `if (id.x >= atlasW ...)` 的图集空间边界检查**——一 group 一探针时 tile 内 16×16 天然不越界，那段检查会杀掉 96.5% 的线程
   - 权重 `pow(max(0, dot(texelDir, rayDir)), 50)`，`< 1e-6` 跳过
   - 距离通道有**自己的 Σw**，不能和颜色通道共用
   - `tMax = maxDistance`（§4.3，≈1.4~1.6，**不是 100**）
   - miss 时 `d = maxDistance`
   - 命中点沿法线外推 `0.01`（`hitBias`）再算 `length(probePos - hitPos)`
   - 写 `float4(mean, mean2, 0, 1)`

2. **`ProbeTypes.h` + `ShadersProbe.hlsl` 的结构体改造**
   - `borderTileStride` 一个字段被两个 pass 用成两种含义（§6.4）→ 拆成 `irradianceStride` / `distanceStride`
   - `distanceUav`（偏移 48）**必须真正被填**，否则是越权写的炸弹（§6.3）

3. **新建 `ProbeDistancePass`**（照 `ProbeUpdatePass` 写）
   - `ProbeDistancePass.h` 现在是 1 行 `#pragma once` 的空壳
   - 需要：`Init` / `Declare`（`ReadWrite(distanceId, UNORDERED_ACCESS)`）/ `Execute`（填参数 + `Dispatch(probeCount,1,1)` + UAV barrier）

4. **`App` 侧接线**
   - 注册距离矩图集：`m_probeDistanceId = m_graph.RegisterResource("ProbeDistance", m_probeGrid.GetDistanceResource(), UNORDERED_ACCESS)`
   - 编译 `ProbeDistanceCS`、建 PSO
   - `AddPass` 序列插到 `ProbeUpdate` 之后、`BorderFixup` 之前
   - **`ProbeBorderFixupPass` 需要第二个实例**（stride 18、tileSize 16、UAV = `GetDistanceUAVIndex()`）——这正是 P0 把 `Init` 参数化的原因

5. **采样侧加 Chebyshev**（§4.3 公式）+ normal bias（§4.5 的 `(n + 3·ω_o)·0.25`）

> 注意 `SampleIrradianceField` 需要**视线方向 `ω_o`**，而 `LightingPS` 里只有 `pin.uv` 和世界坐标。`normalize(cameraPos - worldPos)` 即可，`g_Frame.cameraPos` 已经在 b1 里。探针更新侧如果也调用同一个函数（多弹射时），传入的是"从 surfel 指向探针中心"——论文 §4.3 明确说了这一点。

**验证**

```
① 加 debugMode 11：把距离矩 mean/maxDistance 画出来
   → 应能看出"离墙近处暗、开阔处亮"的空间结构
② Cornell Box 两个球的**间接阴影**应出现（DDGI 最标志性的效果）
③ 墙角不应过度变暗
④ kSharpness 从 50 调到 200，间接阴影应变锐；调到 5 应变糊
   → 完全无变化说明距离权重或采样方向有一处是错的
```

---

### P5 · 多弹射（无限弹射收敛）

**改动点**

1. `ProbeGrid` 改 ping-pong（§4.6）
2. `ProbeUpdateCS` / `BorderFixupCS` 增加"读哪张、写哪张"的参数
3. `TraceRadiance` 命中点着色里加探针间接项（**采样上一帧那张**），并用 `0.85`（`energyPreservation`）压制递归累积
4. `App::RenderTick` 每帧推进 slot，传给 update / fixup / lighting

**验证**

```
① 红球 + 白墙：白墙上的红色渗染应比 P4 明显（多次弹射的 color bleeding）
② 全封闭白盒（wall albedo=1）+ 一盏灯：收敛后角落亮度应显著高于单弹射
③ 静态场景静置 200 帧后画面稳定；把 atlas 直接输出检查无 NaN
```

---

### P6 · 优化（可选）

| 优化 | 收益 | 前提 |
|---|---|---|
| `groupshared` 复用射线结果 | 射线数 ↓64× | 要求 `rayCount == 64 == 线程数` |
| 探针节流（只更新相机附近 / LOD） | 线性降 | 论文 §7.1 列为未来工作 |
| 探针 active/inactive 分类 | 少算无效探针 | 论文不要求；Chebyshev 上线后大部分自愈 |
| 异步 compute 队列 | 与图形并行 | 增加跨队列同步复杂度，当前单队列是最简方案 |

---

## 6. 必须遵守的工程约束（不遵守一定踩坑）

### 6.1 三套"帧号"，语义完全不同

| 帧号 | 定义 | 语义 | 能否用于跨帧累积 |
|---|---|---|---|
| `m_context.GetFrameIndex()` | `RenderContext.cpp` | **环形槽位 0..2** | ❌ 每 3 帧归零 |
| `App::m_frameCounter++` | `App.cpp:319` | 单调递增，写进 `g_Frame.frameIndex` | ✅ |
| `ProbeUpdatePass::m_frameIndex++` | `ProbeUpdatePass.cpp:38` | 单调递增，写进 `g_Probe.frameIndex` | ✅（当前用的就是它，正确） |

做 hysteresis、射线旋转、首帧判断时**必须**用后两个。用 `ctx.frameIndex` 会让 EMA 每 3 帧被 `alpha=1` 冲掉一次。
建议在 `ProbeUpdatePass::m_frameIndex` 上加注释固化这个约定（现在没有）。

### 6.2 两套资源状态账本

`RenderGraph::m_resources[].state` 与 `RenderTexture::m_state` **互不同步**。`RenderGraph` 改状态时直接对裸 `ID3D12Resource*` 发 barrier，不回写 `RenderTexture`。

后果：`ProbeGrid::m_atlas.m_state` 永远停在 `UNORDERED_ACCESS`（创建时的值），而 GPU 真实状态每帧在 `UNORDERED_ACCESS ↔ PIXEL_SHADER_RESOURCE` 来回。

**约束：探针图集的状态迁移一律走 RenderGraph 的 `ReadWrite`/`Read` 声明，永远不要调 `ProbeGrid` 内部 `RenderTexture::TransitionTo`。** 一旦调用，`StateBefore` 是错的。

### 6.3 `distanceUav = 0` 是"静默越权写"炸弹

`ProbeUpdateParams p{}` 值初始化，未赋值字段为 0。`distanceUav` 从没被写过 ⇒ 恒 0。
一旦有 shader 用 `ResourceDescriptorHeap[g_Probe.distanceUav]`，就会把**第 0 号描述符（G-Buffer albedo 的 SRV）当 UAV 写**——不报错，只在某天"G-Buffer 莫名其妙花掉"。

**P4 接线时第一件事就是确认 `p.distanceUav` 被赋了真值**，并在 `Execute` 里加 `assert`。

### 6.4 一个字段两种语义：`borderTileStride`

CPU 注释说它是"10（irradiance）或 18（distance）"（`ProbeTypes.h:21`），但 `ProbeDistanceCS` 又把它当成"自己图集的 stride"用，然后在函数中途被 `const uint stride = 18` 覆盖。

**改造时拆成两个字段**，否则 `stride` 到底指谁永远说不清。

### 6.5 首帧未初始化 → NaN 传播

`RenderTexture::InitUAV` 用 `nullptr` 作 clear value（`RenderTexture.cpp:93`），内容**未定义**。而 shader 第一帧仍然先读 `old` 再 lerp：

```hlsl
float4 old = atlas[pixel];
float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;
atlas[pixel] = lerp(old.rgb, radiance, alpha);
```

数学上 `alpha == 1` 丢弃 `old`，但 IEEE 里 `lerp(NaN, x, 1) = NaN + 1·(x - NaN) = NaN`。RGBA16F 的未初始化显存完全可能是 NaN/Inf 位型 ⇒ **首帧就把 NaN 写进图集，之后永远传染**。

**修法（二选一）**
- shader 侧：`atlas[pixel] = (frameIndex == 0u) ? float4(irr, 1.0) : float4(lerp(old.rgb, irr, alpha), 1.0);`
- 或 `ProbeGrid::Init` 之后录一次 `ClearUnorderedAccessViewFloat`（多一次 GPU 往返，不推荐）

**推荐前者**，零成本。

### 6.6 `.hlsl` 没被部署到输出目录

`x64\Debug\` 下只有一份**过期的** `Shaders.hlsl`（来自已删除的文件），没有 `ShadersProbe.hlsl` / `ShadersCommon.hlsl`。NuGet 的 `.targets` 只复制 `dxcompiler.dll / dxil.dll`。

⇒ 从 VS 启动（CWD = `$(ProjectDir)`）正常；**直接双击 exe 会因 `LoadFile` 失败而启动不了**。

修法：vcxproj 加 `<Content Include="*.hlsl" CopyToOutputDirectory="PreserveNewest" />`，但 `CompileShaderSM66` 传的是相对路径，CWD 不对照样找不到 —— **根本方案是把 shader 目录作为 `-I` 传入，或用 `GetModuleFileName` 拼绝对路径**。

### 6.7 其余零成本修正

- `#include "Pass.h"` → `"pass.h"`（3 处）
- `RegisterResource` 加 `assert(id < m_resources.size())`（`RenderGraph.cpp:102` 无 bounds check，传 `kInvalidId` 直接越界）
- 三处过期注释
- `App.cpp:122,160,195,208` 的 `assert(!blob.empty())` 是**无效断言**——`CompileShaderSM66` 失败时是 `throw` 不是返回空 vector

### 6.8 一个待确认项（低优先）

`RootSignatureBuilder::Build` 用 `D3D_ROOT_SIGNATURE_VERSION_1_0` + `desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`（`RootSignature.cpp:52,59`）。这个 flag 是随 1.1 引入的，1.0 序列化是否接受它值得确认。

工程现在能跑，所以大概率是被接受或忽略了；但如果哪天遇到"`ResourceDescriptorHeap` 索引异常"或 Debug Layer 报根签名相关的错，**第一个怀疑对象就是这里**——改成 `D3D12SerializeVersionedRootSignature` + versioned desc 并显式用 1.1 即可。

---

## 7. 验证工具箱

### 7.1 三张"体检表"

| 测试 | 怎么设 | 看什么 | 说明什么 |
|---|---|---|---|
| **White furnace** | SkyColor=(1,1,1)，关直射，albedo=1 | 探针 texel ≈ **1.0** | 归一化 + cos 权重 + oct 编解码全对（§4.2 的表） |
| **单色环境** | SkyColor=(0.2,0.3,0.5) | atlas 均匀无条纹 | 射线分布均匀、无结构性 bias |
| **Cornell Box 对照** | 正常场景 | debug 6（光追）vs debug 10（探针）亮度量级接近 | 整体校准正确 |

### 7.2 建议新增的 debugMode

| 值 | 内容 | 用途 |
|---|---|---|
| 10 | 只显示探针间接光（`albedo * probeIrr`） | 与 debug 6 并排对比 |
| 11 | 距离矩 `mean / maxDistance` 可视化 | P4 验证距离矩在写 |
| 12 | 可见性项单独输出 | P4 调 Chebyshev |

### 7.3 性能观测

`App::UpdateFrameStats` 已经在窗口标题输出 `WaitGPU / Begin / Record / Execute / Present / WaitHit`。P1 之后重点看：
- **Record** 几乎不变（CPU 录制，dispatch 数没变）
- **Execute** 涨多少（这才是 GPU 上探针更新的真实代价）
- 如果 `Execute` 暴涨到 10ms+ 而你以为探针才 1ms，去查 `TraceRadiance` 里的阴影射线——每条探针射线都额外打了一条 `TraceSunShadow`

### 7.4 需要一个 GPU 抓帧工具

一旦"画面不对但看不出为什么"，PIX / RenderDoc 抓一帧，直接看 atlas 内容。比在 shader 里加 printf 快一个数量级。

---

## 8. 我对几处常见误解的订正

队友的报告质量很高，但有三处说法我核对官方代码后认为会误导实现方向：

### 8.1 ❌ "缺少八面体立体角 Jacobian" —— 不需要

射线方向是**球面上均匀分布**的，texel 方向只在 `max(0, d·ω)` 里作为余弦叶瓣的轴向出现。整个积分 `∫L(ω)max(0,d·ω)dω` 在**方向域**上做，与"texel 覆盖多少立体角"无关——texel 是**存储**格点，不是**积分**格点。官方实现也不乘任何 Jacobian。加上它反而会让角部 texel 被系统性放大。

### 8.2 ❌ "跨 tile 线性采样有缝 / 需要八面体接缝重建" —— 不存在

每探针是独立 tile，UV 按 tile 计算（§4.4），**永远不跨 tile 读取**。gutter 只服务 tile **内部**的双线性，±1 复制正是正确做法（等价 clamp-to-edge）。不要为它立项。

### 8.3 ❌ "需要四面体插值" —— 论文里没有

论文 §4.1 提到的 "tetrahedral" 是指探针**布置**可以是四面体**网格**，不是插值方式。官方实现就是 `for i in 0..7` 的八点三线性。**已确认不存在四面体插值。**

### 8.4 ✅ 三处认知纠偏，我确认队友是对的

1. `ProbeTypes.h` 与 HLSL 结构体**声明层完全一致** —— 对，错在函数体
2. `ProbeUpdateCS` 的线程映射**正确** —— 对
3. `DepthBuffer.cpp` 被写在 `<ClInclude>` 段落里**不是**缺陷（ItemType 由元素名决定）—— 对

### 8.5 ⚠ 一处小事实错误

`01-probe-core-audit.md` §2 说 `ProbeBorderFixupPass.h` 的 `Init` "收 9 个参数"，实际是 **11 个**（§3.1 里自己列的也是 11 个）。以 §3.1 为准。

---

## 9. 一页速查：改哪个文件，做什么

| 阶段 | 文件 | 改什么 |
|---|---|---|
| **P0** | `ProbeBorderFixupPass.h` | `Init` 改成收 `ProbeGrid*` + atlasId + atlasResource + uav + stride + tileSize；私有成员同步 |
| | `ProbeBorderFixupPass.cpp` | `Execute` 里从 `ProbeGrid` 现算 counts/宽高，不再用 `m_countX/Y/Z` |
| | `App.cpp:214-216` | 调用点对齐新签名 |
| | `ShadersProbe.hlsl` | `BorderFixupCS` 三处字段名；**删除**整个 `ProbeDistanceCS` |
| | （顺手）`ProbeUpdatePass.h` / `ProbeBorderFixupPass.h` / `RenderGraph.cpp` | `"Pass.h"` → `"pass.h"` |
| **P1** | `ShadersProbe.hlsl` | `ProbeUpdateCS` 重写：group=探针、texel 循环 N 条 `StochasticFibonacci`、余弦加权、`/Σw`、首帧直接赋值、`tMin=0.08`、`×0.95` |
| | `ShadersCommon.hlsl` | `TraceRadiance` / `TraceHitDistance` 增加 `tMin`、`tMax` 参数 |
| | `ProbeUpdatePass.h/.cpp` | `m_rayCount` 成员；每帧生成全局随机旋转 |
| **P2** | `ShadersCommon.hlsl` | 补 `sampler g_ClampSampler : register(s1);`；新增 `SampleProbeIrradiance()`（最近 1 探针）；`kProbeTileStride = 10` |
| | `ShadersLighting.hlsl` | `LightingPS` 插入探针采样；加 `debugMode == 10` |
| **P3** | `ShadersCommon.hlsl` | 采样函数升级：8 探针 cage + 三线性 + backface wrap + crush + oct 双线性 + sqrt 混合 + `/sumW` |
| **P4** | `ShadersProbe.hlsl` | 重写 `ProbeDistanceCS`（16×16、`cos^50`、正确线程模型、`tMax=maxDistance`、独立 `Σw`） |
| | `ProbeTypes.h` / `ShadersProbe.hlsl` | 拆 stride 字段；确保 `distanceUav` 被填 |
| | 新建 `ProbeDistancePass.h/.cpp` | 照 `ProbeUpdatePass` 写 |
| | `App.h/.cpp` | 注册距离矩 ResourceId；编译 `ProbeDistanceCS`；建 PSO；`AddPass` 插位；**再加一个 Fixup 实例**（stride 18） |
| | `ShadersCommon.hlsl` | 采样函数加 Chebyshev + normal bias `(n+3·ω_o)·0.25` |
| | `ShadersLighting.hlsl` | 传 `ω_o = normalize(cameraPos - worldPos)` 进采样函数 |
| **P5** | `ProbeGrid.h/.cpp` | 辐照度图集 × 2（ping-pong），接口改成 `Get*(slot)` |
| | `App.h/.cpp` | 每帧推进 slot，传给 update / fixup / lighting |
| | `ShadersCommon.hlsl` | `TraceRadiance` 命中点加探针间接项（读上一帧）× 0.85 |
| | `ShadersProbe.hlsl` | update 读 slot^1、写 slot |
| **全程** | `ProbeUpdatePass.cpp` | `m_frameIndex` 加注释固化"这是单调帧号" |

---

## 10. 如果只有一天时间

按这个顺序做，能拿到"能跑 + 能看到 DDGI"：

1. **P0 全部**（约 1 小时，纯机械）
2. **P1 的 `ProbeUpdateCS` 重写**（约 2 小时）—— 不做这个，后面看到的都是错的
3. **White furnace 验证**（约 15 分钟）—— 不做这个，你会带着 4 倍亮度误差走完全程
4. **P2 的最近 1 探针采样**（约 1 小时）
5. 按 `9`、`6`、`1` 反复对比三个 debug 视图

P3 的三线性 + 权重链、P4 的 Chebyshev 是"从能看"到"看得过去"的分水岭，各需要半天起。P5 是"从看得过去"到"接近 path tracing"，需要改 C++ 结构，留出整块时间。

---

## 11. 参考实现的复现方式（如果你要自己核对）

```powershell
$tmp = "$env:TEMP\ddgi_supp"; New-Item -ItemType Directory -Force $tmp | Out-Null
curl.exe -sSL -o "$tmp\supplement.zip" "https://jcgt.org/published/0008/02/01/supplement.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead("$tmp\supplement.zip")
$z.Entries | Where-Object FullName -match 'Irradiance' |
    ForEach-Object { [IO.Compression.ZipFileExtensions]::ExtractToFile($_, "$tmp\" + (Split-Path $_.FullName -Leaf), $true) }
$z.Dispose()
Get-FileHash "$tmp\*.pix","$tmp\IrradianceField.*" -Algorithm SHA256
```

> `supplement.zip` 618 MB，其中 590 MB 是演示视频，只需 `supplement/code/` 下 7 个文本文件（~73 KB）。
> 各文件的 SHA-256 见 `04-reference-formulas.md` §0.2。
