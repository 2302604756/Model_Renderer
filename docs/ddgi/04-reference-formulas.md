# 04 · DDGI 参考实现精确公式取证（官方 supplement.zip 逐行核对）

> 角色：技术资料研究员（只读取证，未修改任何工程源码；本文件是 `docs/ddgi/` 下唯一新增文件）
> 时间：2026-10-30（Asia/Shanghai）
> 对象：Majercik et al. 2019, *Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields*, JCGT 8(2)

**本文的定位**：论文正文只给定性描述，**本文给出官方参考实现里的确切表达式、常数与归一化方式**，并逐条标注「论文这么说 / 代码实际这么做 / 两者是否一致」。所有代码片段均来自 2019-06 官方补充材料 `supplement.zip`，可用 §0.3 的命令复现校验。

---

## 0. 取证记录（来源、哈希、复现方式）

### 0.1 来源表

| 编号 | 内容 | URL | 备注 |
|---|---|---|---|
| **S1** | 论文正文（low-res PDF，30 页） | https://jcgt.org/published/0008/02/01/paper-lowres.pdf | 1 244 399 B；本文引用的论文原句均由此 PDF 抽取 |
| **S2** | 官方补充材料（shader + C++ 参考实现） | https://jcgt.org/published/0008/02/01/supplement.zip | 618 445 577 B（其中 590 MB 是演示视频）；sha256 `51CBD6BA…2255A46` |
| **S3** | 采样 shader | `supplement.zip → supplement/code/sampleIrradianceField.pix` | 10 400 B，243 行 |
| **S4** | 探针更新 shader | `supplement.zip → supplement/code/IrradianceField_updateIrradianceProbe.pix` | 3 939 B，119 行 |
| **S5** | 射线生成 shader | `supplement.zip → supplement/code/IrradianceField_generateRandomRays.pix` | 3 065 B，105 行 |
| **S6** | C++ 侧参数与混合设置 | `supplement.zip → supplement/code/IrradianceField.h` / `.cpp` | 11 863 B / 29 681 B |
| **S7** | 网格/索引 helper | `supplement.zip → supplement/code/GridHelpers.glsl` | 13 324 B，357 行 |
| **S8** | 第三方交叉验证（NVIDIA RTXGI，**不是**官方参考实现） | https://github.com/NVIDIAGameWorks/RTXGI-DDGI （`rtxgi-sdk/shaders/ddgi/Irradiance.hlsl`、`include/ProbeOctahedral.hlsl`） | 用于交叉核对 Chebyshev/crush/归一化，本文所有「官方」结论均不依赖它 |
| **S9** | G3D 10 后续版本（论文发表之后的引擎集成） | https://casual-effects.com/g3d/G3D10/build/manual/files.html → `DDGIVolume` 目录（`DDGIVolume.glsl`、`DDGIVolume_updateProbes.glc`、`DDGIVolume_generateRays.glc` 等） | **只有文件清单与 Doxygen 页面，未提供源码浏览器**；本文未从中取得代码，仅作「后续演进」提示 |

### 0.2 文件哈希（SHA-256，便于确认你拿到的是同一版本）

```
sampleIrradianceField.pix                  F1D7FB06427FE38BF3C66E9E1047CB5D238B317621E9A481A327128895F02011
IrradianceField_updateIrradianceProbe.pix  85533CE927185FE6309398ED75DCBE66C983109FB44CAC8FDE1CE3122D99EF3C
IrradianceField_generateRandomRays.pix     06EC94F6F1DB90F017E3C753C0166D439E850E0761F17DAA1D2CFB3F78523589
IrradianceField.h                          308666CFB80AADD54E553B3431EF4150C0DD43AD9387DC905544CE8767A9F242
IrradianceField.cpp                        A05627B4B0192073EEA0347A07EACC5588FAFB99BB5F8534BC09AD1A34A6C4DB
GridHelpers.glsl                           0D84E4BABE4C48EC661B79E7B3A464B481C7C00A7C76C4DB2B3941755D98D0EB
util.glsl                                  3C81002FED63FAE86F710D06014A5FCFEE3F332ED0F6F1199335C40FAADCB77B
```

### 0.3 复现命令（PowerShell，已验证可跑通）

```powershell
$tmp = "$env:TEMP\ddgi_supp"; New-Item -ItemType Directory -Force $tmp | Out-Null
curl.exe -sSL -o "$tmp\supplement.zip" "https://jcgt.org/published/0008/02/01/supplement.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead("$tmp\supplement.zip")
$z.Entries | Where-Object FullName -match 'Irradiance' |
    ForEach-Object { [IO.Compression.ZipFileExtensions]::ExtractToFile($_, "$tmp\" + (Split-Path $_.FullName -Leaf), $true) }
$z.Dispose()
Get-FileHash "$tmp\*.pix","$tmp\IrradianceField.*" -Algorithm SHA256   # 与 §0.2 对照
```

> 提示：`supplement.zip` 解压后 590 MB 是 `DynamicLightFieldProbes.mp4`，只取 `supplement/code/` 下 7 个文本文件即可（总计 ~73 KB）。

---

## 1. 数据结构与 atlas 布局（先对齐坐标系，否则公式全部错位）

### 1.1 每个探针存什么

| 内容 | 分辨率 | 格式 | 含义 |
|---|---|---|---|
| 辐照度 | 8×8 octahedral | 论文 `GL_R11G11B10F`；代码由 `irradianceFormatIndex = 4` 从 `s_irradianceFormats` 里选（该数组内容未随补充材料发布，无法逐项核对） | 该 texel 方向上的**余弦加权平均辐亮度** È（不是 E，见 §2.4 推导） |
| 距离矩 | 16×16 octahedral | 论文 `GL_RG16F`；代码 `RG32F`/`R16F` 可配（`depthFormatIndex=1`） | `x = E[d]`，`y = E[d²]`，d 是探针沿该方向的命中距离 |

> 来源：S1 §3（PDF p.6「8 × 8 octahedral resolution … GL_RG16F format at 16 × 16」）；S6 `IrradianceField.h:16-17, 60-61, 119`。
> S1 Figure 3 说明：每个探针贴图**pixel gutter/border** 一圈，且「additional padding aligns probes on 4 × 4 write boundaries」——**但补充材料代码里没有 4×4 对齐 padding**（见 §1.3、§6 的差异表）。

### 1.2 采样侧的 atlas 寻址（S3 `textureCoordFromDirection()`，行 60-79）

```glsl
// S = probeSideLength（辐照度 8 / 距离 16），W,H = 整个 atlas 尺寸（含 1px 全局边框）
vec2 oct01   = (octEncode(normalize(dir)) + 1.0) * 0.5;          // [-1,1] → [0,1]
float stride = S + 2.0;                                          // 1px gutter × 2
int probesPerRow = (W - 2) / int(stride);                        // (W-2) 就是两侧各 1px 全局边框

vec2 probeTopLeft = vec2(mod(p, probesPerRow) * stride,
                         (p / probesPerRow) * stride) + vec2(2.0); // +2：全局边框 1px + 该探针 gutter 1px

vec2 uv = (probeTopLeft + oct01 * S) / vec2(W, H);               // 注意：乘 S 后仍是"像素单位"，最后统一归一化
```

**关键约定（这条最容易踩坑）**：oct 方块 `[-1,1]²` 映射到内部 `S×S` 像素块的**边到边**范围（`oct01*S` 取值 `[0,S]`），而不是格心。1px gutter 的存在使边界处的双线性插值连续。更新侧则写"格心方向"：

```glsl
// S4 normalizedOctCoord()，行 52-59：格心 → 方向
vec2 oct = (vec2(texel_in_tile) + 0.5) * (2.0 / float(S)) - vec2(1.0);
vec3 texelDir = octDecode(oct);
```

两者严格互逆：`t_pix = 2 + (texelRel + 0.5)`（相对 atlas 的像素坐标），与 `oct01*S + 2` 完全一致。
> 来源：S3:60-79；S4:52-59。采样器绑定为 `Sampler::video()`（双线性 + clamp），见 S6 `.cpp:244-246`——**gutter 是双线性不产生接缝的必要条件**。

### 1.3 本工程现有布局（`ProbeGrid.h`）与参考实现的换算

本工程：`atlasW = countX * (8+2)`，`atlasH = countY*countZ*10`，**没有全局 1px 外边框**，tile 原点 = `(px*10, (py+pz*countY)*10)`。换算成参考实现的形式：

```hlsl
// 本工程：oct01∈[0,1]，tileOrigin 为该 tile 在 atlas 中的像素原点
float2 t_pix = tileOrigin + 1.0 + oct01 * S;   // 与 G3D 的 "+2 + oct01*S" 等价，只差全局边框
float2 uv    = t_pix / float2(atlasW, atlasH); // 交给双线性采样
```

> 来源（本工程，只读观察）：`RenderingProgrameByHand/ProbeGrid.h:11-16, 34-42`。与 S3 的差异仅是「全局外边框」，不影响公式。

### 1.4 octahedral 编解码（补充材料引用了 `octahedral.glsl`，但该文件未随 zip 发布）

zip 内只有 `GridHelpers.glsl`/`util.glsl`，`octEncode`/`octDecode` 来自 G3D 的 `data-files/shader/octahedral.glsl`，**未获取到原文**。下面给出与 S3/S4 用法完全兼容（top-left texel → `(-1,-1)`，与 S8 的 `DDGIGetNormalizedOctahedralCoordinates()` 同一约定）的标准 Cigolle et al. 2014 实现，可直接抄：

```hlsl
// 【推导实现】Cigolle et al. 2014 标准映射；与 G3D octahedral.glsl 的调用约定一致，但不是逐字原文
float2 OctEncode(float3 n) {
    float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    float2 uv = n.xy / l1;
    if (n.z < 0.0) uv = (1.0 - abs(uv.yx)) * sign(uv + 1e-6);
    return uv;                       // ∈[-1,1]²
}
float3 OctDecode(float2 uv) {
    float3 d = float3(uv, 1.0 - abs(uv.x) - abs(uv.y));
    if (d.z < 0.0) d.xy = (1.0 - abs(d.yx)) * sign(d.xy + 1e-6);
    return normalize(d);
}
```

> 来源：S8 `ProbeOctahedral.hlsl:24-73`（NVIDIA 版，逐字核对过符号约定，可作为上面两个函数的行为对照）。

---

## 2. 探针更新：`IrradianceField_updateIrradianceProbe.pix`（官方 = 权威）

调用方：S6 `.cpp:490-536`。**注意这是"每 texel 遍历全部 n 条射线"的 gather 结构，不是"每 texel 打一条射线"**（后者是本工程当前写法，差异见 §7）。

### 2.1 射线方向：球面 Fibonacci + 每帧随机旋转

```glsl
// S5:42-55
Vector3 sphericalFibonacci(float i, float n) {
    const float PHI = sqrt(5) * 0.5 + 0.5;              // = 1.618…（黄金比）
    float phi      = 2.0 * pi * madfrac(i, PHI - 1);    // madfrac(a,b)=frac(a*b)
    float cosTheta = 1.0 - (2.0*i + 1.0) * (1.0/n);     // 从 z=+1 螺旋到 z=-1
    float sinTheta = sqrt(saturate(1.0 - cosTheta*cosTheta));
    return Vector3(cos(phi)*sinTheta, sin(phi)*sinTheta, cosTheta);
}
```

```glsl
// S5:94-105 main()
const float rayMinDistance = 0.08;                       // "should be on the order of the normal bias"
rayOrigin    = float4(probeLocation(probeID), rayMinDistance);   // w = tMin
rayDirection = float4(randomOrientation * sphericalFibonacci(rayID, RAYS_PER_PROBE), inf); // w = tMax
```

随机源（**在 C++ 侧、每帧一次**）：

```cpp
// S6 .cpp:388
args.setUniform("randomOrientation",
    Matrix3::fromAxisAngle(Vector3::random(), Random::common().uniform(0.f, 2 * pif())));
```

* 语义：整个探针场**共用同一个随机旋转**（每次 dispatch 一个随机轴 + `[0,2π)` 随机角），因此 n 条射线的相对分布始终是 Fibonacci 螺旋，只是整体朝向每帧变化 → 时间上把 banding 转成可被 hysteresis 平滑的噪声。
* `rayOrigin.w = 0.08` 是 tMin（射线起点沿方向推 0.08 米），与 `normalBias = 0.25` 同数量级。
* ⚠ 未确认项：G3D `Vector3::random()` 是否返回单位向量，补充材料中无体现。**自研实现请显式归一化旋转轴**，否则 `fromAxisAngle` 得到的矩阵带缩放，`sphericalFibonacci` 的结果不再是单位向量。

> 来源：S5:42-55、S5:94-105、S6 `.cpp:379-395`；论文对应句 S1 §4.2「uniformly sample n spherical directions according to a stochastically-rotated Fibonacci spiral pattern」。

### 2.2 每个 texel 的权重：辐照度用 `cos`，距离矩用 `cos^k`（k=50）

```glsl
// S4:93-99
vec3  texelDirection = octDecode(normalizedOctCoord(ivec2(gl_FragCoord.xy)));
#if OUTPUT_IRRADIANCE
    float weight = max(0.0, dot(texelDirection, rayDirection));            // k = 1
#else
    float weight = pow(max(0.0, dot(texelDirection, rayDirection)), depthSharpness); // k = 50（默认）
#endif
```

```cpp
// S6 IrradianceField.h:46-49
/** Exponent for depth testing. A high value will rapidly react to depth discontinuities,
    but risks of exhibiting banding. */
float depthSharpness = 50.0f;
```

**为什么颜色与距离用不同指数**（这就是论文所谓 "cosine-power lobe"）：

| 通道 | 权重 | 目的 |
|---|---|---|
| 辐照度 | `max(0, cosθ)` | Lambert 余弦卷积**本身就属于辐照度的定义**，必须只在更新时做一次，采样时直接取用 |
| 距离矩 | `max(0, cosθ)^50` | 距离场不是可积的辐射度量，必须"锐化"成近似 delta 的 lobe，否则一个 texel 会混合整个半球的深度 → 均值被远处几何拉大 → 可见性判断失效（过亮/漏光） |

由 `ε ≤ cos^k θ` 可反解 lobe 半角 `θ_max = acos(ε^(1/k))`，这条关系决定了阈值选择（配合 §2.3）：

| k | ε | θ_max | 说明 |
|---|---|---|---|
| 1 | 1e-6 | 90.0° | 辐照度：几乎整个前半球 |
| 50 | **1e-6（代码）** | **40.7°** | 距离：默认实际使用的孔径 |
| 50 | **0.001（论文）** | **29.4°** | 论文正文所述阈值对应的孔径 |
| 100 | 1e-6 | 29.4° | 若把 `depthSharpness` 提到 100，等价于改用论文的 0.001 |

> 来源：S4:96-99、S6 `IrradianceField.h:46-49`；论文原句 S1（PDF p.11）：「we employ an additional **depth sharpening**, warping them according to a **cosine-power lobe** distribution. We do not update texels weighted below a threshold (**we used 0.001**) in the cosine-power lobe distribution.」

### 2.3 阈值与累加：按**权重和**归一化，不是除以射线数

```glsl
// S4:100-117
const float epsilon = 1e-6;             // 行 43
if (weight >= epsilon) {
#if OUTPUT_IRRADIANCE
    result += vec4(rayHitRadiance * weight, weight);                       // rgb 累加 ΣL·w，a 累加 Σw
#else
    result += vec4(rayProbeDistance * weight,
                   square(rayProbeDistance) * weight,
                   0.0, weight);                                            // (Σd·w, Σd²·w, ·, Σw)
#endif
}
if (result.w > epsilon) {
    result.xyz /= result.w;             // ← 归一化：除以 Σw，**不是**除以 RAYS_PER_PROBE
    result.w = 1.0f - hysteresis;       // ← alpha 槽位改写成混合系数（见 §2.6）
}
```

* **归一化结论（重要）**：`texel = Σ_i (L_i · w_i) / Σ_i w_i`，其中 `w_i = max(0, cosθ_i)`。这是一个**余弦加权平均辐亮度**（记为 È），**不是**辐照度 E，也**不是** `ΣL·cos` 的裸和。
  * 若所有 n 条射线分布在上半球，则 `E = π · È`（推导见 §2.4）。
  * 论文 Eq(1) 里写的是裸和 `Σ(max(0,texelDir·rayDir) * rayRadiance)`，**省略了 `/Σw`**（见 §6 差异表）。
* **阈值语义**：`if (weight >= epsilon)` 是**逐射线**的丢弃（不是逐 texel），因此"低于阈值的 texel 不更新"是它的自然结果：若一个 texel 的所有 n 条射线都低于阈值，则 `result.w` 保持 0 → alpha=0 → 混合阶段保持旧值不动。
* **代码与论文阈值不一致**：代码 `epsilon = 1e-6`，论文正文 `0.001`。对辐照度分支两者等价（都≈90°），对距离分支差别是 40.7° vs 29.4° 的 lobe 孔径。**建议实现中把两个阈值分开命名**（`kIrradianceWeightEps` / `kDepthWeightEps`），论文值 1e-3 可作为保守默认。

### 2.4 从归一化结果到辐照度的推导（E = π·È）

设 n 条方向在球面上均匀分布（Fibonacci ≈ 均匀），半球内命中数为 m。蒙特卡洛估计：

```
E  = ∫_hemi L(ω) (n·ω) dω ≈ (4π / n) · Σ_{i: n·ω_i>0} L_i (n·ω_i)
È  = Σ L_i w_i / Σ w_i ,  w_i = max(0, n·ω_i)
Σ w_i ≈ n · E_ω[max(0, n·ω)] = n · (1/4)          // 球面均匀采样时前半球余弦均值为 1/4
⇒ Σ L_i w_i ≈ (n/4π) E ,  Σ w_i ≈ n/4
⇒ È ≈ [(n/4π) E] / (n/4) = E / π
⇒ E = π · È                                       // ★ 采样侧应乘 π（若 BRDF 用 albedo/π）
```

Vanilla Lambert：`L_o = (albedo/π)·E = albedo · È`。也就是说，**参考实现存的是 È，最干净的用法是 Lambert 出射辐亮度 = albedo × 探针值**（着色时乘 π 再除 π 互消）。

> ⚠ 官方代码在此处有其自身约定，**不能盲抄**：
> * S3:242 末尾是 `E_lambertianIndirect = 0.5 * pi * netIrradiance;`（≈ E/2）
> * S3:240 还乘了 `energyPreservation`（S6 `.cpp:10` `recursiveEnergyPreservation = 0.85f`，用于多帧递归衰减）
> * 第三方 S8:195 用 `irradiance *= RTXGI_2PI;`（=2π），注释写「Multiply by the area of the integration domain (hemisphere)」
>
> 三者分别是 π/2、π（物理正确）、2π，相差 4×，属于**各自渲染管线的能量约定**（G3D 的直射光强度约定 + 递归 bounce 衰减；RTXGI 的 `probeIrradianceEncodingGamma` 编码）。**移植时必须以自己的 Lambert BRDF 与直射光约定为准，用「全白环境」做一次能量标定**，不要直接抄常数。

### 2.5 命中距离 d：miss、bias、clamp（决定了 §4 的可见性行为）

```glsl
// S4:74-91
const float energyConservation = 0.95;                                  // 行 70，仅作用于采样到的辐亮度
rayHitLocation += rayHitNormal * 0.01f;                                 // 朝法线外推 1cm，防自遮挡
float rayProbeDistance = min(maxDistance, length(probeLocation - rayHitLocation));
if (dot(rayHitNormal, rayHitNormal) < epsilon) rayProbeDistance = maxDistance;   // miss → maxDistance
```

```cpp
// S6 .cpp:164-166 —— maxDistance 的确定方式（不是手调！）
const Vector3 boundingBoxLengths(spec.probeDimensions.high() - spec.probeDimensions.low());
// Slightly larger than the diagonal across the grid cell
m_maxDistance = (boundingBoxLengths / spec.probeCounts).length() * 1.5f;
```

* `probeLocation` 取自 `rayOrigins` 纹理（不是从网格重算）→ 支持探针重定位/滚动。
* **miss 写 `maxDistance` 而不是 inf/0**：保证了 `E[d] ≤ maxDistance`、`E[d²] ≤ maxDistance²`，否则 RG16F 会溢出、Chebyshev 会退化成 0 或 NaN。默认成员值是 `4.0f`（`IrradianceField.h:103`），运行时按上式覆盖。
* `0.01f` 的外推是"贴面深度噪声"的对策：DDGI 的距离是探针到命中面的**精确距离**，没有深度图量化误差，唯一风险是射线刚好打到与自己共面的几何（如地面），1cm 外推可以把这类自遮挡推出去。它对 d 的影响方向不固定（取决于命中面法线朝向），量级仅 1cm，可忽略。

### 2.6 hysteresis：混合方向必须记准（论文 Eq(1) 的 lerp 参数是反的）

```cpp
// S6 .cpp:498-499, 493/523
args.setUniform("hysteresis", m_firstFrame ? 0.0f : m_specification.hysteresis);  // 首帧强制 0
rd->setBlendFunc(RenderDevice::BLEND_SRC_ALPHA, RenderDevice::BLEND_ONE_MINUS_SRC_ALPHA);
```

```glsl
// S4:114-117 —— 输出 alpha = 1 - hysteresis；若未更新则 alpha 保持 0
if (result.w > epsilon) { result.xyz /= result.w; result.w = 1.0f - hysteresis; }
```

由 `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` 得：

```
Irradiance_new_texel = (1 - hysteresis) · newValue + hysteresis · oldValue
```

* `hysteresis = 0.98`（默认）→ 新值只占 2%；`hysteresis = 0.85` → 新值占 15%。
* **首帧 `hysteresis = 0`** → 直接覆盖（对应 `m_firstFrame`，参数变化时也会触发重置，见 `IrradianceField.h:216-218`）。
* 论文正文：「we update … by alpha-blending in the new shading results **at a rate of 1 − α**, where α is a hysteresis parameter」（α∈[0.85,0.98]）——**与代码一致**。
* 论文 Eq(1) 写作 `lerp(old, Σ…, hysteresis)`，按通常 `lerp(a,b,t)=(1-t)a+tb` 语义，新值权重应为 `hysteresis`，**与正文和代码都相反**。**以代码/正文为准**。
* 实现提示：D3D12 里可以用 `D3D12_BLEND_SRC_ALPHA / D3D12_BLEND_INV_SRC_ALPHA`，但更推荐在 compute 里显式 `lerp`（混合状态 + UAV 不能并存；参考实现的 blend 依赖 ROP 的 render-to-texture）。

### 2.7 更新 pass 的离散化常数（其它容易漏的）

| 常数 | 值 | 位置 | 作用 |
|---|---|---|---|
| `energyConservation` | 0.95 | S4:70 | 每条射线辐亮度乘 0.95，抑制递归能量累积 |
| `rayMinDistance` | 0.08 | S5:101 | 射线 tMin（≈ normalBias 量级） |
| `hitBias` | 0.01 | S4:84 | 命中点沿法线外推 |
| `RAYS_PER_PROBE` | 64 | S6 `IrradianceField.h:52` | 每探针每帧射线数 |
| probe atlas 全局偏移 | 2 px | S3:69-78 | 1px 全局边框 + 1px 探针 gutter |

### 2.8 更新侧完整伪代码（HLSL 风格，可直接落 D3D12 compute）

```hlsl
// 一个线程 = atlas 中一个 texel（含 gutter；gutter 由后续 border-fixup pass 填）
float3 accRGB = 0; float accW = 0; float2 accMoments = 0;
const float EPS = 1e-6, ENERGY = 0.95;

int   probeID  = AtlasTexelToProbe(tid);                  // §1.2 / 本工程 ProbeGrid 公式
float3 texelDir = OctDecode((tid.xy_in_tile + 0.5) * (2.0 / S) - 1.0);

for (int r = 0; r < RAYS_PER_PROBE; ++r) {
    float3 rayDir  = rayDirs[r][probeID].xyz;
    float3 radiance= rayRadiance[r][probeID].rgb * ENERGY;
    float3 hitPos  = rayHitPos[r][probeID].xyz + rayHitN[r][probeID].xyz * 0.01;
    float  d       = rayHitN[r][probeID] == 0 ? maxDistance
                                              : min(maxDistance, length(probePos - hitPos));

    float wIrr   = max(0, dot(texelDir, rayDir));
    float wDepth = pow(max(0, dot(texelDir, rayDir)), depthSharpness);   // 50

    accRGB += radiance * wIrr;  accW += wIrr;                            // 着色通道用 cos 权重
    accMoments += float2(d, d * d) * wDepth;                             // 距离通道用 cos^50（独立权重和！）
}
// 距离通道需要自己的 Σw：
// outMoments = accMoments / sumW_depth
// outAlpha   = (sumW > EPS) ? (1 - hysteresis) : 0
```

> 注意：S4 用同一个 `result.w` 同时服务两个分支（因为两个分支是**两次独立 draw call**，`OUTPUT_IRRADIANCE` 宏切换）。自研实现若想一次 dispatch 写两种 atlas，需要记两个独立的权重和。

---

## 3. 探针采样：`sampleIrradianceField.pix`（官方 = 权威）

主循环 `for (int i = 0; i < 8; ++i)`（S3:120-232）。以下按代码真实顺序给出。

### 3.1 八探针 cage 与三线性权重

```glsl
// S3:111-124
ivec3 baseGridCoord = baseGridCoord(irradianceFieldSurface, wsPosition);   // GridHelpers: clamp(floor((x-start)/step), 0, counts-1)
Point3 baseProbePos  = gridCoordToPosition(irradianceFieldSurface, baseGridCoord);
Vector3 alpha = clamp((wsPosition - baseProbePos) / irradianceFieldSurface.probeStep, Vector3(0), Vector3(1));

for (int i = 0; i < 8; ++i) {
    GridCoord offset = ivec3(i, i >> 1, i >> 2) & ivec3(1);                // 三轴各取 bit0/1/2
    GridCoord probeGridCoord = clamp(baseGridCoord + offset, GridCoord(0), GridCoord(probeCounts - 1));
    ProbeIndex p = gridCoordToProbeIndex(irradianceFieldSurface, probeGridCoord);  // x + y*countX + z*countX*countY
```

```glsl
// S3:143-148
Vector3 trilinear = lerp(1.0 - alpha, alpha, offset);   // offset=0 用 1-α，offset=1 用 α
float weight = 1.0;
```

```glsl
// S3:219-220
weight *= trilinear.x * trilinear.y * trilinear.z;
```

细节：

* `alpha` 用 `(x - baseProbePos)/probeStep`，`baseProbePos` 是**网格位置**（`probeStep*c + probeStart`，`GridHelpers.glsl:211-213`）。
* `gridCoordToProbeIndex` 用 `x + y*countX + z*countX*countY`（`GridHelpers.glsl:122-124`），与本工程 `ProbeGrid.h` 的 `(px, py, pz)` 行优先排布一致。
* 边界处 8 个探针会**重复**（clamp），此时同名探针被多次加权——参考实现**不做去重**，靠后面 `weight = max(1e-6, ...)` 和归一化吸收。
* 三线性因子**不做** `max(ε)` 下限（G3D 版）；第三方 S8:118 则是 `max(0.001f, lerp(...))`。参考实现靠 `weight = max(0.000001, weight)`（S3:199）兜底。
* 四面体插值：**论文与参考实现都没有**。论文 §4.1 只说「probes can be placed according to schemes used in other probe-based algorithms, including tetrahedral **grids**」（探针布置，不是插值），S3 的插值就是八点三线性。→ **本条为"论文中不存在"，不是"未获取到"**。

### 3.2 normal bias（法线 + 视线方向的双项 bias）

```glsl
// S3:132-141
// Bias the position at which visibility is computed; ... If the normal bias is too small,
// there will be light and dark leaks. If it is too large, then samples can pass through thin occluders ...
Vector3 probeToPoint = wsPosition - probePos + (wsN + 3.0 * w_o) * irradianceFieldSurface.normalBias;
Vector3 dir = normalize(-probeToPoint);
```

```
d_biased = length(x - P + (n + 3·ω_o) · normalBias)      // normalBias 默认 0.25（IrradianceField.h:37）
dir       = normalize(-(x - P + (n + 3·ω_o)·normalBias)) // 用于查距离矩 atlas 的方向
```

* `n` = 着色点世界法线，`ω_o` = **指向相机的单位向量**（S3:96 `w_o = normalize(cameraPos - x)`）。
* 3.0 是经验系数：视线方向偏置是法线方向偏置的 3 倍，作用是沿"出射方向"把测试点拉离表面。
* **关键**：bias 只用于**可见性/距离查询**；backface 测试用的是**未加 bias** 的真实方向（S3:160），否则会破坏 wrap shading 的连续性。这是论文 Figure 7(d)(f) 的差异来源。

### 3.3 backface 软阈值：wrap shading，而不是 `max(0, dot)`

```glsl
// S3:155-172（注释保留原文，它解释了为什么不直接用 max(0,·)）
Vector3 trueDirectionToProbe = normalize(probePos - wsPosition);   // 注意：未加 bias

// 朴素做法： weight *= max(0.0001, dot(trueDirectionToProbe, wsN));   ← 被注释掉
// "The naive soft backface weight would ignore a probe when it is behind the surface. That's good for walls.
//  But for small details inside of a room, the normals on the details might rule out all of the probes
//  that have mutual visibility to the point. So, we instead use a 'wrap shading' test inspired by NPR work."

weight *= square(max(0.0001, (dot(trueDirectionToProbe, wsN) + 1.0) * 0.5)) + 0.2;
```

```
t_probe = normalize(P - x)                    // 真实方向，不含 bias
backfaceWeight = max(1e-4, (dot(t_probe, n) + 1) / 2 )² + 0.2
```

取值范围 `[0.2²+0.2, 1+0.2] ≈ [0.24, 1.2]`（`dot=-1` 时 `max(1e-4,0)=1e-4`，平方后 ≈0，故最小 ≈0.2）。

* 与论文一致：「backface-**cull** probes that lie below the shading point's tangent plane, using a **soft threshold that falls off smoothly** as the dot product … approaches zero」。
* 为什么不用 `max(0,·)`：房间内小物件的法线可能把**所有**互可见的探针都判成背面 → 全黑。wrap shading 保证每个探针至少保留 0.2 的权重。

### 3.4 Chebyshev 均值/方差可见性（含完整推导）

```glsl
// S3:174-196
vec2  texCoord = textureCoordFromDirection(-dir, p, depthW, depthH, depthProbeSideLength);
float distToProbe = length(probeToPoint);

float2 temp     = texture(irradianceFieldSurface.meanMeanSquaredProbeGridbuffer, texCoord, 0).rg; // 双线性
float  mean     = temp.x;                          // E[d]
float  variance = abs(square(temp.x) - temp.y);    // |E[d]² - E[d²]|（abs 处理 bias 导致的负值）

// http://www.punkuser.net/vsm/vsm_paper.pdf; equation 5
// Need the max in the denominator because biasing can cause a negative displacement
float chebyshevWeight = variance / (variance + square(max(distToProbe - mean, 0.0)));

// Increase contrast in the weight
chebyshevWeight = max(pow3(chebyshevWeight), 0.0);

weight *= (distToProbe <= mean) ? 1.0 : chebyshevWeight;
```

**推导**（这是"上界可见性"的由来，也是代码注释里 VSM 引用的含义）：

```
记 X = 探针沿方向 -dir 到最近表面的距离（随机变量），矩由 §2.2 的 cos^50 加权得到：
    μ  = E[X]  = Σ d_i w_i / Σ w_i
    E2 = E[X²] = Σ d_i² w_i / Σ w_i
    σ² = E[X²] - μ²        （代码写 abs()，因为 bias 可使 E2 略小于 μ²）

单边 Chebyshev 不等式（VSM paper eq.5）： 对 t > μ,
    P(X ≥ t) ≤ σ² / (σ² + (t - μ)²)

语义：X ≥ t ⇔ 距离 t 处**没有**遮挡物 ⇔ 着色点可见。
因此右侧就是"可见性"的一个**上界**：
    Vis(t) ≤ σ² / (σ² + (t-μ)²)

代码把它直接当 visibility 用（保守：宁可信其可见，避免过度变暗）：
    Vis = 1                          if t ≤ μ      （比平均表面还近，必然可见）
    Vis = σ² / (σ² + (t-μ)²)         if t > μ      （再取 ³ 次幂提高对比度）
```

* 第三方的做法可作对照：S8:157 `weight *= max(0.05f, chebyshevWeight);` —— 给可见性加 0.05 下限，避免"所有探针都判为不可见 → 全黑"；**官方 G3D 版没有这个下限**（只在后面统一 `max(1e-6, weight)`）。
* `pow3` 是经验锐化，**不是**更严格的数学界（严格界就是一次幂）。若发现间接阴影"太软"，先查这一项。
* 采样 r 通道（E[d]）用的是**双线性过滤后**的矩——这正是论文说"moment-based filtered shadow query"的含义：模糊发生在矩空间，不是 visibility 空间。

### 3.5 低辐照度抑制曲线（"crush"）

```glsl
// S3:198-217
weight = max(0.000001, weight);        // Avoid zero weight

// A tiny bit of light is really visible due to log perception, so crush tiny weights but keep the curve
// continuous. This must be done before the trilinear weights, because those should be preserved.
const float crushThreshold = 0.2;
if (weight < crushThreshold) {
    weight *= weight * weight * (1.0 / square(crushThreshold));
}
```

确切的曲线（`crushThreshold = 0.2`，`1/0.2² = 25`）：

```
w' = w                          , w ≥ 0.2
w' = w · w² / 0.04 = 25·w³      , w < 0.2
```

* 连续性：在 `w = 0.2` 处两段相等（0.2）。斜率不连续（左导数 3，右导数 1）→ 有意为之的"软拐点"。
* 相对衰减：`w'/w = (w/0.2)²`，即**相对权重的平方衰减**；`w=0.1` → `w'=0.025`（4× 衰减），`w=0.05` → `w'=0.0031`（16× 衰减）。
* 顺序敏感：必须在乘三线性权重**之前**做（注释明确说明），否则会把空间过渡也一起压掉。
* 与论文的对应：论文说「reduce the contribution of very low-irradiance values (i.e., **less than 5% of the representable intensity range**) according to a **monotonically-decreasing curve** profile」，但代码压的是**权重**（阈值 20%），**不是辐照度强度**（5%）。两者量纲不同，见 §6 差异表。

### 3.6 辐照度采样与"感知空间"混合

```glsl
// S3:201-209
vec3 irradianceDir = wsN;                       // 采样方向 = 表面法线本身（不是反射方向）
vec2 texCoord = textureCoordFromDirection(normalize(irradianceDir), p, irrW, irrH, irradianceProbeSideLength);
Irradiance3 probeIrradiance = texture(irradianceFieldSurface.irradianceProbeGridbuffer, texCoord).rgb;
```

```glsl
// S3:226-238
#if LINEAR_BLENDING == 0
    probeIrradiance = sqrt(probeIrradiance);    // 转到近似"感知亮度"空间再混合
#endif
    sumIrradiance += weight * probeIrradiance;
    sumWeight     += weight;
...
Irradiance3 netIrradiance = sumIrradiance / sumWeight;
#if LINEAR_BLENDING == 0
    netIrradiance = square(netIrradiance);      // 回到线性
#endif
netIrradiance *= energyPreservation;            // 0.85（多帧递归能量补偿）
E_lambertianIndirect = 0.5 * pi * netIrradiance;
```

* **sqrt 混合**是论文没写的实现细节：在 `value²`-ish 空间做加权平均，可以软化探针之间的亮度台阶（注释：「Weight in a more-perceptual brightness space instead of radiance space. This softens the transitions between probes with respect to translation」）。第三方用 `pow(irr, gamma*0.5)` / 再平方（S8:183-194），思路相同。**注意：sqrt 必须在乘权重之前逐个做，再在归一化之后平方回来**（先平均再变换是错的）。
* `sumWeight` 归一化：`Σ(w·È) / Σw`，权重含 backface × vis × crush × trilinear。

### 3.7 采样侧完整伪代码

```hlsl
float3 SampleIrradianceField(float3 x, float3 n, float3 wo) {
    int3  base   = BaseGridCoord(x);                        // clamp(floor((x-origin)/spacing), 0, counts-1)
    float3 alpha = saturate((x - GridToPos(base)) / spacing);
    float3 sumE = 0; float sumW = 0;

    for (int i = 0; i < 8; ++i) {
        int3 off = int3(i, i >> 1, i >> 2) & 1;
        int3 c   = clamp(base + off, 0, counts - 1);
        int   p  = c.x + c.y * counts.x + c.z * counts.x * counts.y;
        float3 P = GridToPos(c);

        float3 toP    = normalize(P - x);                                  // 无 bias（backface 用）
        float3 biased = x - P + (n + 3.0 * wo) * normalBias;               // 有 bias（visible 用）
        float3 dir    = normalize(-biased);
        float  t      = length(biased);
        float  w      = 1.0;

        w *= pow2(max(1e-4, (dot(toP, n) + 1.0) * 0.5)) + 0.2;             // backface wrap
        float2 m  = SampleBilinear(depthAtlas, p, OctEncode(dir));         // 双线性过滤的矩
        float mu  = m.x, e2 = m.y;
        float var = abs(mu * mu - e2);
        float cheb = var / (var + pow2(max(t - mu, 0.0)));
        cheb = max(pow3(cheb), 0.0);
        w *= (t <= mu) ? 1.0 : cheb;                                       // Chebyshev 可见性上界

        w = max(1e-6, w);
        if (w < 0.2) w *= w * w * 25.0;                                    // crush：w³/0.04
        float3 tri = lerp(1.0 - alpha, alpha, off);
        w *= tri.x * tri.y * tri.z;

        float3 E = SampleBilinear(irrAtlas, p, OctEncode(n));              // 沿法线取 È
        sumE += w * sqrt(max(E, 0));  sumW += w;                           // 感知空间混合
    }
    if (sumW == 0) return 0;
    float3 outE = pow2(sumE / sumW) * energyPreservation;                   // 0.85
    return outE * (0.5 * PI);                                              // ← 按 §2.4 自行标定
}
```

---

## 4. 距离矩 → 可见性：`maxDistance` 归一化与"远处几何被当遮挡"

### 4.1 距离矩的定义（不是 shadow map 深度）

```
E[d]  = Σ_i min(maxDistance, |P - hit_i|)     · w_i / Σ w_i ,   w_i = max(0, cosθ_i)^50
E[d²] = Σ_i min(maxDistance, |P - hit_i|)²    · w_i / Σ w_i
miss  ⇒ d_i = maxDistance（不是 inf、不是 0）
```

因此 `E[d] ∈ [0, maxDistance]`，`σ² = E[d²] - E[d]² ≥ 0`（代码 `abs` 兜底）。

### 4.2 为什么没有显式"归一化到 [0,1]"

* 参考实现**不做** `d/maxDistance` 归一化，也不需要：可见性测试是 `t` 与 `μ` 的**同量纲比较**，两端都来自世界空间距离，clamp 只影响"miss 出来的值"。
* `maxDistance` 的作用是**截断尾部**，防止 `E[d²]` 爆炸（RG16F/R16F 的数值范围）与"无穷远"污染矩。取 `1.5 × 网格单元对角线`（S6 `.cpp:166`）意味着：**超过一格半的距离差会被压平**，远处几何与 miss 在统计上趋于不可区分。
* 与论文的对应：论文只说了 `mean and squared distances to the nearest geometry`（§3），未给 `maxDistance` 的取法 —— **该常数来自参考实现，论文无对应文字**。

### 4.3 "远处几何被当成遮挡"到底怎么发生（以及如何避免）

设着色点到探针的真实距离 `t`，矩为 `(μ, σ²)`：

| 情形 | 判定 | 结果 | 风险 |
|---|---|---|---|
| `t ≤ μ` | 直接 1 | 可见 | 无 |
| `t > μ`，`σ²` 很小（该方向是平面墙） | `Vis ≈ σ²/(σ²+(t-μ)²) → 0` | **强遮挡** | 正确（墙在近处） |
| `t > μ`，`σ²` 很大（半遮挡/远处几何混入） | `Vis → 1` | 几乎全可见 | 保守，不会错误变暗 |
| `t > μ`，远处大平面 + 少量近处遮挡，`μ` 被拉大 | 可能 `t < μ` | 1（全可见） | **漏光（light leak）** |

也就是说：`maxDistance` 截断 + Chebyshev 上界使得**默认误差方向是"漏光"而不是"过度变暗"**。论文 Figure 7(c) 的经典漏光正是这套机制要压制的对象，靠的是 backface（§3.3）+ Chebyshev（§3.4）+ normal bias（§3.2）三者叠加。

反向的"远处几何被当成遮挡"（错误变暗）会出现，当且仅当：**某方向 `μ` 偏小但 `t` 偏大而 `σ²` 又小**，即"探针看到近处遮挡、但着色点实际在其后方很远"。典型场景：薄墙、单面几何、探针卡在几何内部。缓解手段（按参考实现给出的顺序）：

1. `normalBias = 0.25`（§3.2），把测试点拉出表面；
2. `hitBias = 0.01` + `rayMinDistance = 0.08`（§2.5/§2.7），让探针自己不要打到贴脸几何；
3. `energyConservation = 0.95` / `energyPreservation = 0.85`（§2.7/§3.6），压制递归累积；
4. 第三方额外用 `max(0.05, chebyshevWeight)`（S8:157）给可见性加下限——**官方版没有**，但如果你发现暗部死黑可以加；
5. 只用 `1.5 × cell diagonal` 的 `maxDistance`，不要设成"场景包围盒对角线"。

### 4.4 实现时必须自己决定的三个量（参考实现里是隐式的）

| 量 | 参考实现 | 建议 |
|---|---|---|
| `maxDistance` | `1.5 * length(bbox / probeCounts)`（`.cpp:166`） | 直接用同一公式；**不要**用固定 100 |
| miss 的 d | `maxDistance` | 同；若用 RGBA16F，`maxDistance=100` 时 `d²=1e4` 仍可存，但 `σ²` 量级失衡 |
| 可见性下限 | 无（只有 `max(1e-6, weight)`） | 可选 `max(0.05, cheb)`（S8 做法），代价是更多漏光 |

---

## 5. 常数总表

### 5.1 官方参考实现默认值（`IrradianceField.h` / `.cpp`）

| 参数 | 值 | 位置 |
|---|---|---|
| `irradianceOctResolution`（辐照度 tile 边长 S） | **8** | h:16 |
| `depthOctResolution`（距离 tile 边长 S） | **16** | h:17 |
| `irradianceDistanceBias` | 0.0 | h:21 |
| `irradianceVarianceBias` | 0.02 | h:25 |
| `irradianceChebyshevBias` | 0.07 | h:31 |
| `normalBias` | **0.25** | h:37 |
| `hysteresis` | **0.98**（论文 0.85~0.98） | h:44 |
| `depthSharpness`（cos^k 的 k） | **50.0** | h:49 |
| `irradianceRaysPerProbe` | **64** | h:52 |
| `m_maxDistance`（成员默认） | 4.0 | h:103 |
| `m_maxDistance`（运行时计算） | `1.5 * length(bbox/probeCounts)` | cpp:166 |
| `recursiveEnergyPreservation` | **0.85** | cpp:10 |
| `energyConservation`（更新侧） | **0.95** | pix:70 |
| `epsilon`（权重/更新阈值） | **1e-6**（论文写 1e-3） | pix:43 |
| `crushThreshold` | **0.2** | pix:214 |
| `rayMinDistance` | 0.08 | rays.pix:101 |
| `hitBias`（命中点外推） | 0.01 | pix:84 |
| 权重下限 | 1e-6 | pix:199 |
| 探针计数 | 2 的幂（索引用位运算） | h:275 / GridHelpers:159-166 |
| 探针间距范围（论文） | 1~2 m（人尺度场景） | 论文 §4.1 |

### 5.2 本工程当前值（只读观察，用于对照）

| 项 | 本工程值 | 位置 | 与参考实现 |
|---|---|---|---|
| 辐照度 tile / gutter / stride | 8 / 1 / **10** | `ProbeGrid.h:11-13` | 一致 |
| 距离 tile / stride | 16 / **18** | `ProbeGrid.h:15-16` | 一致 |
| 探针网格 | 8×4×8，spacing 0.63，origin (-2.2, 0.3, -2.2) | `ProbeGrid.h:19-24` | 一致（2 的幂） |
| hysteresis 实际值 | `alpha=0.05` ⇒ **0.95**；首帧 1.0 ⇒ 0 | `ShadersProbe.hlsl:53, 98` | 落在论文区间 ✓ |
| 更新权重 | **无 cos 加权**（每 texel 单射线） | `ShadersProbe.hlsl:37-54` | ✗ 结构不同，见 §7 |
| 距离权重 | 无 `cos^50`（同上原因） | `ShadersProbe.hlsl:81-103` | ≈ `k→∞` 的极限 |
| miss 距离 | **100**（固定） | `ShadersProbe.hlsl:86` | 参考实现是 `1.5×|cellDiag|`（spacing 0.63 时 ≈ **1.64**） |
| `normalBias` / Chebyshev / crush | 采样侧未实现 | — | 待实现 |

---

## 6. 论文正文 vs 官方代码：差异清单（评审时的权威依据）

| # | 论文正文（S1） | 官方代码（S2） | 结论 / 建议 |
|---|---|---|---|
| 1 | Eq(1)：`lerp(old, Σ(max(0,texelDir·rayDir)*rayRadiance), hysteresis)` | 输出 `alpha = 1-hysteresis`，`SRC_ALPHA/ONE_MINUS_SRC_ALPHA` ⇒ `(1-h)·new + h·old` | **论文 Eq(1) 的 lerp 参数方向与正文/代码相反**；以代码为准（正文「α-blending the new results at a rate of 1−α」是对的） |
| 2 | Eq(1) 是**裸和**，无归一化 | `result.xyz /= result.w`（Σw） | Eq(1) 省略了 `/Σw`；**必须归一化**，否则射线数改变会整体改变亮度 |
| 3 | 阈值 **0.001**（cosine-power lobe） | **1e-6**（两个分支共用） | 对辐照度分支等价；对 cos^50 分支 = 半角 40.7° vs 29.4°。建议分开命名两个阈值 |
| 4 | 「cosine-power lobe」 | `depthSharpness = 50` | **指数 50**（论文未给数字，这是唯一权威来源） |
| 5 | 「reduce the contribution of very low-irradiance values (< 5% of representable intensity range), monotonically-decreasing curve」 | `crushThreshold = 0.2`，`w ← w³/0.04` | 论文说的是**强度 5%**，代码压的是**权重 20%**；曲线形式代码给全：`w' = 25w³`（w<0.2） |
| 6 | 「mean- and variance-**biased** Chebyshev interpolants」 | S3 的 .pix **完全没有**使用 `irradianceChebyshevBias`(0.07) / `irradianceVarianceBias`(0.02)，虽然它们存在结构体（`GridHelpers.glsl:107-109`）与 C++（`.cpp:260-263`）里 | 说明 zip 里的 `.pix` 是**较早版本**；bias 的实际用法在 G3D10 的 `DDGIVolume.glsl`（**源码未获取到**，只有文件清单）。→ 本文的 Chebyshev 公式是"无 bias 版"，加了 bias 后应为 `var = |μ² - E2| + varianceBias`、`t -= chebyshevBias`（**此式为依据字段语义的推断，未经官方代码确认**） |
| 7 | Figure 3：「additional padding aligns probes on 4 × 4 write boundaries」 | `.pix` 中只有 1px gutter + 全局 2px 偏移，无额外 padding | 32bpp 格式（R11G11B10F / RG16F）不需要 4×4 对齐；若改用 R8/RG8 需自行补 padding |
| 8 | 「tetrahedral」 | 仅 §4.1 提到探针可布置在四面体**网格**上 | **不存在四面体插值**；插值就是八点三线性 |
| 9 | 论文 §3：距离矩 `RG16F` | `IrradianceField.h:119` 注释写 `RG32F`，`depthFormatIndex=1` | 格式可配；数值上 RG16F 足够（`maxDistance` 已截断） |
| 10 | 论文未提 | `energyConservation = 0.95`（每射线）、`recursiveEnergyPreservation = 0.85`（每次采样） | 两个**独立**的能量因子，都是经验值；移植时要当成可调参数 |

---

## 7. 与本工程当前实现的落点（只读观察，未改动任何源码）

> 依据：`RenderingProgrameByHand/ShadersProbe.hlsl`（155 行，含 11 个编译错误，见 `03-build-verify.md`）。

### 7.1 最大的结构差异：辐照度 texel 的语义不同

* **参考实现**：`texel[dir] = Σ_r L_r·max(0, dir·rayDir_r) / Σ_r max(0, dir·rayDir_r)`，即**余弦加权平均辐亮度**；采样时直接沿 `n` 取值，**不再乘 cos**。
* **本工程当前**：`ProbeUpdateCS` 每个 texel 只沿自身方向打**一条**射线，写入 `radiance`（未加权的方向辐亮度）；`8×8 = 64` 个 texel 恰好用满 64 条射线。

两者都能算 GI，但**混用会错 2 倍以上**：

| | 参考实现（gather） | 本工程（scatter，1 ray/texel） |
|---|---|---|
| texel 含义 | È = E/π（已含余弦卷积） | L(dir)（方向辐亮度） |
| 采样侧需要的额外操作 | 无 | **必须**对 64 个 texel 做球面加权求和 `E ≈ (4π/64)·Σ L_i·max(0,n·ω_i)`（等价 `π·È`）；若直接对 texel 取平均，得到的是**球面平均辐亮度**，与环境光分布相关（均匀环境下恰好相同，强侧光下差数倍），并不是只差一个常数 |
| 噪声特性 | n 条射线全部参与每个 texel，天然低通 | 单样本/texel，高噪声，靠 8×8 双线性 + 时间混合压制 |
| 距离矩等价于 | `cos^50` 加权 | `k→∞`（单方向 delta）——**这一侧反而与参考实现更接近** |

**建议（二选一，不要混）**：
1. **A 路线（照抄论文/参考实现）**：把 `ProbeUpdateCS` 改成"每探针 `n=64` 条 Fibonacci 射线 + 每 texel 遍历全部射线累加 cos 权重"，需要射线结果按 `[ray, probe]` 布局（参考实现就是 `texture2D(RAYS_PER_PROBE, probeCount)`），代价是每 texel 读 64 次，但可用 LDS 缓存一次 group 的全部射线。
2. **B 路线（保留现有 1 ray/texel）**：texel 存 L，采样侧必须做球面加权求和（或对 oct atlas 做一次 cosine 卷积的 mip/预滤波），并接受更高噪声。

### 7.2 可以直接抄的常数（按本文 §5.1）

`hysteresis=0.95`（已对）、`depthSharpness=50`、`normalBias=0.25`、`crushThreshold=0.2`、`weightEps=1e-6`（或论文 1e-3）、`hitBias=0.01`、`rayMinDistance=0.08`、`maxDistance=1.5×|cellDiag|`。

### 7.3 现有代码里需要优先修的点（与公式无关，但会挡住验证）

* `ShadersProbe.hlsl:78, 98` 使用 `g_Probe.gridOriginSpacing.xyz`，而 `ProbeUpdateParams`（`ProbeTypes.h`）里字段名是 `originX/originY/originZ/spacing` —— 编译错误已由 `03-build-verify.md` 记录；
* `ShadersProbe.hlsl:97, 103` 的 `distAtlas` 未声明；
* `ProbeDistanceCS` 用 `borderTileStride/borderTargetUav` 当自己的 stride/target，把 border-fixup 的参数复用到了距离矩 pass 上（语义混用）。

> 本节只做定位，不构成对工程源码的修改建议书；具体修法由 lead 决定。

---

## 附录：原始代码片段（每段 ≤ 40 行，均含文件与行号）

### A. `sampleIrradianceField.pix:116-220`（八探针 cage → 权重链，节选）

```glsl
    // alpha is how far from the floor(currentVertex) position. on [0, 1] for each axis.
    Vector3 alpha = clamp((wsPosition - baseProbePos) / irradianceFieldSurface.probeStep, Vector3(0), Vector3(1));
    for (int i = 0; i < 8; ++i) {
        GridCoord  offset = ivec3(i, i >> 1, i >> 2) & ivec3(1);
        GridCoord  probeGridCoord = clamp(baseGridCoord + offset, GridCoord(0), GridCoord(irradianceFieldSurface.probeCounts - 1));
        ProbeIndex p = gridCoordToProbeIndex(irradianceFieldSurface, probeGridCoord);
        Point3 probePos = gridCoordToPosition(irradianceFieldSurface, probeGridCoord);

        Vector3 probeToPoint = wsPosition - probePos + (wsN + 3.0 * w_o) * irradianceFieldSurface.normalBias;
        Vector3 dir = normalize(-probeToPoint);

        Vector3 trilinear = lerp(1.0 - alpha, alpha, offset);
        float weight = 1.0;

        {   // Smooth backface test
            Vector3 trueDirectionToProbe = normalize(probePos - wsPosition);
            weight *= square(max(0.0001, (dot(trueDirectionToProbe, wsN) + 1.0) * 0.5)) + 0.2;
        }
        {   // Moment visibility test
            vec2 texCoord = textureCoordFromDirection(-dir, p,
                irradianceFieldSurface.depthTextureWidth, irradianceFieldSurface.depthTextureHeight,
                irradianceFieldSurface.depthProbeSideLength);
            float distToProbe = length(probeToPoint);
            float2 temp = texture(irradianceFieldSurface.meanMeanSquaredProbeGridbuffer, texCoord, 0).rg;
            float mean = temp.x;
            float variance = abs(square(temp.x) - temp.y);
            float chebyshevWeight = variance / (variance + square(max(distToProbe - mean, 0.0)));
            chebyshevWeight = max(pow3(chebyshevWeight), 0.0);
            weight *= (distToProbe <= mean) ? 1.0 : chebyshevWeight;
        }
        weight = max(0.000001, weight);
        const float crushThreshold = 0.2;
        if (weight < crushThreshold) { weight *= weight * weight * (1.0 / square(crushThreshold)); }
        weight *= trilinear.x * trilinear.y * trilinear.z;
    }
```

### B. `IrradianceField_updateIrradianceProbe.pix:88-117`（余弦/余弦幂加权 + 归一化 + 混合系数）

```glsl
        // Detect misses and force depth
        if (dot(rayHitNormal, rayHitNormal) < epsilon) { rayProbeDistance = maxDistance; }

        vec3 texelDirection = octDecode(normalizedOctCoord(ivec2(gl_FragCoord.xy)));

#if OUTPUT_IRRADIANCE
        float weight = max(0.0, dot(texelDirection, rayDirection));
#else
        float weight = pow(max(0.0, dot(texelDirection, rayDirection)), depthSharpness);
#endif
        if (weight >= epsilon) {
            // Storing the sum of the weights in alpha temporarily
#               if OUTPUT_IRRADIANCE
            result += vec4(rayHitRadiance * weight, weight);
#               else
            result += vec4(rayProbeDistance * weight,
                square(rayProbeDistance) * weight,
                0.0,
                weight);
#               endif
        }
    }
    if (result.w > epsilon) {
        result.xyz /= result.w;
        result.w = 1.0f - hysteresis;
    } // if nonzero
```

### C. `IrradianceField_generateRandomRays.pix:42-55, 94-105`（Fibonacci 球面点 + 随机旋转）

```glsl
Vector3 sphericalFibonacci(float i, float n) {
    const float PHI = sqrt(5) * 0.5 + 0.5;
#   define madfrac(A, B) ((A)*(B)-floor((A)*(B)))
    float phi = 2.0 * pi * madfrac(i, PHI - 1);
    float cosTheta = 1.0 - (2.0 * i + 1.0) * (1.0 / n);
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    return Vector3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
#   undef madfrac
}

void main() {
    ivec2 pixelCoord = ivec2(gl_FragCoord.xy);
    int probeID = pixelCoord.y;
    int rayID   = pixelCoord.x;
    // This value should be on the order of the normal bias.
    const float rayMinDistance = 0.08;
    rayOrigin = float4(probeLocation(probeID), rayMinDistance);
    rayDirection = float4(randomOrientation * sphericalFibonacci(rayID, RAYS_PER_PROBE), inf);
}
```

### D. `sampleIrradianceField.pix:60-79`（oct → atlas uv，含 gutter 与全局 2px 偏移）

```glsl
vec2 textureCoordFromDirection(vec3 dir, int probeIndex, int fullTextureWidth, int fullTextureHeight, int probeSideLength) {
    vec2 normalizedOctCoord = octEncode(normalize(dir));
    vec2 normalizedOctCoordZeroOne = (normalizedOctCoord + vec2(1.0f)) * 0.5f;
    // Length of a probe side, plus one pixel on each edge for the border
    float probeWithBorderSide = (float)probeSideLength + 2.0f;
    vec2 octCoordNormalizedToTextureDimensions = (normalizedOctCoordZeroOne * (float)probeSideLength)
        / vec2((float)fullTextureWidth, (float)fullTextureHeight);
    int probesPerRow = (fullTextureWidth - 2) / (int)probeWithBorderSide;
    // Add (2,2) back to texCoord within larger texture. Compensates for 1 pix
    // border around texture and further 1 pix border around top left probe.
    vec2 probeTopLeftPosition = vec2(mod(probeIndex, probesPerRow) * probeWithBorderSide,
        (probeIndex / probesPerRow) * probeWithBorderSide) + vec2(2.0f, 2.0f);
    vec2 normalizedProbeTopLeftPosition = vec2(probeTopLeftPosition) / vec2((float)fullTextureWidth, (float)fullTextureHeight);
    return vec2(normalizedProbeTopLeftPosition + octCoordNormalizedToTextureDimensions);
}
```

### E. `IrradianceField.h:10-52`（参数默认值，权威常数表）

```cpp
    struct Specification {
        AABox           probeDimensions         = AABox(Point3(0.0f, 0.0f, 0.0f), Point3(1.0f, 1.0f, 1.0f));
        Vector3int32    probeCounts             = Vector3int32(4, 2, 4);
        int             irradianceOctResolution = 8;
        int             depthOctResolution      = 16;
        float           irradianceDistanceBias  = 0.0f;
        float           irradianceVarianceBias  = 0.02f;
        float           irradianceChebyshevBias = 0.07f;
        float           normalBias              = 0.25f;
        float           hysteresis              = 0.98f;
        float           depthSharpness          = 50.0f;
        int             irradianceRaysPerProbe  = 64;
        bool            glossyToMatte           = true;
        bool            singleBounce            = false;
        int             irradianceFormatIndex   = 4;
        int             depthFormatIndex        = 1;
        // ...
    };
```

### F. `RTXGI-DDGI/rtxgi-sdk/shaders/ddgi/Irradiance.hlsl:128-171`（第三方交叉验证，**非官方**）

```hlsl
        float wrapShading = (dot(worldPosToAdjProbe, direction) + 1.f) * 0.5f;
        weight *= (wrapShading * wrapShading) + 0.2f;

        float2 octantCoords = DDGIGetOctahedralCoordinates(-biasedPosToAdjProbe);
        float3 probeTextureUV = DDGIGetProbeUV(adjacentProbeIndex, octantCoords, volume.probeNumDistanceInteriorTexels, volume);
        float2 filteredDistance = 2.f * resources.probeDistance.SampleLevel(resources.bilinearSampler, probeTextureUV, 0).rg;

        float variance = abs((filteredDistance.x * filteredDistance.x) - filteredDistance.y);

        float chebyshevWeight = 1.f;
        if(biasedPosToAdjProbeDist > filteredDistance.x) {    // occluded
            float v = biasedPosToAdjProbeDist - filteredDistance.x;
            chebyshevWeight = variance / (variance + (v * v));
            chebyshevWeight = max((chebyshevWeight * chebyshevWeight * chebyshevWeight), 0.f);
        }
        weight *= max(0.05f, chebyshevWeight);
        weight = max(0.000001f, weight);

        const float crushThreshold = 0.2f;
        if (weight < crushThreshold) { weight *= (weight * weight) * (1.f / (crushThreshold * crushThreshold)); }
        weight *= trilinearWeight;

        float3 probeIrradiance = resources.probeIrradiance.SampleLevel(resources.bilinearSampler, probeTextureUV, 0).rgb;
        float3 exponent = volume.probeIrradianceEncodingGamma * 0.5f;
        probeIrradiance = pow(probeIrradiance, exponent);
        irradiance += (weight * probeIrradiance);
        accumulatedWeights += weight;
    }
    irradiance *= (1.f / accumulatedWeights);
    irradiance *= irradiance;
    irradiance *= RTXGI_2PI;
```

---

## 附：未获取到的条目（明确列出）

| 条目 | 状态 | 说明 |
|---|---|---|
| `octahedral.glsl`（octEncode/octDecode 原文） | **未获取到** | 补充材料 zip 不含该文件；§1.4 给的是标准实现 + 第三方对照 |
| `IrradianceField_copyProbeEdges.pix`（gutter 生成） | **未获取到** | `.cpp:533` 引用了它，但 zip 未包含；本工程 `BorderFixupCS` 已在做同样的事 |
| `irradianceChebyshevBias` / `irradianceVarianceBias` 的**实际用法** | **未获取到** | 参数存在（值 0.07 / 0.02）但 `.pix` 未使用；G3D10 `DDGIVolume.glsl` 源码无公开浏览器 |
| G3D10 `DDGIVolume_*` 系列源码 | **未获取到** | 只有 Doxygen 文件清单（S9）；如需可再花时间找 G3D 源码包 |
| 论文 Eq(4)/(5) 的编号式（正文里权重链是 bullet list，无编号公式） | 已确认不存在 | 论文只在 §5.2 用 5 条 bullet 描述权重链，**没有编号公式**；精确公式只能在 shader 里找 |
| 四面体插值 | 已确认不存在 | 见 §3.1、§6 #8 |
