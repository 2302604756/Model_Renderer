# DDGI 完整讲解（唯一主文档）

> **本文件是 DDGI 的唯一主文档，持续更新。**
> 后续任何 DDGI 相关的原理讲解、公式、渲染流程、代码注解、常数、踩坑记录，**一律追加到这里，不再新建文件**。
>
> **配套材料（项目侧，只作证据与实施用）**
> - `00-implementation-plan.md` —— 本项目（D3D12 引擎）的改造方案与分阶段路线
> - `01-probe-core-audit.md` —— 本项目探针核心代码审计
> - `02-integration-audit.md` —— 本项目帧内集成审计
> - `03-build-verify.md` —— 本项目真实编译结果
> - `04-reference-formulas.md` —— 官方公式的逐行取证（含 SHA-256）
>
> **本项目现状**：见 **第十部分**（§29–§33）。每次复核代码后更新，
> 含进度总表、逐文件三方对照、`SampleDDGI` 逐行评审、探针更新缺口、剩余 TODO。
>
> **公式渲染**：正文公式使用 LaTeX 数学语法（`$…$` 行内、`$$…$$` 独立成段），
> 在 GitHub、Typora、VS Code（KaTeX / Markdown Preview Enhanced）中均可正常渲染。
> 代码块仍是代码块，不做转换。纯文本编辑器会看到公式源码。
>
> **源码**：`%TEMP%\ddgi_supp\code\`（JCGT 补充材料 `supplement.zip` 解压）
> **论文**：Majercik, Guertin, Nowrouzezahrai, McGuire. *Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields*. JCGT 8(2), 2019.

---

## 目录

**第一部分 · 原理**
1. DDGI 解决什么问题
2. 数学基础：从辐照度到 `E/π`

**第二部分 · 数据结构**
3. 探针网格
4. Octahedral 映射与图集布局
5. 两张 atlas

**第三部分 · 更新**
6. 射线生成
7. 求交与 surfel 着色
8. 累加、归一化与混合

**第四部分 · 采样**
9. 八探针 cage 与三线性
10. 权重链五步
11. 能量约定：为什么三种实现差 4 倍

**第五部分 · 多弹射**
12. 无限弹射是怎么来的

**第六部分 · 官方实现的渲染流程**
13. 四步管线
14. 资源清单与读写真值矩阵
15. 每个 pass 写了什么
16. atlas 的三个版本
17. 生产者 → 消费者
18. 内存账
19. 两个实现技巧

**第七部分 · 读懂代码**
20. G3D 惯用法词典
21. 逐行注释版：两个关键 draw

**第八部分 · 常数与差异**
22. 官方常数总表
23. 论文 vs 代码：10 处差异
24. 四个常见误解（都不用做）

**第九部分 · 落地**
25. 移植到 D3D12 的逐项对照
26. 常见陷阱清单
27. 验证方法
28. 分阶段路线（摘要）

**第十部分 · 本项目现状对照**（每次复核代码后更新）
29. 进度总表
30. 你的代码 ↔ 本文章节 ↔ 官方 三方对照
31. `SampleDDGI` 逐行评审
32. 探针更新：当前语义与官方差在哪
33. 剩余 TODO（按收益 / 成本排序）

**附录**
A. 证据边界
B. 更新记录

---

# 第一部分 · 原理

## 1. DDGI 解决什么问题

### 1.1 经典 irradiance probe 的三个失效模式

经典的（球谐 / cubemap）辐照度探针只存了**入射光的方向分布**，没有存**可见性**。于是：

| 失效模式 | 现象 | 原因 |
|---|---|---|
| **漏光（light leak）** | 墙另一侧的亮光渗到暗房间 | 探针只知道"那个方向有光"，不知道"中间有墙" |
| **漏阴影（shadow leak）** | 墙角、遮挡物附近不够暗 | 同上 |
| **插值断裂** | 探针之间出现块状台阶 | 粗糙的三线性插值，缺更平滑的度量 |

这三个问题的共同根源：**辐照度场是不完整的** —— 它缺了遮挡。

### 1.2 DDGI 的两个关键补充

1. **每个探针额外存一张"距离矩"图**（16×16 octahedral，`E[d]` 和 `E[d²]`）。
   它把"沿这个方向最近的表面有多远"编码成一个双参数分布，从而能对任意查询距离 `t` 给出一个**可见性的上界**（Chebyshev 不等式）。
2. **采样时按可见性加权**（backface wrap + Chebyshev + normal bias + 低辐照度抑制 + 三线性 + 感知空间混合）。

于是：**同一次查询里，离得远的探针会被判为"看不见你"从而降权**，漏光被压制。
论文的核心创新就是这个 —— 摘要原话是 "a novel **visibility-aware moment-based interpolant**"。

### 1.3 它在 GI 方案里的位置

| 方案 | 动态几何 | 动态光照 | 成本 | 主要伪影 |
|---|---|---|---|---|
| Lightmap | ❌ | ❌ | 低 | 漏光、接缝 |
| 经典 irradiance probe | ✅（需重算） | ✅（需重算） | 中 | **漏光** |
| Screen-space GI | ✅ | ✅ | 低 | 屏幕外信息缺失 |
| 路径追踪 | ✅ | ✅ | **极高** | 噪声 |
| **DDGI** | ✅ | ✅ | **中低** | 收敛延迟（1+ 帧） |

DDGI 的关键取舍：**把"精确"换成"平滑且不漏光"**。间接光本来就是低频的，这个取舍很划算。

---

## 2. 数学基础：从辐照度到 `E/π`

这一节是整个算法"数值对不对"的根基。**看完这节可以跳过后面所有的公式推导。**

### 2.1 辐照度与 Lambert

入射辐照度（irradiance）：

$$
E(\mathbf n) \;=\; \int_{\mathcal H^2(\mathbf n)} L(\omega)\,\max\!\bigl(0,\ \mathbf n\cdot\omega\bigr)\;\mathrm{d}\omega
\qquad [\mathrm{W/m^2}]
$$

Lambert 漫反射的**出射辐亮度**：

$$
L_o \;=\; \frac{\rho}{\pi}\,E(\mathbf n)
$$

这个 $1/\pi$ 来自 BRDF：$f_r = \rho/\pi$。

### 2.2 探针里到底存了什么：$\hat E = E/\pi$

DDGI 的每个 texel（对应一个方向 $\mathbf d$）在更新时做的是：

$$
\hat E(\mathbf d) \;=\; \frac{\sum_i L_i\,w_i}{\sum_i w_i},
\qquad w_i \;=\; \max\!\bigl(0,\ \mathbf d\cdot\omega_i\bigr)
$$

即"**余弦加权平均辐亮度**"。这个量不是 $E$，也不是 $L$，而是：

$$
\hat E \;=\; \frac{E}{\pi}
$$

**推导**（$N$ 条射线在球面均匀分布，蒙特卡洛估计 $\int f\,\mathrm d\omega \approx \frac{4\pi}{N}\sum_i f(\omega_i)$）：

$$
S \;=\; \sum_i w_i L_i \;\approx\; \frac{4\pi}{N}\int L\,\max(0,\mathbf d\cdot\omega)\,\mathrm d\omega \;=\; \frac{4\pi}{N}\,E
$$

$$
W \;=\; \sum_i w_i \;\approx\; \frac{4\pi}{N}\int \max(0,\mathbf d\cdot\omega)\,\mathrm d\omega
\;=\; \frac{4\pi}{N}\cdot\pi \;=\; \frac{N}{4}
$$

$$
\Longrightarrow\quad \frac{S}{W} \;=\; \frac{E}{\pi}
$$

代入 Lambert：

$$
L_o \;=\; \frac{\rho}{\pi}E \;=\; \rho\,\frac{E}{\pi} \;=\; \rho\,\hat E
$$

**结论：$\mathrm{albedo}\times$ 探针值 直接就是出射辐亮度。** 不需要再乘或除 $\pi$。

这就是为什么采样侧最后一行是 `E_lambertianIndirect = 0.5*pi*netIrradiance` 这种奇怪的形式 —— 那是 G3D 自己直射光强度约定下的补偿，**不是通用公式**（详见 §11）。

### 2.3 三个必须记牢的"归一化"事实

| 事实 | 错误写法 | 后果 |
|---|---|---|
| 归一化**除以 $\sum w$**，不是除以 $N$ | $S/N$ | 暗 4 倍（$E/4\pi$ 而非 $E/\pi$） |
| 距离通道有**自己的 $\sum w$** | 复用颜色通道的 $\sum w$ | 距离矩完全错乱 |
| 采样时 8 个探针要**再归一化一次** | 只加权不除 $\sum w$ | 墙根/墙角一圈发黑 |

### 2.4 为什么颜色用 $\cos$、距离用 $\cos^{50}$

| 通道 | 权重 | 理由 |
|---|---|---|
| 辐照度 | $\max(0,\cos\theta)$ | Lambert 余弦卷积**本身就是辐照度的定义**，只需在更新时做一次；采样时直接沿法线取值 |
| 距离矩 | $\max(0,\cos\theta)^{50}$ | 距离场不是可积的辐射度量。宽余弦会让一个 texel 混合整个半球的深度 → 均值被远处几何拉大 → 可见性判断失效 |

由 $\varepsilon \le \cos^k\theta$ 反解 lobe 半角：

$$
\theta_{\max}(\varepsilon,k) \;=\; \arccos\!\left(\varepsilon^{1/k}\right)
$$

| $k$ | $\varepsilon$ | $\theta_{\max}$ | 说明 |
|---|---|---|---|
| 1 | $10^{-6}$ | 90.0° | 辐照度，几乎整个前半球 |
| 50 | $10^{-6}$（代码） | **40.7°** | 距离，默认实际孔径 |
| 50 | $0.001$（论文） | **29.4°** | 论文正文阈值对应孔径 |
| 100 | $10^{-6}$ | 29.4° | 等价于"$k=50$ + 论文阈值" |

> 论文正文只说 "cosine-power lobe" + "threshold 0.001"，**没给 $k$ 的值**。
> `depthSharpness = 50` 只存在于代码里，是唯一权威来源。

---

# 第二部分 · 数据结构

## 3. 探针网格

### 3.1 三个参数

| 参数 | 含义 | 官方默认 |
|---|---|---|
| `probeDimensions`（AABox） | 网格覆盖的包围盒 | 场景包围盒 × 0.9（或 ×1.1 "enclose"） |
| `probeCounts`（int3） | 三轴探针数，**必须是 2 的幂** | (4, 2, 4) |
| `probeStep`（float3） | 相邻探针间距 | `(hi − lo) / (counts − 1)` |

**注意 `probeStep` 除以 `N − 1`**（不是 N）：

$$
\mathbf s \;=\; \frac{\mathbf{hi}-\mathbf{lo}}{\max\bigl(\mathbf N-\mathbf 1,\ \mathbf 1\bigr)}
\qquad(\text{逐轴独立})
$$

```cpp
m_probeStep = (hi - lo) / (Vector3(probeCounts) - Vector3(1,1,1)).max(Vector3(1,1,1));
```

这样首尾探针正好落在包围盒边界上。而 `maxDistance` 用的是除以 $N$（见 §3.2）。

特例：某轴 `counts == 1` 时，该轴起点取 `(hi+lo)/2`（单层探针要放在中间）。

**为什么要求 2 的幂**：`probeIndex → gridCoord` 可以用位运算代替除法（`index & (cx−1)`、`index >> findMSB(cx)`），官方注释说省 ~10ms。

### 3.2 `maxDistance`：距离矩的截断尺度

$$
d_{\max} \;=\; 1.5\,\left\|\frac{\mathbf{hi}-\mathbf{lo}}{\mathbf N}\right\|
$$

```cpp
const Vector3 boundingBoxLengths(probeDimensions.high() - probeDimensions.low());
m_maxDistance = (boundingBoxLengths / spec.probeCounts).length() * 1.5f;
```

语义：**1.5 倍单元对角线**。

对本项目的网格（spacing $0.63$，$8\times4\times8$）：

$$
\frac{\mathbf{hi}-\mathbf{lo}}{\mathbf N}=\frac{(4.41,\ 1.89,\ 4.41)}{(8,4,8)}=(0.551,\ 0.473,\ 0.551),
\qquad \|\cdot\| = 0.906
$$

$$
d_{\max} \;=\; 1.5\times 0.906 \;=\; 1.37
$$

**为什么必须这么小（而不是随手写 100）**：

1. $\mathbb E[d]\le d_{\max}$、$\mathbb E[d^{2}]\le d_{\max}^{2}$ ⇒ $d^{2}\le 2$，half 精度充足。
   用 $100$ 的话 $d^{2}=10^{4}$，而 $\sigma^{2}=\left|\mu^{2}-\mathbb E[d^{2}]\right|$ 是两个接近大数相减 —— 精度损失会把方差算成 0（可见性恒 1，等于没遮挡）或算爆。
2. 作用就是**截断尾部**：距离差超过一格半就被压平，远处几何与 miss 在统计上不可区分。这正是"避免远处几何被当成遮挡"的手段。

**miss 写 $d_{\max}$**，不是 inf、不是 0。

> 实现技巧：既然超过 `maxDistance` 一律当 miss，直接把这个值当 `RayDesc.TMax` 即可 —— 等价且不用额外 clamp。

### 3.3 探针落在几何体内部怎么办

论文原话：落在墙里的探针"**会被 visibility-query metrics 忽略**"。

意思是：**加了 Chebyshev 之后这类探针会自愈**，不需要额外的"探针分类 / relocation pass"。

> DDGI 论文原版**不做 probe relocation**（那是 Unity HDRP 等后续变体）。
> 如果你对标的是论文，这一项不算差距；如果对标 Unity 版，才需要单独立项。

---

## 4. Octahedral 映射与图集布局

### 4.1 编解码

球面 → 单位正方形的八面体映射（Cigolle et al. 2014）：

```hlsl
float2 OctEncode(float3 n) {
    float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    float2 uv = n.xy / l1;
    if (n.z < 0.0) uv = (1.0 - abs(uv.yx)) * sign(uv + 1e-6);
    return uv;                       // ∈[-1,1]²（有的实现直接返回 [0,1]）
}

float3 OctDecode(float2 uv) {
    float3 d = float3(uv, 1.0 - abs(uv.x) - abs(uv.y));
    if (d.z < 0.0) d.xy = (1.0 - abs(d.yx)) * sign(d.xy + 1e-6);
    return normalize(d);
}
```

**为什么不用 cubemap**：论文 §2 说八面体"略有更小的畸变，且边界管理更简单"。
不需要 6 个面、不需要 seam fixup、图集打包更简单。

### 4.2 两种尺寸

| 内容 | 分辨率 | 原因 |
|---|---|---|
| 辐照度 | **8×8** | 辐照度已经很平滑，低分辨率足够 |
| 距离矩 | **16×16** | 距离场有锐利不连续（墙角、边缘），需要更高分辨率 |

论文 §6.1 的消融结论：**探针数量比探针分辨率更重要**。低角分辨率探针也能收敛得很好，但探针太稀会漏光。

### 4.3 gutter 与双线性

每个探针 tile 外面加 **1px gutter**，内容 = 内部边缘 texel 的复制（clamp-to-edge）。

```
tile (S=8, stride=10):
  +---------------------------+
  | g | 8 个内部 texel      | g |
  +---------------------------+
    ^                         ^
   gutter                   gutter
```

**gutter 的唯一职责**：让 tile **内部**的双线性在边界处不外溢。

于是采样时的 tile 内像素坐标是（$\mathrm{oct}_{01}\in[0,1]$ 为八面体编码）：

$$
p_{\text{tile}} \;=\; 1 + \mathrm{oct}_{01}\cdot S
\qquad\Longrightarrow\qquad
p_{\text{tile}}\in[\,1,\;S+1\,]
$$

**为什么是 $1+\mathrm{oct}_{01}S$ 而不是 $1.5+\mathrm{oct}_{01}S$**：

内部 texel 索引 $1\dots S$ 的中心在像素坐标 $1.5 \dots S+0.5$。

- $\mathrm{oct}_{01}=0$ → 像素 $1.0$（gutter texel 0 与 texel 1 的边界）；因 texel 0 是 texel 1 的复制，
  双线性结果**恰好等于 texel 1**
- $\mathrm{oct}_{01}=1$ → 像素 $S+1.0$，同理等于 texel $S$
- $\mathrm{oct}_{01}\in\left[\tfrac{0.5}{S},\ \tfrac{S-0.5}{S}\right]$ 时落在真正的 texel 中心上
  （$S=8$ 即 $[0.0625,\ 0.9375]$）—— 这就是标准的 half-texel inset

> ⚠ **不要**去做"八面体接缝重建"。每个探针是独立 tile，采样 UV **永不跨 tile**；
> tile 内部折痕处的轻微不连续是八面体参数化的固有近似，官方也不处理。

### 4.4 图集布局：官方 vs 本项目

官方（`IrradianceField.cpp:569-573`）：

```cpp
width  = (S + 2) * probeCounts.x * probeCounts.y + 2;
height = (S + 2) * probeCounts.z + 2;
```

⇒ **宽里塞下所有 (x,y)，高里每行一个 z 层**；`tile 列号 = x + y·cx`，`tile 行号 = z`。

本项目（`ProbeGrid.h`）：

```
width  = cx * (S + 2);
height = cy * cz * (S + 2);
```

⇒ **列 = x，行 = y + z·cy**。

**两者互为转置，都自洽。** 差别有三点：
1. 官方在整张图集外面多 1px 全局边框（`+2`），本项目没有。
2. 官方 `probesPerRow = (W − 2) / (S + 2) = cx·cy`；本项目 = `cx`。
3. 官方图集"扁而宽"，本项目"窄而高"。

**改布局要同步改 4 处**：更新写入、边界修补、debug 可视化、采样读取。

---

## 5. 两张 atlas

### 5.1 内容

| atlas | 通道 | 内容 |
|---|---|---|
| 辐照度 | `rgb` | $\hat E = E/\pi$ |
| | `a` | 通常不用（本项目写 1） |
| 距离矩 | `r` | $\mathbb E[d]$ |
| | `g` | $\mathbb E[d^2]$ |
| | `b`,`a` | 未用 |

> 论文用 `RG16F` 存距离矩，用 `R11G11B10F` 存辐照度。
> 官方代码默认 **`RGB16F`（索引 4）**，不是论文写的 `R11G11B10F`（索引 3）—— 见 §23 差异 #9。
> 论文 Figure 12 的结论：11 位浮点在存储与质量之间最平衡；8 位整型会明显偏暗、闪烁。

### 5.2 格式选择表

| 格式 | 每像素 | 评价 |
|---|---|---|
| `RGB5A1` | 2 B | 需要很低的 hysteresis（0.8）才不闪烁，且过饱和 |
| `RGB8` | 4 B | 动态范围不足，偏暗 |
| `RGB10A2` | 4 B | 质量/体积平衡，但无浮点动态范围 |
| `R11G11B10F` | 4 B | **论文推荐**；有个绿色偏色（无法表示精确灰） |
| `RGB16F` | 8 B | 官方代码默认 |
| `RGB32F` | 12 B | 过剩 |

### 5.3 跨帧累积

两张 atlas 都是**常驻资源，内容跨帧保留**。累积系数就是 hysteresis（§8.4）。

⇒ **绝对不能用每帧 reset 的瞬态堆分配它们。**

> 本项目用的是 `RenderTexture::InitUAV` → `CreateCommittedResource`，是常驻的 ✅。
> 但要小心首帧未初始化（§26 陷阱 5）。

---

# 第三部分 · 更新

## 6. 射线生成

### 6.1 Fibonacci 螺旋

```glsl
Vector3 sphericalFibonacci(float i, float n) {
    const float PHI = sqrt(5) * 0.5 + 0.5;          // 黄金比
    float phi      = 2.0 * pi * madfrac(i, PHI - 1); // madfrac(a,b) = frac(a*b)
    float cosTheta = 1.0 - (2.0*i + 1.0) * (1.0/n);  // 从 z=+1 螺旋到 z=-1
    float sinTheta = sqrt(saturate(1.0 - cosTheta*cosTheta));
    return Vector3(cos(phi)*sinTheta, sin(phi)*sinTheta, cosTheta);
}
```

球面近均匀分布，**无表、无随机数、可复现**。
`i < n/2` 时只覆盖 +z 半球 —— 这个性质有时能省一半射线。

### 6.2 每帧一次**全局**随机旋转

```cpp
args.setUniform("randomOrientation",
    Matrix3::fromAxisAngle(Vector3::random(), Random::common().uniform(0.f, 2*pif())));
```

- 随机轴 + `[0, 2π)` 随机角，**整个探针场共用同一个旋转**（不是每探针一个）。
- 目的：让每帧的样本集合不同，这样 hysteresis 的指数平均才真的在**近似球面积分**，而不是原地空转。
- ⚠ 自研时**必须显式归一化随机轴**，否则 `fromAxisAngle` 得到的矩阵带缩放，方向不再是单位向量。

> **这是最高优先级的一条**：如果每帧射线一样，`avg_n = L·(1 − (1−α)^n) → L`，
> 时间累积退化成纯粹的算力浪费，banding 一条都不会消。

### 6.3 `tMin` / `tMax` 藏在 w 分量里

```glsl
const float rayMinDistance = 0.08;                      // 量级与 normalBias 相当
rayOrigin    = float4(probeLocation(probeID), rayMinDistance);                             // w = tMin
rayDirection = float4(randomOrientation * sphericalFibonacci(rayID, RAYS_PER_PROBE), inf);  // w = tMax
```

G3D 的 `intersectRays` 接口用 `.w` 传 tMin/tMax。你自己实现时给 `RayDesc.TMin/TMax` 即可。

`tMin = 0.08` 的作用：让射线不要"贴着探针自己"就开始求交，避免打到与探针共面的几何。

---

## 7. 求交与 surfel 着色

### 7.1 三个"关掉"

| 关掉什么 | 代码 | 论文依据 |
|---|---|---|
| 背面剔除 | `TriTree::DO_NOT_CULL_BACKFACES` | 单面几何（只有内表面的天花板）会被探针看穿 |
| AO | `e.ambientOcclusionSettings.enabled = false` | §5.2：算 surfel 着色时不加 AO，影响"显著小于"对直接可见表面加 AO |
| glossy 间接缓冲 | `USE_GLOSSY_INDIRECT_BUFFER=false` | 探针射线没有二次镜面反射 |
| （额外）glossy→matte | `GLOSSY_TO_MATTE=true` | 把 glossy 系数并进 matte，消除低概率的时序不稳定焦散 |

### 7.2 miss 怎么处理

```
命中 ⇒ 法线非 0 ⇒ 正常着色
miss ⇒ 法线置 0 ⇒ sampleIrradianceField 提前 return 0 ⇒ 延迟着色走 skybox 分支
```

于是**天空光是通过"miss → 读 skybox → 写进 rayShadedFB"这条路径进入探针的**，
不是靠一个单独的 ambient 项。

### 7.3 surfel 着色 = 像素着色

论文 §4.3 的核心设计：**同一份着色代码同时服务探针 surfel 和最终像素**。

所以 `sampleIrradianceField.pix` 里：

```glsl
w_o = normalize(texelFetch(gbuffer_WS_RAY_ORIGIN_buffer, C, 0).xyz - wsPosition);
```

- 相机像素：`rayOrigin` 是相机位置
- 探针 surfel：`rayOrigin` 是探针位置

**换个输入就把 "相机看到的点" 变成 "探针看到的点"** —— 这是整个实现最优雅的一处复用。

---

## 8. 累加、归一化与混合

### 8.1 gather 结构：一个 texel 遍历全部射线

```glsl
for (int r = 0; r < RAYS_PER_PROBE; ++r) {
    ivec2 C = ivec2(r, relativeProbeID);              // x=射线号, y=探针号

    float weight = max(0.0, dot(texelDirection, rayDirection));            // 辐照度：cos
    // 距离矩分支：pow(max(0.0, dot(...)), depthSharpness);                // = 50

    if (weight >= epsilon) {                          // epsilon = 1e-6
        result += vec4(rayHitRadiance * weight, weight);                   // ΣL·w , Σw
        // 距离矩：result += vec4(d*w, d²*w, 0, w)
    }
}
if (result.w > epsilon) {
    result.xyz /= result.w;          // ← 除以 Σw，不是除以 N
    result.w = 1.0f - hysteresis;    // ← alpha = 新值权重
}
// result.w <= epsilon ⇒ alpha 保持 0 ⇒ 这一帧这块不写 ⇒ 保留旧值
```

**注意这是 gather（每个 texel 收集所有射线），不是 scatter（每线程打一条自己的射线）。**
两者存的东西完全不同：

| 结构 | texel 含义 | 采样侧还需要做什么 |
|---|---|---|
| **gather（官方）** | $\hat E=E/\pi$，已含余弦卷积 | **什么都不用**，直接沿法线取值 |
| scatter（本项目当前） | $L(\mathbf d)$，方向辐亮度 | **必须**再做球面加权求和 $E\approx\dfrac{4\pi}{N}\sum_i L_i\max(0,\ \mathbf n\cdot\omega_i)$ |

⚠ **两个归一化不能混用**。如果只是把 64 个 texel 平均一下当辐照度，
得到的**不是** $E/\pi$，而是球面平均辐亮度 —— 在强侧光下差数倍，且**误差不是常数**，无法用一个系数校正。

### 8.2 距离矩的独立权重和

`OUTPUT_IRRADIANCE` 宏让同一个 shader 产出两个变体，但**两个变体各跑一次 dispatch**，
所以每个变体只需要自己的 `result.w`。

如果自研时想在一次 dispatch 里写两张 atlas，**必须记两个独立的 Σw**。

### 8.3 `hitBias` 与 miss

```glsl
rayHitLocation += rayHitNormal * 0.01;                                    // hitBias
float rayProbeDistance = min(maxDistance, length(probeLocation - rayHitLocation));
if (dot(rayHitNormal, rayHitNormal) < epsilon) rayProbeDistance = maxDistance;   // miss
```

`0.01` 的沿法线外推是"贴面噪声"的对策：DDGI 的距离是探针到命中面的精确距离，
唯一风险是射线打到与自己共面的几何，1cm 外推能推出去。

### 8.4 hysteresis 的方向：论文 Eq(1) 写反了

官方：

```cpp
args.setUniform("hysteresis", m_firstFrame ? 0.0f : m_specification.hysteresis);   // 首帧 0
rd->setBlendFunc(BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA);
```
```glsl
result.w = 1.0f - hysteresis;      // 输出的 alpha
```

因输出 alpha 为 $a = 1-h$，ROP 混合给出：

$$
\hat E_{\text{new}}
\;=\; a\,\hat E_{\text{this frame}} + (1-a)\,\hat E_{\text{old}}
\;=\; (1-h)\,\hat E_{\text{this frame}} + h\,\hat E_{\text{old}}
$$

$h=0.98$ 时新值只占 $2\%$；$h=0.85$ 时占 $15\%$。

- 论文正文："alpha-blending the new results **at a rate of $1-\alpha$**" —— 与代码一致 ✅
- 论文 **Eq(1)**：$\mathrm{lerp}(\text{old},\ \Sigma\ldots,\ \text{hysteresis})$ —— 按 $\mathrm{lerp}(a,b,t)=(1-t)a+tb$ 的语义，新值权重应为 $h$，**与正文和代码都相反** ❌

**以代码/正文为准。**

**首帧 $h=0$** ⇒ $a=1$ ⇒ 直接覆盖。这是躲开"未初始化显存做 lerp 产生 NaN"的关键（§26 陷阱 5）。

> D3D12 里 **UAV 不能和 blend state 共存**，所以 compute 版本必须手动 lerp。
> 而手动 lerp 就要小心 NaN 传播：
> $\mathrm{lerp}(\mathrm{NaN},\,x,\,1)=\mathrm{NaN}+1\cdot(x-\mathrm{NaN})=\mathrm{NaN}$。

### 8.5 能量因子

| 因子 | 值 | 位置 | 作用 |
|---|---|---|---|
| `energyConservation` | **0.95** | 更新侧，每条射线辐亮度 ×0.95 | 抑制递归能量累积 |
| `recursiveEnergyPreservation` | **0.85** | 采样侧，每次采样 ×0.85 | 同上（多帧递归） |

两个都是经验值，移植时当可调参数。**它们是"暗室不会越照越亮"的保证。**

---

# 第四部分 · 采样

## 9. 八探针 cage 与三线性

```glsl
ivec3  base  = clamp(floor((X - probeStartPosition) / probeStep), 0, probeCounts - 1);
Point3 baseP = probeStep * Vector3(base) + probeStartPosition;
Vector3 alpha = clamp((X - baseP) / probeStep, 0, 1);

for (int i = 0; i < 8; ++i) {
    ivec3 off = ivec3(i, i >> 1, i >> 2) & ivec3(1);      // 三轴各取 bit0/1/2
    ivec3 c   = clamp(base + off, 0, probeCounts - 1);     // ← 二次 clamp
    int   p   = c.x + c.y*countX + c.z*countX*countY;

    Vector3 tri = lerp(1.0 - alpha, alpha, off);           // off=0 → 1-α，off=1 → α
    weight *= tri.x * tri.y * tri.z;
}
```

两个要点：

1. **边界处 8 个探针会重复**（二次 clamp 造成），同一探针被多次加权。
   官方**不做去重**，靠 `weight = max(1e-6, …)` 和最后归一化吸收。
2. **就是三线性，没有四面体插值**。论文 §4.1 提到的 "tetrahedral" 是指探针**布置**可以是四面体网格，不是插值方式。

---

## 10. 权重链五步

按官方代码的真实顺序（顺序**不能调换**）：

### ① 两种方向：backface 用真实方向，可见性用 biased 方向

```glsl
Vector3 toP    = normalize(probePos - X);                          // 无 bias
Vector3 biased = (X - probePos) + (wsN + 3.0*w_o) * normalBias;    // normalBias = 0.25
Vector3 dirVis = normalize(-biased);                               // 查距离矩的方向
float   t      = length(biased);                                   // 可见性用的距离
```

- `w_o` = 指向相机的单位向量
- `3.0` 是经验系数：视线方向偏置是法线方向的 3 倍，作用是把测试点沿出射方向拉离表面
- **bias 只用于可见性/距离查询**，backface 用真实方向（否则破坏 wrap shading 的连续性）

`normalBias` 太小的后果：明暗漏光；太大的后果：采样能穿过薄遮挡物。
官方默认 **0.25**。

### ② backface wrap shading

$$
w_{\text{bf}}
\;=\;
\Bigl(\max\bigl(10^{-4},\ \tfrac{1}{2}(\mathbf n\cdot\mathbf t_{\text{probe}}+1)\bigr)\Bigr)^{2}
\;+\;0.2
\qquad\in[\,0.2,\ 1.2\,]
$$

其中 $\mathbf t_{\text{probe}}=\mathrm{normalize}(\mathbf P_{\text{probe}}-\mathbf X)$ 是**未加 bias** 的真实方向。

```glsl
weight *= square(max(1e-4, (dot(toP, wsN) + 1.0) * 0.5)) + 0.2;
```

**为什么不用 $\max(0,\ \mathbf n\cdot\mathbf t)$**（代码注释原文）：
"The naive soft backface weight would ignore a probe when it is behind the surface. That's good for walls.
But for small details inside of a room, the normals on the details might rule out **all** of the probes that have mutual visibility to the point."

⇒ 房间里有小物件时，物件的法线可能把所有互可见探针都判成背面 → 全黑。
wrap shading 保证每个探针至少保留 $0.2$。

### ③ Chebyshev 矩可见性

$$
w_{\text{cheb}}(t)\;=\;
\begin{cases}
1, & t\le\mu\\[4pt]
\left(\dfrac{\sigma^{2}}{\sigma^{2}+(t-\mu)^{2}}\right)^{\!3}, & t>\mu
\end{cases}
$$

```glsl
float2 m    = SampleBilinear(depthAtlas, p, OctEncode(dirVis));
float  mu   = m.x, e2 = m.y;
float  var  = abs(mu*mu - e2);                                  // abs：bias 可使 E² < μ²
float  cheb = var / (var + square(max(t - mu, 0.0)));
cheb        = max(pow3(cheb), 0.0);                             // 经验锐化
weight     *= (t <= mu) ? 1.0 : cheb;
```

**推导**：记 $X$ 为探针沿 $\mathbf{dir}_{\text{vis}}$ 到最近表面的距离，其前两阶矩为

$$
\mu=\mathbb E[X],\qquad \sigma^{2}=\mathbb E[X^{2}]-\mu^{2}
$$

单边 Chebyshev 不等式（VSM paper eq.5），对 $t>\mu$：

$$
\Pr\bigl(X\ge t\bigr)\;\le\;\frac{\sigma^{2}}{\sigma^{2}+(t-\mu)^{2}}
$$

语义上 $X\ge t$ ⇔ 距离 $t$ 处没有遮挡 ⇔ **着色点可见**，所以右侧是**可见性的上界**。

直接用它是**保守**的（宁可信其可见，避免过度变暗）；误差默认方向是"漏光"而不是"过暗"。

`pow3` 是经验锐化，**不是**更严格的数学界。间接阴影"太软"时先查这一项。

> 采样的 `r` 通道用的是**双线性过滤后**的矩 —— 这就是论文说 "moment-based **filtered** shadow query" 的含义：
> 模糊发生在**矩空间**，不是 visibility 空间。

### ④ 低辐照度 crush

```glsl
weight = max(1e-6, weight);
const float crushThreshold = 0.2;
if (weight < crushThreshold) weight *= weight * weight * (1.0 / square(crushThreshold));
```

确切曲线（$1/0.2^{2}=25$）：

$$
w' \;=\;
\begin{cases}
w, & w\ge 0.2\\[4pt]
25\,w^{3}, & w<0.2
\end{cases}
$$

相对衰减 $\dfrac{w'}{w}=\left(\dfrac{w}{0.2}\right)^{2}$：
$w=0.1\Rightarrow w'=0.025$（4×），$w=0.05\Rightarrow w'=0.0031$（16×）。

**必须在三线性之前**（代码注释："This must be done before the trilinear weights, because those should be preserved"），
否则会把空间过渡一起压掉。

> 与论文的差异：论文说"降低**强度**低于 5% 的辐照度"，代码压的是**权重**（阈值 20%）。
> 两者量纲不同，见 §23 差异 #5。

### ⑤ 三线性

```glsl
weight *= trilinear.x * trilinear.y * trilinear.z;
```

### ⑥ 取辐照度 + 感知空间混合

```glsl
Irradiance3 probeIrradiance = texture(irrAtlas, textureCoordFromDirection(wsN, p, ...)).rgb;
// 沿【法线】取值，不是反射方向 —— 因为 texel 已含余弦卷积

#if LINEAR_BLENDING == 0
    probeIrradiance = sqrt(probeIrradiance);        // 转近似感知空间
#endif
sumIrradiance += weight * probeIrradiance;
sumWeight     += weight;

netIrradiance = sumIrradiance / sumWeight;          // ← 归一化，必须有
#if LINEAR_BLENDING == 0
    netIrradiance = square(netIrradiance);          // 回到线性
#endif
netIrradiance *= energyPreservation;                // 0.85
E_lambertianIndirect = 0.5 * pi * netIrradiance;    // ← G3D 管线约定，见 §11
```

**sqrt 混合**是论文没写的实现细节：在 `value²` 附近空间做加权平均，
可以软化探针之间的亮度台阶（"This softens the transitions between probes with respect to translation"）。

⚠ **必须"先逐项变换、再平均、再变回来"**。先平均再变换是错的。

### 权重链一览

| 步 | 项 | 表达式 | 作用 |
|---|---|---|---|
| ① | normal bias | $\mathbf X' = \mathbf X + (\mathbf n + 3\,\omega_o)\,b_{\text{normal}}$，$b_{\text{normal}}=0.25$ | 把可见性测试点拉离表面 |
| ② | backface wrap | $\bigl(\tfrac12(\mathbf n\cdot\mathbf t+1)\bigr)^{2}+0.2\ \in[\,0.2,\ 1.2\,]$ | 防止小物件全黑 |
| ③ | Chebyshev | $\left(\dfrac{\sigma^{2}}{\sigma^{2}+(t-\mu)^{2}}\right)^{3}$；$t\le\mu$ 时取 $1$ | 核心创新：可见性上界 |
| ④ | crush | $w<0.2\ \Rightarrow\ 25\,w^{3}$ | 压掉暗处微量漏光 |
| ⑤ | 三线性 | $\prod_{a\in\{x,y,z\}}\mathrm{lerp}(1-\alpha_a,\ \alpha_a,\ \mathrm{off}_a)$ | 摊平探针间台阶 |
| ⑥ | sqrt 混合 | $\Bigl(\dfrac{\sum_i w_i\sqrt{\hat E_i}}{\sum_i w_i}\Bigr)^{2}\times 0.85$ | 感知空间平滑 + 归一化 |

---

## 11. 能量约定：为什么三种实现差 4 倍

最后那个常数是**管线相关的**，不能照抄：

| 实现 | 末行 | 等效乘子 |
|---|---|---|
| G3D 官方 | `E_lambertianIndirect = 0.5 * pi * netIrradiance` | $\approx E/2$ |
| 物理正确（§2.2 约定） | 不乘，直接 $\rho\hat E$ | $E/\pi$ |
| RTXGI（第三方） | `irradiance *= RTXGI_2PI` | $E\cdot 2\pi$ |

三者相差 4 倍，属于**各自渲染管线的能量约定**（G3D 的直射强度约定 + 递归 bounce 衰减；RTXGI 的 gamma 编码）。

**正确做法：用全白环境标定（§27）。**

---

# 第五部分 · 多弹射

## 12. 无限弹射是怎么来的

论文 §5.3：多次弹射靠"**用上一帧的辐照度场去点亮这一次的 surfel**"。

```
帧 k：
  ① 生成射线
  ② 求交 → surfel
  ③ 着色 surfel：直射 + 【采样 atlas 的当前内容 = 上一帧结果】 → radiance
  ④ 用 radiance 更新 atlas → V(k)
  ⑤ 主图延迟着色：直射 + 采样 atlas 的【新内容 V(k)】
```

- surfel 用的是 `V(k−1)`，atlas 更新成 `V(k)`
- 相机像素用的是 `V(k)`

于是每一次弹射都比上一帧"退一位"，逐帧递归 ⇒ 无限弹射收敛。
代价是**间接光有 1 帧延迟**（论文 §5.3 明确承认这个 time-lag，并认为在动态场景里不可察觉）。

### 12.1 为什么一张 atlas 就够（不需要 ping-pong）

**读严格发生在写之前**，两者是**不同的 draw call**，GPU 按提交顺序执行 ⇒ 无读写冲突。

```
③b 读 atlas  →  ④ 写 atlas
```

**真正的约束不是"要两张图"，而是"不能把采样探针和写探针塞进同一个 dispatch"。**

两种合法做法：

| 方案 | 做法 | 评价 |
|---|---|---|
| A. 拆成两个 pass | trace+shade 一个 pass，写 atlas 另一个 pass | 官方做法，最省资源 |
| B. ping-pong 两张 | 读 A 写 B，每帧交换 | 更保险，但没必要（`+200 KB` 而已） |

⚠ 如果像本项目当前那样把 "trace + shade + write" 耦合在一个 dispatch 里，
做多弹射时**必须拆**，否则同一个 dispatch 里读写同一张 UAV 是 UB。

---

# 第六部分 · 官方实现的渲染流程

> 这一部分讲的是 **G3D 参考实现**（`supplement.zip`）。它和论文讲的是同一件事，
> 但落地形态是「**每帧四次全屏 draw 的 deferred 管线**」，不是 compute。

## 13. 四步管线

`IrradianceField::onGraphics3D`（`IrradianceField.cpp:309-319`）：

```cpp
void IrradianceField::onGraphics3D(rd, surfaceArray) {
    if (m_sceneDirty && System::time() - lastSceneUpdateTime() > 0.1) {
        m_sceneTriTree->setContents(m_scene);      // 场景脏 → 重建求交树，≤10Hz
        m_sceneDirty = false;
    }

    generateIrradianceProbes(rd);                  // ① 分配资源 + 首帧写边界遮罩
    generateIrradianceRays(rd, m_scene);           // ② 生成 64×N 条射线
    sampleAndShadeIrradianceRays(rd, ...);         // ③ 求交 → GI → 着色
    updateIrradianceProbes(rd, m_scene);           // ④ 写进两张 atlas
}
```

```
                      ② generateIrradianceRays
                    +--------------------------+
                    | m_irradianceRayOrigins   |  64 × N  RGBA32F  xyz=位置, w=tMin=0.08
                    | m_irradianceRayDirections|  64 × N  RGBA32F  xyz=方向, w=tMax=inf
                    +------------+-------------+
                                 |  x = rayID (0..63),  y = probeID (0..N-1)
                                 v
   ③ sampleAndShadeIrradianceRays
     a) intersectRays(...)              -> 射线 GBuffer（命中点/法线，miss 时法线=0）
     b) renderIndirectIllumination(...)  -> 采样【上一帧 atlas】-> m_giFramebuffer
     c) GIRenderer_deferredShade.pix     -> 直射+间接 -> m_irradianceRaysShadedFB  (64×N)
                                 |
                                 v
   ④ updateIrradianceProbes
      updateIrradianceProbe(IRRADIANCE)  -> m_irradianceProbes
      updateIrradianceProbe(DEPTH)       -> m_meanDistProbes
      每张 atlas 两次 draw：
        draw A  IrradianceField_updateIrradianceProbe.pix  （只写内部 texel）
        draw B  IrradianceField_copyProbeEdges.pix         （只写 gutter）
```

### 13.1 ① `generateIrradianceProbes`（`.cpp:539-617`）

名义上每帧调用，实际只在首次或分辨率/格式变化时才分配。

```cpp
irradianceWidth  = (S + 2) * cx * cy + 2;
irradianceHeight = (S + 2) * cz + 2;
```

两张 atlas 各配一个 `DEPTH32` 附件 —— **不是用来做深度测试的**，是当 1-bit 遮罩（§19.1）。

首帧还会 clear + 调 `IrradianceField_writeOnesToProbeBorders.pix` 打遮罩标记。

### 13.2 ② `generateIrradianceRays`（`.cpp:379-395`）

```cpp
rd->push2D(m_irradianceRaysFB); {
    args.setMacro("RAYS_PER_PROBE", 64);
    args.setUniform("randomOrientation", Matrix3::fromAxisAngle(Vector3::random(), uniform(0, 2π)));
    LAUNCH_SHADER("IrradianceField_generateRandomRays.pix", args);
} rd->pop2D();
```

shader（`generateRandomRays.pix:94-105`）：

```glsl
int probeID = int(gl_FragCoord.y);   // 纹理高 = probeCount
int rayID   = int(gl_FragCoord.x);   // 纹理宽 = 64
rayOrigin    = float4(probeLocation(probeID), 0.08);
rayDirection = float4(randomOrientation * sphericalFibonacci(rayID, RAYS_PER_PROBE), inf);
```

### 13.3 ③ `sampleAndShadeIrradianceRays`（`.cpp:398-473`）

**a. 求交**

```cpp
m_sceneTriTree->intersectRays(rayOrigins, rayDirections, gbuffer, TriTree::DO_NOT_CULL_BACKFACES);
```

**b. 算间接光（多弹射就在这里）**

```cpp
renderIndirectIllumination(rd, gbuffer, environment);   // .cpp:352-376
```

```cpp
rd->push2D(m_giFramebuffer); {
    rd->setDepthTest(RenderDevice::DEPTH_GREATER);      // 跳过天空像素
    gbuffer->setShaderArgsRead(args, "gbuffer_");
    setShaderArgs(args, "irradianceFieldSurface.");     // ← 两张 atlas + 网格参数
    m_irradianceRayOrigins ->setShaderArgs(args, "gbuffer_WS_RAY_ORIGIN_",  Sampler::buffer());
    m_irradianceRayDirections->setShaderArgs(args, "gbuffer_WS_RAY_DIRECTION_", Sampler::buffer());
    args.setUniform("energyPreservation", 0.85);
    LAUNCH_SHADER("GIRenderer_computeIndirect.pix", args);
} rd->pop2D();
```

**c. 延迟着色成 radiance**

```cpp
rd->push2D(m_irradianceRaysShadedFB); {
    e.ambientOcclusionSettings.enabled = false;                      // 关 AO
    args.setMacro("GLOSSY_TO_MATTE", glossyToMatte);                 // true
    args.setUniform("matteIndirectBuffer", useProbeIndirect ? m_giFramebuffer->texture(0)
                                                            : Texture::opaqueBlack(), Sampler::buffer());
    args.setMacro("LIGHTING_MODE", LightingMode::DIRECT_INDIRECT);
    args.setMacro("OVERRIDE_SKYBOX", true);
    args.setMacro("USE_GLOSSY_INDIRECT_BUFFER", false);
    LAUNCH_SHADER("GIRenderer/GIRenderer_deferredShade.pix", args);
} rd->pop2D();
```

`useProbeIndirect = !m_oneBounce`：`singleBounce = true` 时这里传黑色 ⇒ 探针只捕获一次弹射。

### 13.4 ④ `updateIrradianceProbes`（`.cpp:476-536`）

```cpp
updateIrradianceProbe(rd, IRRADIANCE);
updateIrradianceProbe(rd, DEPTH);
m_firstFrame = false;       // ← 只在这里清掉
```

`updateIrradianceProbe(rd, irradiance)` 对每张 atlas 做**两次全屏 draw**：

**draw A —— 更新内部 texel**

```cpp
rd->push2D(irradiance ? m_irradianceProbeFB : m_meanDistProbeFB); {
    rd->setBlendFunc(BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA);   // hysteresis 交给 ROP
    rd->setDepthTest(RenderDevice::DepthTest::DEPTH_GREATER);       // 只写内部
    args.setUniform("hysteresis", m_firstFrame ? 0.0f : 0.98f);
    args.setUniform("depthSharpness", 50.0f);
    args.setUniform("maxDistance", m_maxDistance);
    args.setMacro("OUTPUT_IRRADIANCE", irradiance);                 // 一个文件两个变体
    // 四张逐射线缓冲，全部 Sampler::buffer()（最近邻）
    m_irradianceRaysGBuffer->texture(WS_POSITION)->setShaderArgs(args, "rayHitLocations.", Sampler::buffer());
    m_irradianceRaysGBuffer->texture(WS_NORMAL)  ->setShaderArgs(args, "rayHitNormals.",   Sampler::buffer());
    m_irradianceRayDirections                    ->setShaderArgs(args, "rayDirections.",    Sampler::buffer());
    m_irradianceRaysShadedFB->texture(0)         ->setShaderArgs(args, "rayHitRadiance.",   Sampler::buffer());
    skyboxKeyframe->setShaderArgs(args, "skybox_", Sampler::defaults());   // miss 时读天空
    LAUNCH_SHADER("IrradianceField_updateIrradianceProbe.pix", args);
} rd->pop2D();
```

**draw B —— 填 gutter**

```cpp
rd->push2D(same FB); {
    rd->setBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
    rd->setDepthTest(RenderDevice::DepthTest::DEPTH_LEQUAL);        // 只写 gutter
    args.setUniform("probeTexture", irradiance ? m_irradianceProbes : m_meanDistProbes, Sampler::buffer());
    LAUNCH_SHADER("IrradianceField_copyProbeEdges.pix", args);
} rd->pop2D();
```

---

## 14. 资源清单与读写真值矩阵

### 14.1 资源清单

设 `R = 64`、`N = cx·cy·cz`、`S_irr = 8`、`S_d = 16`：

| # | 资源 | 类型 / 格式 | 尺寸 | 生命周期 |
|---|---|---|---|---|
| 1 | `m_sceneTriTree` | CPU 求交树 | — | 场景脏时重建，≤10Hz |
| 2 | `m_irradianceRayOrigins` | Texture RGBA32F | `R × N` | 常驻 |
| 3 | `m_irradianceRayDirections` | Texture RGBA32F | `R × N` | 常驻 |
| 4 | `m_irradianceRaysFB` | Framebuffer{#2,#3} | `R × N` | 常驻 |
| 5 | `m_irradianceRaysGBuffer` | GBuffer（6 字段 RGBA32F） | `N × R`（转置 ⚠） | 常驻 |
| 6 | `m_irradianceRaysShadedFB` | FB{1 × RGB32F} | `R × N` | 常驻 |
| 7 | `m_giFramebuffer` | FB{1 × RGBA32F} | `R × N` | 常驻 |
| 8 | `m_irradianceProbes` | Texture **RGB16F** | `(S_irr+2)·cx·cy+2` × `(S_irr+2)·cz+2` | **跨帧累积** |
| 9 | `m_meanDistProbes` | Texture **RG16F** | `(S_d+2)·cx·cy+2` × `(S_d+2)·cz+2` | **跨帧累积** |
| 10 | `irradianceStencil` | DEPTH32 | 同 #8 | 1-bit 遮罩 |
| 11 | `depthStencil` | DEPTH32 | 同 #9 | 1-bit 遮罩 |
| 12 | 4 张 debug 纹理 | RGBA32F | 512² / 8192² | 纯 debug |

> 官方 `s_irradianceFormats = {RGB5A1, RGB8, RGB10A2, R11G11B10F, RGB16F, RGB32F}`，
> `irradianceFormatIndex` 默认 **4** ⇒ **RGB16F**（不是论文写的 R11G11B10F）。
> 深度 `s_depthFormats = {RGB8, RG16F, RG32F}`，默认索引 1 ⇒ RG16F ✅ 与论文一致。

### 14.2 每帧读写矩阵

`R`=读 `W`=写 `R/W`=读改写。**列顺序即帧内顺序。**

| 资源 | ② rays | ③a trace | ③b GI | ③c shade | ④A update | ④B edges |
|---|---|---|---|---|---|---|
| 1 `sceneTriTree` | | R | | R | | |
| 2 `rayOrigins` | **W** | R | R (`w_o`) | R | R | |
| 3 `rayDirections` | **W** | R | R | R | R | |
| 5 `GB.WS_POSITION` | | **W** | R | R | R | |
| 5 `GB.WS_NORMAL` | | **W** | R | R | R | |
| 5 `GB.LAMBERTIAN/GLOSSY/EMISSIVE/TRANSMISSIVE` | | **W**\* | R (GLOSSY) | R | | |
| 6 `rayShadedFB` | | | | **W** | R | |
| 7 `giFramebuffer` | | | **W** | R | | |
| 8 `irradianceProbes` | 创建+清 | | **R（上一帧）** | | **R/W** | **R/W** |
| 9 `meanDistProbes` | 创建+清 | | **R（上一帧）** | | **R/W** | **R/W** |
| 10/11 `*Stencil` | W | | | | R (mask) | R (mask) |
| skybox | | | | R | | |
| `LightingEnvironment` | | | R | R | | |

\* 材质字段由 `TriTree::intersectRays` 填充（其内部实现未随补充材料发布）。

**整张表最重要的一格**：③b 对 atlas 的 `R` 出现在 ④A 的 `R/W` **之前** ⇒ 一张 atlas 就够。

---

## 15. 每个 pass 写了什么

| pass | 目标 | 写出的确切内容 |
|---|---|---|
| ② rays | #2 `rayOrigins` | `xyz` = `probeStartPosition + probeStep·gridCoord(probeID)`；`w` = **0.08**（tMin） |
| ② rays | #3 `rayDirections` | `xyz` = `randomOrientation · sphericalFibonacci(rayID, 64)`；`w` = **inf**（tMax） |
| ③a trace | #5 GBuffer | 命中点世界位置、世界法线、材质/反照率；**miss ⇒ 法线 = 0**；无深度附件 |
| ③b GI | #7 `giFramebuffer` | `rgb` ≈ `E_lambertianIndirect`（八探针 cage 加权 ×0.85）；miss 像素写 0 |
| ③c shade | #6 `rayShadedFB` | `rgb` = 该射线命中面的**出射 radiance**（直射+间接；miss ⇒ **天空色**） |
| ④A 辐照度 | #8 内部 texel | `rgb` = `Σ(L·cos)/Σcos` = **È = E/π**；`a` = **`1 − hysteresis`** |
| ④A 距离矩 | #9 内部 texel | `rg` = `E[d]`、`E[d²]`（`cos^50` 加权）；`a` = `1 − hysteresis` |
| ④B edges | #8/#9 的 gutter | 内部边缘 texel 复制进 gutter（算法未发布） |
| ① alloc | #10/#11 | 两类深度值，把 atlas 像素分成「内部」/「gutter」 |

两个容易漏的点：

- **天空光是走"miss → 读 skybox → 写进 `rayShadedFB`"这条路进入探针的**，不是单独的 ambient 项。
- **`a` 通道不是不透明度，是混合系数。** 配合 `BLEND_SRC_ALPHA / ONE_MINUS_SRC_ALPHA` 得到 `new·(1−h) + old·h`。
  `Σw ≤ 1e-6` 时 `a` 保持 0 ⇒ 这一帧整块不写 ⇒ 保留旧值。

---

## 16. atlas 的三个版本

理解全流程最省力的心智模型：

```
                     atlas 内容
帧 k 开始            V(k−1)
③b 读               V(k−1)      <- 探针 surfel 用【上一帧】的场做多弹射
④A/④B 写             -> V(k)     <- 本帧 64 条射线混进去
主图 pass 读         V(k)        <- 相机像素用【本帧刚更新完】的场
帧 k 结束            V(k)
```

所以：**相机看到的间接光是最新的；探针 surfel 用的是上一帧的**（§12）。

---

## 17. 生产者 → 消费者

```
① Stencil ---------------+--> ④A depth test（只写内部）
                         +--> ④B depth test（只写 gutter）

② origins/dirs --+--> ③a 求交
                 +--> ③b 算 w_o
                 +--> ③c 视线方向
                 +--> ④A probeLocation

③a GBuffer ------+--> ③b 法线/位置
                 +--> ③c 着色
                 +--> ④A 算 rayProbeDistance

③b giFB ---------------> ③c matteIndirectBuffer
③c rayShadedFB --------> ④A rayHitRadiance
④A/④B atlas -----+--> ③b（下一帧多弹射）
                 +--> 主图 GI pass（本帧相机像素）
```

---

## 18. 内存账

| 资源 | 官方默认 `(4,2,4)`，N=32 | 本项目 `(8,4,8)`，N=256 |
|---|---|---|
| 射线纹理 ×2 | 各 32 KB | 各 256 KB |
| 射线 GBuffer（6 字段） | 192 KB | **1.5 MB** |
| rayShadedFB | 24 KB | 192 KB |
| giFramebuffer | 32 KB | 256 KB |
| 辐照度 atlas | 82×42 RGB16F ≈ 20 KB | 322×82 RGB16F ≈ 155 KB |
| 距离矩 atlas | 146×74 RG16F ≈ 42 KB | 578×146 RG16F ≈ 330 KB |

> **中间缓冲比 atlas 本身大一个量级**（1.5 MB vs 155 KB）。
> 因为它们正比于 `R × N`，而 atlas 只正比于 `N × tile面积`。
> 做性能/显存优化时先看那几个 `R × N` 的缓冲。

---

## 19. 两个实现技巧

### 19.1 用 depth buffer 当 1-bit stencil

同一张 atlas 要用两次 draw 分别处理「内部 texel」和「gutter texel」。官方做法：

1. 首帧把「内部」和「gutter + 全局边框」写成两种深度值；
2. 更新 pass 用 `DEPTH_GREATER` → 只有一种深度值能通过；
3. 补边 pass 用 `DEPTH_LEQUAL` → 只有另一种能通过。

**一次全屏 draw = 一次结构化遮罩写入，零分支开销。**

> 在 D3D12 compute 里完全不需要这招：直接 `if (local.x == 0 || local.x == S+1) return;`。
> （`IrradianceField_writeOnesToProbeBorders.pix` 未随材料发布，所以 0/1 的具体分配未确认。）

### 19.2 用 ROP 混合实现 hysteresis

这是官方代码和"compute 直觉"最大的差异：

| | 官方（G3D） | 自研 compute |
|---|---|---|
| 旧值从哪来 | ROP 的 framebuffer 读-改-写 | shader 里 `RWTexture2D[...]` 读出来 |
| 混合 | `BLEND_SRC_ALPHA / ONE_MINUS_SRC_ALPHA` | 手动 `lerp(old, new, 1−h)` |
| 首帧 | `hysteresis = 0` → alpha = 1 → 全覆盖 | `frameIndex == 0` 时直接赋值 |

⚠ compute shader 里 **UAV 不能和 blend state 共存**，所以这套必须改成手动 lerp。

---

# 第七部分 · 读懂代码

## 20. G3D 惯用法词典

### 20.1 `Args` + `LAUNCH_SHADER` + `push2D`

```cpp
rd->push2D(m_irradianceRaysFB); {          // ① 绑定渲染目标（存/换/恢复状态）
    Args args;                             // ② 准备参数包
    args.setMacro("RAYS_PER_PROBE", 64);   // ③ 编译期 #define
    args.setRect(rd->viewport());          // ④ 画多大
    args.setUniform("randomOrientation", ...);  // ⑤ 运行期 uniform
    LAUNCH_SHADER("x.pix", args);          // ⑥ 编译+绑定+画全屏四边形
} rd->pop2D();                             // ⑦ 恢复
```

| 符号 | 作用 | D3D12 等价 |
|---|---|---|
| `Args` | 「名字→值」字典，只用于启动 shader 前批量赋值 | 待绑定的根参数清单 |
| `args.setUniform(n, v)` | **运行期** uniform | 写常量缓冲 / 绑 CBV |
| `args.setMacro(n, v)` | **编译期** `#define` → 不同值 = 不同编译产物 | 多份 entry point / 多份 PSO |
| `args.setRect(rect)` | 画出的四边形覆盖多大；`rd->viewport()` = 目标全尺寸 | viewport + scissor |
| `LAUNCH_SHADER(f, args)` | 编译该 shader（按宏缓存）→ 灌 uniform → 绑定 → **画全屏四边形** | `SetPSO` + `SetGraphicsRoot*` + `DrawInstanced(3,1,0,0)` |
| `rd->push2D(fb)` / `pop2D()` | 状态栈 + 换渲染目标 + 切 2D 正交 | `OMSetRenderTargets` + 手动保存/恢复 |

**核心认知**：`LAUNCH_SHADER` **永远是画一个铺满 `setRect` 的四边形**。
所以"目标纹理的每个像素" = "这个 PS 跑一次"。这就是为什么一个没有 compute 的引擎也能做 DDGI ——
**整张 atlas 的每个 texel 都变成一次 fragment 调用。**

⚠ `push2D` **不会**自动清屏，要清得显式 `rd->clear()`。

### 20.2 `setShaderArgs(args, prefix)` 前缀约定

C++ 侧（`IrradianceField.cpp:241-268`）：

```cpp
alwaysAssertM(endsWith(prefix, "."), "Requires a struct prefix");   // ← 必须带点
args.setUniform(prefix + "probeStep", m_probeStep);
args.setUniform(prefix + "normalBias", spec.normalBias);
m_irradianceProbes->setShaderArgs(args, prefix + "irradianceProbeGrid", bilinear);
```

GLSL 侧（`GridHelpers.glsl:92-111`，**已发布**）：

```glsl
struct IrradianceField {
    Vector3int32 probeCounts;
    Point3       probeStartPosition;
    Vector3      probeStep;
    int          lowResolutionDownsampleFactor;
    sampler2D    irradianceProbeGridbuffer;
    sampler2D    meanMeanSquaredProbeGridbuffer;
    int          irradianceTextureWidth;
    ...
    float        normalBias;
};
```

shader 里再声明：

```glsl
uniform IrradianceField irradianceFieldSurface;
```

于是 `prefix + "probeStep"` → `irradianceFieldSurface.probeStep`。

| C++ 设置的名字（prefix 展开后） | GLSL 实际访问 | 备注 |
|---|---|---|
| `irradianceFieldSurface.probeCounts` | 同名 | 直接对应 |
| `irradianceFieldSurface.probeStep` | 同名 | |
| `irradianceFieldSurface.irradianceProbeGrid` | `...irradianceProbeGridbuffer` | **`Texture::setShaderArgs` 自动加 `buffer` 后缀** |
| `irradianceFieldSurface.normalBias` | 同名 | |

**一句话**：`setShaderArgs` = 「把 C++ 对象成员批量映射成 GLSL 某个 struct 的成员」。带点号的前缀 = struct 实例名。

### 20.3 两种纹理声明

| GLSL 声明 | C++ 写法 | 用途 | D3D12 |
|---|---|---|---|
| `uniform_Texture(sampler2D, foo_)` | `tex->setShaderArgs(args, "foo_", Sampler::buffer())` | 纯采样器 | SRV + 静态采样器 |
| `uniform Texture2D bar;` | `tex->setShaderArgs(args, "bar.", Sampler::buffer())` | 能 texelFetch、能查尺寸 | SRV + 尺寸 CB |

**区别就是结尾 `_` 还是 `.`**：

```cpp
m_irradianceRayDirections->setShaderArgs(args, "rayDirections.", Sampler::buffer());   // -> Texture2D rayDirections
m_irradianceRayOrigins   ->setShaderArgs(args, "gbuffer_WS_RAY_ORIGIN_", ...);          // -> sampler2D gbuffer_WS_RAY_ORIGIN__buffer
```

### 20.4 `#expect`

```glsl
#expect RAYS_PER_PROBE "int"
#expect OUTPUT_IRRADIANCE
```

G3D 预处理器扩展："**这个宏必须由加载器在编译时提供，缺了就报错**"。`"int"` 只是文档/校验。
无类型的 `#expect FLAG` = 布尔开关。

真正作用是**做 shader 变体**：一个 `OUTPUT_IRRADIANCE` 宏让同一个 `.pix` 同时产出「写辐照度」和「写距离矩」两个版本。

D3D12 等价：`#ifndef X #error`，或按变体编译多个 PSO。

### 20.5 类型系统：`Vector3` / `Point3` / `Color3` 全是 `vec3`

| G3D | 底层 | 语义 |
|---|---|---|
| `Point3` / `Point2` | `vec3` / `vec2` | 位置（齐次 w=1） |
| `Vector3` / `Vector2` | `vec3` / `vec2` | 方向/位移（w=0） |
| `Color3` | `vec3` | 线性颜色 |
| `Radiance3` | `vec3` | 辐亮度 L |
| `Irradiance3` | `vec3` | 辐照度 E |
| `Biradiance3` | `vec3` | 双向辐亮度 |
| `Vector3int32` / `Point3int32` | `ivec3` | 整数三维 |
| `Matrix3` | `mat3` | 3×3 |
| `float2` / `float3` | `vec2` / `vec3` | 弱类型版本 |

只有 4×4 变换时 `Point`/`Vector` 才真有区别（自动补 w=1 / w=0）。
区分 `Color3`/`Irradiance3`/`Radiance3` 纯粹是**防止把量加错** —— 这个习惯值得抄。

`GridHelpers.glsl:76-80` 的 typedef（已发布）：

```glsl
#define ProbeIndex int       // 探针编号 [0, cx·cy·cz)
#define GridCoord  ivec3     // 探针三维网格坐标
#define CycleIndex int       // [0,7]，三个 1-bit 偏移
#define TraceResult int
```

### 20.6 格式对照

| G3D | DXGI_FORMAT | 用在哪 |
|---|---|---|
| `RGB5A1()` | `B5G5R5A1_UNORM` | 辐照度候选 0 |
| `RGB8()` | `R8G8B8A8_UNORM` | 候选 1 / 深度候选 0 |
| `RGB10A2()` | `R10G10B10A2_UNORM` | 候选 2 |
| `R11G11B10F()` | `R11G11B10_FLOAT` | 候选 3（论文推荐） |
| `RGB16F()` | `R16G16B16A16_FLOAT` | 候选 4（**代码默认**） |
| `RGB32F()` | `R32G32B32_FLOAT` → D3D 用 `R32G32B32A32_FLOAT` | 候选 5 / `rayShadedFB` |
| `RG16F()` | `R16G16_FLOAT` | **深度矩默认** |
| `RG32F()` | `R32G32_FLOAT` | 深度候选 2 |
| `R16F()` | `R16_FLOAT` | `distanceFormat()` |
| `RGBA32F()` | `R32G32B32A32_FLOAT` | 所有中间缓冲 |
| `DEPTH32()` | `D32_FLOAT` | 遮罩附件 |

⚠ **D3D12 没有 3 通道 16F/32F 格式**，移植时 `RGB16F` → `R16G16B16A16_FLOAT`。

### 20.7 采样器

| G3D | 语义 | D3D12 |
|---|---|---|
| `Sampler::video()` | 线性 + clamp | `MIN_MAG_MIP_LINEAR` + `TEXTURE_ADDRESS_CLAMP` |
| `Sampler::buffer()` | 最近邻 + clamp | `MIN_MAG_MIP_POINT` + `TEXTURE_ADDRESS_CLAMP` |
| `Sampler::defaults()` | 按纹理编码决定 | 视情况 |

分配：

| 资源 | 采样器 | 为什么 |
|---|---|---|
| 两张 atlas | `video()` 线性 | 需要跨 texel 双线性（gutter 就是为它准备） |
| 所有逐射线缓冲 | `buffer()` 最近邻 | 必须精确命中第 r 条射线，不能插值 |

对应 shader 侧函数也不同：

- `sampleTextureFetch(tex, c, lod)` = `texelFetch` → 精确取值 → 读**射线数据**
- `texture(tex, uv)` → 归一化 UV + 过滤 → 读 **atlas**

### 20.8 渲染状态

| G3D | 作用 | D3D12 |
|---|---|---|
| `setBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` | 开 alpha 混合（= hysteresis） | PSO 的 `RENDER_TARGET_BLEND_DESC` |
| `setDepthTest(DEPTH_GREATER)` | 片段 z > 缓冲 z 才通过 | `COMPARISON_FUNC_GREATER` |
| `setDepthTest(DEPTH_LEQUAL)` | 片段 z ≤ 缓冲 z | `COMPARISON_FUNC_LESS_EQUAL` |
| `setDepthWrite(true)` | 允许写深度 | `DEPTH_WRITE_MASK_ALL` |
| `clear()` | 清颜色+深度 | `ClearRenderTargetView` + `ClearDepthStencilView` |
| `setGuardBandClip2D(n)` | 裁掉 guard band | `RSSetScissorRects` |

> **`DEPTH_GREATER` / `DEPTH_LEQUAL` 这一对不是在做深度测试**，而是把深度缓冲当 1-bit 遮罩（§19.1）。

### 20.9 其它 C++ 符号

| 符号 | 作用 | D3D12 |
|---|---|---|
| `TriTree::create / setContents` | 建/重建 CPU 求交树 | 自己的 BVH，或 DXR BLAS/TLAS |
| `intersectRays(origins, dirs, gbuf, opts)` | 批量求交，结果写 GBuffer | `DispatchRays` |
| `TriTree::DO_NOT_CULL_BACKFACES` | 不求交背面 | 不设 `RAY_FLAG_CULL_BACK_FACING_TRIANGLES` |
| `GBuffer::Specification` + `encoding[Field::X].format` | "我要哪些语义字段、各存什么格式" | 自己的一组 RenderTexture |
| `encoding[Field::X].format = nullptr` | **不要这个字段** | 不创建该纹理 |
| `BEGIN/END_PROFILER_EVENT` | 计时/调试区间 | `PIXBeginEvent/PIXEndEvent` |
| `alwaysAssertM(cond, msg)` | 断言（release 也保留） | `assert` + 日志 |
| `debugPrintf(fmt, ...)` | 格式化输出 | `OutputDebugString` |
| `AABox(lo, hi)` | 轴对齐包围盒 | `BoundingBox` |
| `Array<T>` | 类似 `std::vector` | `std::vector` |
| `shared_ptr<T>` | 引用计数指针 | `ComPtr` |

### 20.10 成员变量

| 变量 | 含义 | 关键行为 |
|---|---|---|
| `m_probeStartPosition` | 网格原点 = `probeDimensions.low()` | 某轴 `counts==1` 时取 `(hi+lo)/2` |
| `m_probeStep` | 相邻探针间距 = `(hi−lo)/(counts−1)` | 除以 **N−1** |
| `m_encloseScene` | 网格"包住场景"(1.1×) 还是"被包住"(0.9/0.7×) | 只影响初始包围盒 |
| `m_maxDistance` | 距离矩上限 = `1.5 × length(bbox/counts)` | miss 也写这个值 |
| `m_firstFrame` | 首帧/参数变化 | 为真时 `hysteresis` 传 **0** ⇒ 直接覆盖 |
| `m_probeFormatChanged` | 格式/分辨率脏标记 | 强制重建 atlas |
| `m_sceneDirty` | 场景脏标记 | 触发 TriTree 重建（限流 0.1s） |
| `m_oneBounce` | 只要一次弹射 | `useProbeIndirect = !m_oneBounce` ⇒ 真时 GI 缓冲换黑色 |
| `m_lightingMode` | `DIRECT_INDIRECT` / `DIRECT_ONLY` / `INDIRECT_ONLY` | 作为 `LIGHTING_MODE` 宏传进 shader |
| `m_debugProbeIndex` | 只显示某个探针 | 其他探针权重置 0 |

**`m_probeStep` 为什么除以 N−1，而 `maxDistance` 除以 N**：

| 量 | 除数 | 要的是 |
|---|---|---|
| `m_probeStep` | `N − 1` | 把 N 个探针均匀铺满包围盒（首尾压在边界上） |
| `m_maxDistance` | `N` | 一个格子的尺度 |

两个口径不同，是有意的。

---

## 21. 逐行注释版：两个关键 draw

### 21.1 `generateIrradianceRays`

```cpp
rd->push2D(m_irradianceRaysFB); {
    // ↑ 绑定 2 个 color attachment：rayOrigins、rayDirections
    //   目标尺寸 = 64 × N（x = 射线号，y = 探针号）

    Args args;
    setShaderArgs(args, "irradianceFieldSurface.");
    // ↑ probeCounts / probeStartPosition / probeStep / 两张 atlas / 各种 bias
    //   以 "irradianceFieldSurface.xxx" 塞进 args → shader 里 uniform IrradianceField irradianceFieldSurface;

    args.setMacro("RAYS_PER_PROBE", 64);
    // ↑ 编译期 #define；shader 里 #expect RAYS_PER_PROBE "int" 要求必须有

    args.setRect(rd->viewport());
    // ↑ 四边形铺满 64×N → 每个 (rayID, probeID) 一次 fragment

    args.setUniform("randomOrientation", Matrix3::fromAxisAngle(Vector3::random(), uniform(0, 2π)));
    // ↑ 每帧一个随机轴 + 随机角 → 整个探针场共用
    //   ⚠ Vector3::random() 是否单位向量未确认，自研时要 normalize

    LAUNCH_SHADER("IrradianceField_generateRandomRays.pix", args);
} rd->pop2D();
```

### 21.2 `updateIrradianceProbe` 的两次 draw

```cpp
rd->push2D(irradiance ? m_irradianceProbeFB : m_meanDistProbeFB); {
    // ↑ 绑定一张 atlas 作为渲染目标；它的 DEPTH32 附件作为"内部/gutter"遮罩

    rd->setBlendFunc(BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA);
    // ↑ 这就是 hysteresis：dst = new*(1-h) + old*h

    rd->setDepthTest(DepthTest::DEPTH_GREATER);
    // ↑ 深度缓冲当遮罩：只有"内部 texel"能通过

    Args args;
    args.setMacro("RAYS_PER_PROBE", 64);
    args.setUniform("hysteresis", m_firstFrame ? 0.0f : 0.98f);
    // ↑ 首帧 h=0 → alpha=1 → 直接覆盖（避免和未初始化显存做 lerp）

    args.setUniform("depthSharpness", 50.0f);
    args.setUniform("maxDistance", m_maxDistance);
    args.setUniform("probeSideLength", ...);         // 8 或 16
    args.setUniform("fullTextureWidth", ...);        // shader 用它算 probesPerRow

    setShaderArgs(args, "irradianceFieldSurface.");

    // 四张"逐射线"缓冲，全部 Sampler::buffer()（最近邻）
    m_irradianceRaysGBuffer->texture(WS_POSITION)->setShaderArgs(args, "rayHitLocations.",  Sampler::buffer());
    m_irradianceRaysGBuffer->texture(WS_NORMAL)  ->setShaderArgs(args, "rayHitNormals.",    Sampler::buffer());
    m_irradianceRayDirections                    ->setShaderArgs(args, "rayDirections.",     Sampler::buffer());
    m_irradianceRaysShadedFB->texture(0)         ->setShaderArgs(args, "rayHitRadiance.",    Sampler::buffer());

    args.setMacro("OUTPUT_IRRADIANCE", irradiance);
    // ↑ 一个 shader 文件 → 两个变体：true 写辐照度，false 写距离矩

    LAUNCH_SHADER("IrradianceField_updateIrradianceProbe.pix", args);
    // ↑ 每个 atlas texel 一次 fragment，内部 for 循环遍历全部 64 条射线
} rd->pop2D();

// ---- 第二次 draw：填 gutter ----
rd->push2D(same atlas); {
    rd->setBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
    rd->setDepthTest(DepthTest::DEPTH_LEQUAL);       // 反向遮罩：只有 gutter 能通过
    args.setUniform("probeTexture", irradiance ? m_irradianceProbes : m_meanDistProbes, Sampler::buffer());
    LAUNCH_SHADER("IrradianceField_copyProbeEdges.pix", args);
} rd->pop2D();
```

---

# 第八部分 · 常数与差异

## 22. 官方常数总表

| 参数 | 值 | 出处 |
|---|---|---|
| 辐照度 tile 边长 `S` | **8** | `IrradianceField.h:16` |
| 距离 tile 边长 `S` | **16** | `:17` |
| `irradianceDistanceBias` | 0.0 | `:21` |
| `irradianceVarianceBias` | 0.02 | `:25` |
| `irradianceChebyshevBias` | 0.07 | `:31` |
| `normalBias` | **0.25** | `:37` |
| `hysteresis` | **0.98**（论文 0.85~0.98） | `:44` |
| `depthSharpness`（`cos^k` 的 k） | **50.0** | `:49` |
| 每探针每帧射线数 | **64** | `:52` |
| `glossyToMatte` | true | `:56` |
| `singleBounce` | false | `:58` |
| 辐照度格式索引 | 4 → **RGB16F** | `:60` |
| 深度格式索引 | 1 → **RG16F** | `:61` |
| `m_maxDistance`（成员默认） | 4.0 | `:103` |
| `m_maxDistance`（运行时） | `1.5 × length(bbox/probeCounts)` | `.cpp:166` |
| `recursiveEnergyPreservation` | **0.85** | `.cpp:10` |
| `energyConservation`（更新侧） | **0.95** | `.pix:70` |
| `epsilon`（权重阈值） | **1e-6**（论文写 1e-3） | `.pix:43` |
| `crushThreshold` | **0.2** | `.pix:214` |
| `rayMinDistance`（tMin） | **0.08** | `generateRandomRays.pix:101` |
| `hitBias`（命中点沿法线外推） | **0.01** | `.pix:84` |
| 权重下限 | **1e-6** | `.pix:199` |
| 探针计数 | 2 的幂（索引用位运算） | `.h:275` |
| 探针间距（论文） | 1~2 m（人尺度场景） | 论文 §4.1 |

---

## 23. 论文 vs 代码：10 处差异

| # | 论文正文 | 官方代码 | 结论 |
|---|---|---|---|
| 1 | Eq(1)：`lerp(old, Σ(…), hysteresis)` | 输出 `alpha = 1−h`，`SRC_ALPHA/ONE_MINUS` ⇒ `(1−h)·new + h·old` | **论文 Eq(1) 参数方向与正文/代码相反**；以代码为准 |
| 2 | Eq(1) 是**裸和**，无归一化 | `result.xyz /= result.w`（Σw） | Eq(1) 省略了 `/Σw`；**必须归一化** |
| 3 | 阈值 **0.001** | **1e-6** | 对辐照度分支等价；对 `cos^50` 分支 = 40.7° vs 29.4° |
| 4 | "cosine-power lobe" | `depthSharpness = 50` | 指数 50（论文没给，唯一权威来源） |
| 5 | 降低**强度** <5% 的辐照度 | `crushThreshold=0.2`，压的是**权重** `25w³` | 量纲不同；曲线形式代码给全 |
| 6 | "mean- and variance-**biased** Chebyshev" | `.pix` 里**完全没用** `irradianceChebyshevBias`(0.07) / `irradianceVarianceBias`(0.02) | zip 里的 `.pix` 是较早版本 |
| 7 | Fig.3："padding aligns probes on 4×4 write boundaries" | 只有 1px gutter + 全局 2px 偏移，无额外 padding | 32bpp 格式不需要 4×4 对齐 |
| 8 | "tetrahedral" | 仅 §4.1 提到探针可布置在四面体**网格**上 | **不存在四面体插值** |
| 9 | §3：距离矩 `RG16F`，辐照度 `R11G11B10F` | 深度默认 `RG16F` ✅；辐照度默认索引 4 = **RGB16F** ❌ | 辐照度格式与论文不一致 |
| 10 | 未提 | `energyConservation=0.95`（每射线）、`energyPreservation=0.85`（每次采样） | 两个独立经验因子，可调 |

---

## 24. 四个常见误解（都不用做）

### 24.1 不需要八面体立体角 Jacobian

射线方向在球面上均匀分布，texel 方向只在 `max(0, d·ω)` 里作为余弦叶瓣轴向出现。
整个积分 `∫L(ω)max(0,d·ω)dω` 在**方向域**上做，与"texel 覆盖多少立体角"无关 ——
texel 是**存储**格点，不是**积分**格点。

官方实现也不乘任何 Jacobian。加上它反而会让角部 texel 被系统性放大。

### 24.2 不需要跨 tile 八面体接缝重建

每探针是独立 tile，UV 按 tile 计算，**永不跨 tile 读取**。
gutter 只服务 tile **内部**的双线性，`±1` 复制正是正确做法（等价 clamp-to-edge）。

### 24.3 不需要四面体插值

论文 §4.1 提到的 "tetrahedral" 是探针**布置**方式，不是插值方式。
官方就是 `for i in 0..7` 的八点三线性。

### 24.4 不需要 probe relocation（如果对标论文原版）

论文明确说落在墙里的探针"会被 visibility-query metrics 忽略" —— 加了 Chebyshev 就自愈。
relocation 是 Unity HDRP 等后续变体，不是本文内容。

---

# 第九部分 · 落地

## 25. 移植到 D3D12 的逐项对照

| 官方做法 | D3D12 建议 | 理由 |
|---|---|---|
| PS + framebuffer，一次全屏 draw 覆盖整张 atlas | Compute `Dispatch(probeCount,1,1)`，一 group 一探针 | UAV 无法 blend，官方那套依赖 ROP |
| depth buffer 当 1-bit 遮罩 | shader 里 `if (local.x==0 \|\| local.x==S+1) return;` | 更直接，无额外附件 |
| hysteresis 靠 blend state | shader 里手动 `lerp` | UAV 不能和 blend 共存 |
| 图集：列 = `x + y·cx`，行 = `z` | 列 = `x`，行 = `y + z·cy` | 都能用，别轻易改 |
| 射线存 `64 × N` 纹理，update 里 for 64 次 | 同（一次 dispatch 里 for 循环） | 结构一致最省事 |
| `tMin=0.08 / tMax=inf` 存在 `.w` | `RayDesc.TMin / TMax` | 给 helper 加参数 |
| `hitBias = 0.01` | 命中点沿法线外推 0.01 | 防贴面自遮挡 |
| `DO_NOT_CULL_BACKFACES` | 不设 `RAY_FLAG_CULL_BACK_FACING_TRIANGLES` | |
| 每帧一次全局随机旋转 | 同 | 时间累积能生效的前提 |
| 场景脏才重建求交树（≤10Hz） | 静态场景无所谓；有动态物体时要处理 | 否则 TLAS 每帧重建太贵 |
| `RGB16F` 辐照度 | `DXGI_FORMAT_R16G16B16A16_FLOAT` | D3D 无 3 通道 |
| `RG16F` 距离矩 | `DXGI_FORMAT_R16G16_FLOAT` | 一致 |
| 全局 2px 图集边框 | 可以省，但必须用 **CLAMP** 采样器 | 否则最外层探针会环绕到图集另一侧 |

### 25.1 采样器：现在是安全的，但很脆弱

本项目 `ShadersCommon.hlsl:44` 只声明了 `sampler g_LinearSampler : register(s0)`，
而 `RootSignature.cpp:26-30` 里 **s0 是 WRAP**。`SampleProbeIrradiance` / `SampleProbeDistance`
都用它采样图集。

**复核结论（2026-11-01）：目前不会出错**，因为 UV 的上界刚好卡在安全位置：

```
atlasUV 对应的连续像素坐标 p ∈ [tileOrigin+1, tileOrigin+1+S]
最外层探针的 p_max = atlasW - 1
双线性在 p 处取 floor(p - 0.5) 与 +1 两个 texel
p = atlasW-1 ⇒ taps {atlasW-2, atlasW-1}  ← 都在图集内，不会环绕
```

也就是说 `tilePx` 里那个 `+1`（跳过 gutter）恰好把上界压到了 `W-1`。

**但这是"碰巧对"**，一旦以下任一条件变化就会出错：

- 图集外加了 1px 全局边框（照抄官方的 `+2`）
- 把 `tilePx` 的 `+1` 去掉
- `OctEncode` 因浮点误差返回略大于 1 的值
- 换成 `Load + 手算双线性` 与 `SampleLevel` 混用

`RootSignature.cpp:35-38` 已经备好 **s1 = CLAMP**。建议直接切过去，代价是加一行：

```hlsl
sampler g_ClampSampler : register(s1);
```

然后 `SampleProbeIrradiance` / `SampleProbeDistance` 里把 `g_LinearSampler` 换成 `g_ClampSampler`。

---

## 26. 常见陷阱清单

| # | 陷阱 | 后果 | 规避 |
|---|---|---|---|
| 1 | 每帧射线固定不旋转 | 时间累积原地空转，banding 永不消失 | 每帧一个全局随机旋转 |
| 2 | 除以 N 而不是 Σw | 亮度暗 4 倍 | `/Σw` |
| 3 | 距离矩复用颜色通道 Σw | 距离矩错乱 | 独立 Σw |
| 4 | 采样不归一化 Σw | 墙根/墙角发黑一圈 | `/= sumWeight` |
| 5 | 首帧 `lerp(NaN, x, 1)` 仍是 NaN | 图集永久 NaN 传染 | 首帧直接赋值，不 lerp |
| 6 | 用环形帧号做累积 | 每 3 帧被 `alpha=1` 冲掉 | 用单调帧号 |
| 7 | 图集状态被两套账本追踪 | `StateBefore` 错，Debug Layer 报错 | 状态只由 RenderGraph 管 |
| 8 | 距离矩的 target 索引默认 0 | 把别家 SRV 当 UAV 写，静默写坏 | 确保真值，加 assert |
| 9 | 用 `tMax = 100` | half 精度崩，方差算成 0 | `1.5 × 单元对角` |
| 10 | 把采样与写 atlas 放同一 dispatch | UB / 闪烁 | 拆两个 pass，或 ping-pong |
| 11 | 用 WRAP 采样器采图集 | 最外层探针串到图集另一侧 | 用 CLAMP |
| 12 | 把 `a` 通道当不透明度 | 混合系数错 | `a = 1 − hysteresis` |

---

## 27. 验证方法

### 27.1 White furnace test（5 分钟，最重要）

设环境为"处处 radiance = 1"：`SkyColor()` 返回 `(1,1,1)`、所有 albedo = 1、**关掉直射光**。

解析解：任何法线方向 `E = π` ⇒ 探针 texel 应为 `È = E/π = 1.0` ⇒ 最终像素应为 1。

| 读到 | 说明 |
|---|---|
| **1.0** | ✅ 归一化、cos 权重、oct 编解码全对 |
| 0.25 | 除以 N 了（应为 `/Σw`） |
| π | 多乘了一个 π |
| 0.5 | 抄了 G3D 的 `0.5*π`（那是他们管线的约定） |

> 测试前要把"命中点 `+ SkyColor()*0.3`"这类 hack 和直射项一起关掉。

### 27.2 其它三个测试

| 测试 | 怎么设 | 看什么 | 说明什么 |
|---|---|---|---|
| 单色环境 | `SkyColor=(0.2,0.3,0.5)` | atlas 应均匀无方向条纹 | 射线分布均匀、无结构性 bias |
| 收敛性 | 场景静止连跑 200 帧 | 纹路应几乎不变 | 时间累积已收敛 |
| 对照 | debug 视图并排：光追 GI vs 探针 GI | 亮度量级接近、低频结构一致 | 整体校准正确 |

### 27.3 建议的 debug 视图

| 内容 | 用途 |
|---|---|
| 单探针 oct 采样（按法线方向） | 看单个探针内容 |
| atlas 1:1 显示 | 验证 tile 布局 / gutter |
| 只显示探针间接光 `albedo × probeIrr` | 与光追 GI 对照 |
| 距离矩 `mean / maxDistance` 灰度图 | 验证距离矩在写 |
| 可见性项单独输出 | 调 Chebyshev |

---

## 28. 分阶段路线（摘要）

详见 `00-implementation-plan.md`。核心顺序：

> 状态列按 **2026-11-01** 的代码复核结果标注，详见第十部分。

| 阶段 | 目标 | 关键点 | 状态 |
|---|---|---|---|
| **P0** | 能编过能跑，画面不变 | 统一 pass 接口；删掉坏死的死代码 | ✅ **已完成** |
| **P1** | 探针产出真正的辐照度 | 每 texel 遍历 N 条射线 + `/Σw`；**白炉测试** | ❌ **未做**（当前最大缺口） |
| **P2** | 画面第一次用上探针 | 采样函数已写；缺"接进最终着色"这一步 | 🟠 **一半**（只在 debug 8） |
| **P3** | 8 探针三线性 + 权重链 | 三线性 ✅；backface/crush/sqrt 未对齐 | 🟠 **一半** |
| **P4** | 距离矩 + Chebyshev | 链路 ✅；`cos^50` 与 `maxDistance` 未对齐 | 🟠 **大半** |
| **P5** | 多弹射 | 拆成"shade"和"update"两个 pass | ❌ 未做 |

**顺序不能反**：先接消费端（能看到东西），再修生产端（看到对的东西）。
反过来的话，你会在"改了算法但画面没变化"里迷失很久。

---

# 第十部分 · 本项目现状对照

> 复核时间：**2026-11-01**（Asia/Shanghai）。行号以复核时的磁盘内容为准。
> 实测证据：`dxc` 编 4 个入口点全部 `exit 0`；6 个关键 TU 单独 `cl /c` 全部通过。
> 链接期未验证（未跑完整 MSBuild）。

## 29. 进度总表

### 29.1 已完成的

| 环节 | 状态 | 位置 |
|---|---|---|
| C++ 编译 | ✅ | `ProbeBorderFixupPass.h:30` 补了 `m_countX/Y/Z`；`App.cpp:239-252` 按 11 参调用 |
| HLSL 编译 | ✅ | `ProbeUpdateCS` / `ProbeDistanceCS` / `BorderFixupCS` / `LightingPS` 四个入口 dxc 全 `exit 0` |
| `ProbeDistanceCS` 线程模型 | ✅ 已修 | `ShadersProbe.hlsl:68-81` 用 `groupId.x` + `gtid.xy`，去掉了图集空间边界检查 |
| `BorderFixupCS` 参数化 | ✅ 已修 | `ShadersProbe.hlsl:112-113` 用 `borderTileStride` / `borderTargetUav` |
| 距离矩 atlas 注册 | ✅ | `App.cpp:185-186` → `m_probeDistanceId` |
| `ProbeDistancePass` | ✅ 新建 | `ProbeDistancePass.h/.cpp`，已进 vcxproj 与 RenderGraph |
| 两个 gutter 实例 | ✅ | `App.cpp:239-244`（stride 10）+ `App.cpp:247-252`（stride 18） |
| `distanceUav` 真值 | ✅ 已解除陷阱 #8 | `ProbeDistancePass.cpp:37` |
| Lighting 读距离矩 | ✅ | `LightingPass.cpp:55` Declare + `:83` 传 `distanceAtlasIndex` |
| pass 顺序 | ✅ | `App.cpp:400-406` |
| 绑定量布局 | ✅ | `LightingTypes.h:17` ↔ `ShadersCommon.hlsl:242`，均 64 B |
| 探针采样函数 | ✅ 已写 | `ShadersLighting.hlsl:35-158` |
| oct UV 公式 | ✅ **正确** | `ShadersLighting.hlsl:43,56` 等价于官方 `tileOrigin + 1 + oct01·S` |
| 图集索引 | ✅ **正确** | `probeIndex % cx` / `probeIndex / cx` 与本引擎布局（列=x、行=y+z·cy）自洽 |

**pass 顺序实测**：

```
GBuffer -> ComputeSmoke -> ProbeUpdate -> irr gutter -> ProbeDistance -> dist gutter -> Lighting -> Present
  400          401           402           403            404              405            406
```

这正是 §13 官方四步的等价拆解，**而且比上次复核多了距离矩那两步**。

### 29.2 还没做的

| 缺口 | 严重度 | 说明 |
|---|---|---|
| **`ProbeUpdateCS` 仍是 scatter** | 🔴 最高 | 1 texel 1 射线、无 cos 加权、无 `/Σw`、无随机旋转（§32.1） |
| **`SampleDDGI` 没接进最终着色** | 🔴 | 只在 `debugMode==8` 里被调用；最终像素仍走 `TraceGI`（`ShadersLighting.hlsl:251-270`） |
| **`ProbeDistanceCS` 权重/距离未对齐** | 🟠 | 每 texel 一条自身射线（k→∞）、`tMax=100`、无 `cos^50`（§32.2） |
| `SampleDDGI` 权重链偏离官方 7 处 | 🟠 | backface / crush / sqrt / energy / bias / pow3 / debug÷π（§31） |
| 首帧 lerp NaN 风险 | 🟡 | `ShadersProbe.hlsl:53-54, 97-101` |
| 多弹射 | ⚪ | 需拆 pass（§12.1） |

---

## 30. 你的代码 ↔ 本文章节 ↔ 官方 三方对照

| 你的代码 | 本文对应 | 官方对应 | 一致性 |
|---|---|---|---|
| `ProbeGrid.h` tile/stride 常量 | §4.3 | `IrradianceField.h:16-17` | ✅ 8/10、16/18 全同 |
| `ProbeGrid::Desc` 网格参数 | §3.1 | `Specification` | ✅ 2 的幂；⚠ `probeStep` 用固定 spacing，不是 `(hi−lo)/(N−1)` |
| `ProbeUpdateCS` | §6、§8.1 | `updateIrradianceProbe.pix` | ❌ **结构不同**（scatter vs gather） |
| `ProbeDistanceCS` | §8.1、§8.3 | 同文件 `#else` 分支 | 🟠 线程模型对了；权重与 `maxDistance` 未对齐 |
| `BorderFixupCS` | §4.3、§19.1 | `copyProbeEdges.pix`（未发布） | ✅ 等价；用 shader 分支替代了深度遮罩 |
| `SampleProbeIrradiance` | §4.3、§10⑥ | `textureCoordFromDirection` | ✅ UV 公式等价 |
| `SampleProbeDistance` | §10③ | 同上（depth 版） | ✅ 结构等价 |
| `SampleDDGI` cage + 三线性 | §9 | `sampleIrradianceField.pix:111-148` | ✅ 二次 clamp、权重和恒为 1 |
| `SampleDDGI` 权重链 | §10 | 同上 `:155-220` | 🟠 见 §31 |
| `LightingPass` 传参 | §14 | `setShaderArgs(prefix)` | ✅ 布局正确 |

> 与官方的**布局差异**保持原样即可：你的图集是「列=x、行=y+z·cy」（§4.4），
> 官方是「列=x+y·cx、行=z」。两者都自洽，**不要为了对齐而改**（要改得同步改 4 处）。

---

## 31. `SampleDDGI` 逐行评审

`ShadersLighting.hlsl:91-158`。**结构完全正确**，问题都在权重细节上。

### 31.1 先确认对的部分（这些不用动）

```hlsl
int3 idx = clamp(int3(base) + corner, int3(0,0,0), int3(counts) - 1);   // 二次 clamp，同官方
uint probeIndex = (idx.z * counts.y + idx.y) * counts.x + idx.x;        // 与本引擎布局一致
float3 w3 = lerp(1.0f - f, f, float3(corner));                          // 正确
float w = w3.x * w3.y * w3.z;                                           // 正确
return irradiance / max(wsum, 1e-4f);                                   // 归一化，陷阱 #4 已避开
```

- `float3 Pn = P + N * 0.02f` 后 `gridPos = (Pn - origin)/spacing`：
  越界时 `base` 出界，但 `idx` 被 clamp 到同一格，两个角点相同 ⇒ **权重和恒为 1**，不会变暗。✅ 安全
- `SampleProbeDistance(probeIndex, -dirToProbe)`：方向是「探针 → 着色点」✅ 与官方 `-dir` 一致
- `dist` 用的是 biased 后的距离 ✅ 与官方 `length(probeToPoint)` 一致

### 31.2 偏离官方的 7 处

#### ① backface 权重（影响最大）

```hlsl
// 你的（:129-132）
float backface = saturate((dot(N, dirToProbe) + 0.2f) / 1.2f);
backface *= backface; backface *= backface;        // 四次方
w *= backface;

// 官方（sampleIrradianceField.pix:171）
w *= square(max(1e-4, (dot(toP, n) + 1.0) * 0.5)) + 0.2;
```

| `dot` 值 | 你的权重 | 官方权重 |
|---|---|---|
| +1（正对） | 1.00 | 1.20 |
| 0（切平面） | 0.00077 | 0.45 |
| −1（背面） | 0 | 0.20 |

**后果**：官方注释专门讨论过这件事 ——
"for small details inside of a room, the normals on the details might rule out **all** of the probes
that have mutual visibility to the point"。
你的版本在切平面处直接掉到 ~0，小物件/墙角处 8 个探针可能全部被 `if (w <= 1e-5f) continue;` 掉 →
`wsum = 0` → **输出全黑斑**。

`+0.2` 在官方是**加性下限**（保证最低 0.2），你的写法把它当成了分子的一部分，下限作用消失。

**改法**：换成官方那一行即可。

#### ② 缺 crush（低辐照度抑制）

官方（`sampleIrradianceField.pix:214-217`）：

```glsl
if (weight < 0.2) weight *= weight * weight * (1.0 / 0.04);   // = 25w³
```

你没有。后果：暗处的微量漏光不被压制 —— 而这正是论文 Figure 7(e)(f) 在压的东西。

**改法**：在 `if (w <= 1e-5f)` **之前**插入（顺序必须在三线性之后）。

#### ③ 缺 sqrt 感知空间混合

官方逐项 `sqrt` → 加权平均 → `square`；你是线性平均。
后果：探针之间的亮度台阶更硬（平移时能看到"跳变"）。

#### ④ 缺 `energyPreservation = 0.85`

多弹射上线后"暗室越照越亮"。现在没有多弹射，暂时看不出来。

#### ⑤ normal bias 只有法线项且量级偏小

| | 你的 | 官方 |
|---|---|---|
| 表达式 | `P + N * 0.02` | `(n + 3·ω_o) * 0.25` |
| 量级 | 0.02 | 0.25（且沿视线方向还有 3 倍） |

`ω_o` 在 shader 里现成：`normalize(g_Frame.cameraPos.xyz - world.xyz)`。

#### ⑥ Chebyshev 没有 `pow3`

官方 `cheb = max(pow3(cheb), 0.0)`；你是**一次幂**。
后果：**间接阴影偏软**（对比度不足）。这是最容易被误判成"算法不对"的一项。

#### ⑦ `debugMode == 8` 多除了一次 π

```hlsl
float3 E = SampleDDGI(world.xyz, N);
return float4(E / 3.14159265f, 1.0f);      // :207
```

若探针存的是 `È = E/π`（§2.2 的官方语义），最终像素就应该是 `albedo * È`，**不该再除 π**。
现在除 π 是因为 `ProbeUpdateCS` 存的是原始 radiance、语义尚未统一，属于"临时凑亮度"。

**P1 做完后必须删掉这个 `/π`**，否则你会带着一个 π 的系统偏差去调别的东西。

---

## 32. 探针更新：当前语义与官方差在哪

这是**当前唯一还没对上的核心算法**。

### 32.1 `ProbeUpdateCS`（`ShadersProbe.hlsl:16-55`）

```hlsl
uint2 texel = uint2(id.x % 8, id.y);
float2 uv = (float2(texel) + 0.5f) / 8.0f;
float3 dir = OctDecode(uv);
float3 radiance = TraceRadiance(probePos + dir * 0.02f, dir);   // 每个 texel 一条自己的射线
...
float alpha = (frameIndex == 0u) ? 1.0f : 0.05f;
atlas[pixel] = float4(lerp(old.rgb, radiance, alpha), 1.0f);
```

对照 §8.1：

| | 你的 | 官方 |
|---|---|---|
| texel 语义 | `L(dir)` 方向辐亮度 | `È = E/π` 余弦加权平均 |
| 每 texel 射线数 | **1** | **64**（遍历全部） |
| 余弦加权 | ❌ | ✅ `max(0, dot)` |
| 归一化 | ❌（直存） | ✅ `/Σw` |
| 每帧随机旋转 | ❌ | ✅ 全局随机旋转 |
| 时间累积是否有效 | **无效**（每帧同一条射线） | 有效 |

**两个独立后果**：

1. `SampleDDGI` 里 `SampleProbeIrradiance(probeIndex, N)` 取到的是"法线方向那一条射线的 radiance"，
   不是辐照度 → 方向混合缺失、色溢偏弱、亮度量级不对。
2. `frameIndex` 带来的 EMA 只是在平均**同一个确定性值**：
   `avg_n = L·(1 − (1−α)^n) → L`，第 1 帧之后原地空转，**banding 一条都不会消**。

**改造要点**见 §8.1：`SV_GroupID` 取探针、`gtid.xy` 取 texel、内层 `for` 遍历 `rayCount` 条
`StochasticFibonacci`、`cos` 加权、`/Σw`、`w<1e-6` 跳过、`Σw<eps` 时保留旧值、首帧直接赋值。

### 32.2 `ProbeDistanceCS`（`ShadersProbe.hlsl:59-102`）

线程模型已经对了 ✅，但**权重和距离尺度**还不对齐：

| 项 | 你的 | 官方 |
|---|---|---|
| 每个 texel 的样本数 | 1（沿自身方向） | 64（`cos^50` 加权累加） |
| 权重 | 无（等价 k→∞） | `max(0,dot)^50` |
| 阈值 | 无 | `1e-6` |
| `tMax` | **100** | `1.5 × length(bbox/counts)` ≈ **1.37** |
| miss 值 | 100 | `maxDistance` |
| hitBias | 无（改用 `dir*0.02` 偏移起点） | `0.01` 沿命中法线 |

**`tMax=100` 的直接后果**：`mean2` 到 `t² = 10⁴`，而 `variance = moments.y − mean²` 是两个接近大数相减，
half 精度下可能算成 0 → `cheb = 0/(0+d²) = 0` → **该探针恒不可见**；也可能算爆。
所以即便 Chebyshev 写得对，看起来也会"要么全黑、要么完全没遮挡"。

> 建议：`maxDistance = 1.5 * length(spacing * counts)` 算一次（本网格 ≈ 1.37）传给 shader，
> 同时把它当 `RayDesc.TMax`。

### 32.3 改成 gather 之后要跟着改的地方

`ProbeUpdateCS` 改成 gather 后，texel 语义从 `L(dir)` 变成 `È`。届时：

- `debugMode == 8` 的 `/π` 要删（§31⑦）
- 探针间接光并入最终着色时直接 `albedo * SampleDDGI(...)` 即可（§2.2）

---

## 33. 剩余 TODO（按收益 / 成本排序）

| 优先级 | 事项 | 位置 | 成本 | 收益 |
|---|---|---|---|---|
| **P1** | `ProbeUpdateCS` 改 gather + cos + `/Σw` + 每帧旋转 | `ShadersProbe.hlsl:16-55` | 中 | **决定 DDGI 是对是错** |
| **P2** | `SampleDDGI` 接进最终着色（替换 `indirect`） | `ShadersLighting.hlsl:251-270` | 小 | **第一次真正看到 DDGI** |
| **P2** | 删 `debugMode 8` 的 `/π` | `:207` | 极小 | 避免系统偏差 |
| **P3** | backface 换官方形式 | `:129-132` | 极小 | 消除黑斑 |
| **P3** | 加 crush | `:147` 前 | 极小 | 压暗处漏光 |
| **P3** | 加 sqrt 混合 | `:153-157` | 小 | 软化探针台阶 |
| **P3** | Chebyshev 加 `pow3` | `:143-144` | 极小 | 间接阴影对比度 |
| **P4** | `ProbeDistanceCS` 加 `cos^50` + 收 `tMax` | `ShadersProbe.hlsl:59-102` | 中 | 让可见性真的生效 |
| **P4** | normal bias 加视线项 | `:98` | 小 | 减少明暗漏光 |
| **P5** | 首帧改成直接赋值（不 lerp） | `:53-54`、`:97-101` | 极小 | 防 NaN |
| **P5** | 多弹射（拆 pass） | `ProbeUpdateCS` | 大 | 无限弹射 |

**建议顺序：P1 → P2 → P3 → P4**。
P1 和 P2 之间不要插别的：P1 让数据变对，P2 让数据可见，这两步做完才有可判断的基准。

> ⚠ P2 接入最终着色时，**保留 `TraceGI` 并留一个开关**。
> 没有对照基准，你无法判断"探针 GI 偏暗"是归一化错了、还是本来就该那么暗。

---

# 附录

## A. 证据边界

补充材料只发布了 7 个文件。被调用但**未发布**的 shader，其内部内容只能由调用点 + 论文推断：

| 文件 | 是否发布 | 本文如何处理 |
|---|---|---|
| `IrradianceField_updateIrradianceProbe.pix` | ✅ | 逐行实证 |
| `IrradianceField_generateRandomRays.pix` | ✅ | 逐行实证 |
| `sampleIrradianceField.pix` | ✅ | 逐行实证 |
| `GridHelpers.glsl` / `util.glsl` | ✅ | 逐行实证 |
| `IrradianceField_copyProbeEdges.pix` | ❌ | 由调用点 + depth 遮罩反推 |
| `IrradianceField_writeOnesToProbeBorders.pix` | ❌ | 只能反推"写两种深度值" |
| `GIRenderer_computeIndirect.pix` | ❌ | 由 `sampleIrradianceField` + 输出名反推 |
| `GIRenderer_deferredShade.pix` | ❌ | 由传入的 GBuffer 字段 + macro 反推 |
| `octahedral.glsl` | ❌ | 用标准实现替代 |

**复现命令**（用于确认拿到的是同一版本）：

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
> 各文件 SHA-256 见 `04-reference-formulas.md` §0.2。

## B. 更新记录

| 日期 | 内容 |
|---|---|
| 2026-11-01 | **按代码复核结果新增第十部分（§29–§33）**。实测：4 个 shader 入口 dxc 全通过、6 个关键 TU `cl /c` 全通过。记录：距离矩链路 + 双 gutter 实例已接通；`SampleDDGI` 已写但未接入最终着色；`ProbeUpdateCS` 仍是 scatter（当前最大缺口）。新增 `SampleDDGI` 逐行评审（7 处偏离官方）、探针更新语义对照、剩余 TODO 表。同时**订正 §25.1**：WRAP 采样器目前因 UV 上界碰巧安全，不再表述为"已踩的坑"，改为"安全但脆弱"。 |
| 2026-10-31 | **重建为 DDGI 唯一主文档**。合并原 05（官方渲染流程）+ 原 06（G3D 惯用法），并补入原理、数据结构、更新、采样、多弹射五大部分；新增 §2 数学基础、§11 能量约定、§24 四个常见误解、§26 陷阱清单。后续所有 DDGI 讲解一律追加到本文件。 |
| 2026-10-31 | 新增资源读写真值表（启动期表格、每 pass 读写矩阵、内容级写入清单、生产者→消费者、内存账）。 |
| 2026-10-31 | 初版：官方实现四步管线、三个设计精髓。 |
