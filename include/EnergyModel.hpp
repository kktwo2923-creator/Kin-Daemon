// 帕累托前沿 / 相对能效模型（EEM）—— P2-1 的算法内核
//
// 背景：LoadGovernor 的 `target = util × headroom` 是「够用就停」，但它不知道
// **该用几颗核、跑到多少频**才是能效最优。同一个算力需求可以：
//   1 核 @ 3.5GHz  ≈ 3.8W      3 核 @ 1.15GHz ≈ 1.2W
// 后者省 68%（电压低得多，且 P ∝ f·V²）。帕累托前沿要回答的就是这件事。
//
// 核心洞察（来自 ChiRi 第二代 EEM）：**绝对能效曲线不参与任何决策**。
// 单核功耗与单核算力各自归一成相对向量，之后全部比较都在无量纲空间里做：
// 「同算力下取更低功耗」。因此功耗模型只要有正确的**形状**就够，绝对标定不重要。
//
// 两条来自 ChiRi 的反面教训，改本文件时勿回退：
//   [1] 不能用「整簇满载」假设成本。调度器搬的是**线程** —— 1 线程跑在 6 核簇上是
//       「1 核忙 5 核闲」，簇频照样拉满。旧版 demand_buckets 两轴都按整簇满载算，
//       sm8750 big 顶档高估 5.85×，且首桶下限高于单线程满打满算 → 前沿对单线程从不生效。
//       本文件一律按**忙核数 k** 算：cost = pow[g]×k、supply = cap[g]×k。
//   [2] 甜点判据必须用严格 `dp <= 0` 判零增量，不能用 EPSILON 之类绝对阈值 ——
//       相对向量整体缩放后增量会同比例变小，绝对阈值会把「不免费」误判成「免费」，
//       甜点随缩放漂移。
//
// 与 ChiRi 的差异（有意为之）：
//   · **只做簇内归一，不做跨簇比较**。跨簇比较需要可信的绝对功耗标定；Kin 没有，
//     而「扣掉首档静态开销」会让 prime 因动态增量小而永远胜出（ChiRi 明确踩过）。
//   · **不做迁移侧**。那需要逐线程 util（eBPF sched_switch 记账），Kin 是静态 bionic
//     二进制拿不到。只做调频侧：把上限封在甜点档。
//
// 零 IO：曲线由调用方喂进来，本文件只做纯计算，可在 Windows 主机单测。
#pragma once

namespace kin {

// 单簇单核能效曲线。构造期一次算定，运行期只做下标访问。
struct EnergyCurve {
    static constexpr int kMax = 40;
    int   freqKhz[kMax] = {};   // 升序
    float capRel[kMax]  = {};   // 簇内归一：首档 0、末档 1
    float powRel[kMax]  = {};   // 簇内归一：首档 0、末档 1
    int   n        = 0;
    int   sweetIdx = 0;

    bool valid() const { return n >= 2; }
    int   sweetKhz() const { return (n > 0) ? freqKhz[sweetIdx] : 0; }
    // 供给/成本一律按**忙核数 k** 算（教训 [1]）
    float supply(int gear, int k) const {
        if (gear < 0 || gear >= n) return 0.0f;
        const int kk = k < 1 ? 1 : k;
        return capRel[gear] * (float)kk;
    }
    float cost(int gear, int k) const {
        if (gear < 0 || gear >= n) return 0.0f;
        const int kk = k < 1 ? 1 : k;
        return powRel[gear] * (float)kk;
    }
};

namespace detail {
inline float clamp01(float v) { return (!(v > 0.0f)) ? 0.0f : (v > 1.0f ? 1.0f : v); }
}  // namespace detail

// 甜点下标：`ΔcapRel/ΔpowRel` **最后一次** ≥ 1.0 的档。
// 该段 Δpow == 0 视为无限边际能效；全程斜率都 < 1.0 → 0（首档即最优）。
inline int sweetIndex(const float* capRel, const float* powRel, int n) {
    if (!capRel || !powRel || n < 2) return 0;
    int sweet = 0;
    for (int i = 1; i < n; ++i) {
        const float dc = capRel[i] - capRel[i - 1];
        if (dc <= 0.0f) continue;
        const float dp = powRel[i] - powRel[i - 1];
        // 严格 <= 0 才算「免费」。见文件头教训 [2]。
        const float eff = (dp <= 0.0f) ? 1e30f : dc / dp;
        if (eff >= 1.0f) sweet = i;
    }
    return sweet;
}

// 簇内归一：首档扣 0、末档拉到 1。退化（<2 档 / 末档不大于首档）→ 返回 false。
inline bool normalizeCluster(const float* raw, int n, float* out) {
    if (!raw || !out || n < 2) return false;
    const float lo = raw[0], hi = raw[n - 1];
    if (!(hi > lo)) return false;
    for (int i = 0; i < n; ++i) out[i] = (raw[i] - lo) / (hi - lo);
    return true;
}

// 构造曲线。capRaw 必须是**单核**算力（capacity × freq 之类，同簇内单调即可）；
// powRaw 必须是**单核**动态功耗（整簇功耗 ÷ 核数，或单核实测）。
// 两者长度须与 n 一致且 freq 升序。任一步退化 → 返回 false（调用方回退比例路径，不猜）。
inline bool buildCurve(const int* freqKhz, const float* capRaw, const float* powRaw, int n,
                       EnergyCurve& out) {
    out.n = 0;
    out.sweetIdx = 0;
    if (!freqKhz || !capRaw || !powRaw || n < 2 || n > EnergyCurve::kMax) return false;
    for (int i = 0; i < n; ++i) {
        if (freqKhz[i] <= 0) return false;
        if (i > 0 && freqKhz[i] <= freqKhz[i - 1]) return false;   // 必须严格升序
        out.freqKhz[i] = freqKhz[i];
    }
    if (!normalizeCluster(capRaw, n, out.capRel)) return false;
    if (!normalizeCluster(powRaw, n, out.powRel)) return false;
    out.n = n;
    out.sweetIdx = sweetIndex(out.capRel, out.powRel, n);
    return true;
}

// 解析式功耗模型：P = f × V(f)²，V 在 [fmin, fmax] 上从 vminMv 线性升到 vmaxMv。
// 用于没有实测功耗表、但设备树能给出 opp-microvolt 的情况（P = C·f·V² 是 ChiRi 给
// sm8550 用的口径，那里叫「推理功耗」）。
// 输出写进 powMw[]（绝对值，调用方随后归一）。vmin/vmax <= 0 时返回 false。
inline bool analyticPower(const int* freqKhz, int n, float vminMv, float vmaxMv, float* powMw) {
    if (!freqKhz || !powMw || n < 1 || !(vminMv > 0.0f) || !(vmaxMv >= vminMv)) return false;
    const float f0 = (float)freqKhz[0];
    const float f1 = (float)freqKhz[n - 1];
    const float span = (f1 > f0) ? (f1 - f0) : 1.0f;
    for (int i = 0; i < n; ++i) {
        const float f = (float)freqKhz[i];
        const float v = vminMv + (vmaxMv - vminMv) * ((f - f0) / span);
        powMw[i] = f * v * v;
    }
    return true;
}

// 该需求在该簇上「刚好够用」的档位下标；-1 = 需求超出允许范围。
// allowAboveSweet=false 时封顶在甜点档（需求装不下也不越界，交给调用方的抗饥饿逻辑）；
// true 时允许用到全档（本簇占用已达 ceil(n/2)，迁移无处可去）。
inline int gearForDemand(const EnergyCurve& c, float demand, int k, bool allowAboveSweet) {
    if (!c.valid() || !(demand > 0.0f)) return -1;
    const int limit = allowAboveSweet ? (c.n - 1) : c.sweetIdx;
    for (int g = 0; g <= limit; ++g) {
        if (c.supply(g, k) >= demand) return g;
    }
    return -1;
}

// 整簇升频的占用门槛（核当量）：ceil(n/2)。
// 未达门槛 → 占用还稀，升频会让其余空闲核白吃漏电，正确动作是把线程迁走/拉别的核进来；
// 达门槛 → 迁移已无处可去，此时抬整簇频率才是唯一有效动作。
// 6 核 big 簇 = 3.0（单线程最多 1.0，永不单独触发）；2 核 prime 簇 = 1.0（单线程跑满即触发）。
inline float busyGate(int clusterCores) {
    const int n = clusterCores < 1 ? 1 : clusterCores;
    return (float)((n + 1) / 2);
}

}  // namespace kin
