// Kin 负载闭环调频决策层（P0-2 / P0-3 的算法内核）
//
// 背景: Kin 的智能档原来是纯开环 —— AutoFsm 每 2s 选一个状态, 状态映射到一张静态频率界表,
// 状态不变则 scaling_max 恒定。也就是说 2 秒内频率上限是个常数, 对负载没有任何反馈。
//
// 本文件把 ChiRi(千漓调度) 的 CLG 决策函数抽成纯函数: 输入本簇负载, 输出本 tick 的期望
// 频率上限(性能比 0..1)。**不含任何 IO** —— 读 sysfs、写 scaling_max、频率档位 floor 对齐
// 全部留在 scheduler 侧, 因此本文件可在 Windows 主机上单测。
//
// 与 ChiRi 的对应关系(源码 src/chiri/cpu_load_governor.rs::on_load_update):
//   1) 决策负载 EMA 平滑         util_smoothing
//   2) 尖峰抑制                   spike_jump_threshold / spike_decay
//   3) headroom 过渡带            headroom_ramp / headroom_factor
//   4) 目标 = util × headroom     clamp 到 [perf_floor, perf_ceil]
//   5) 双向死区 gate              write_deadzone
//   6) 升/降频确认 tick 数        up/down_rate_limit_ticks
//   7) 极低负载立即降频            down_fast_threshold
//   8) 升频速率(smoothing_up)     高位/显著跳变走快路, 滞回带内线性加速
//
// 未包含(分别排期): 高位升频门槛 high_zone、稳态下探 T11、帕累托前沿 EEM、触摸升频、热钳制。
//
// 三条来自 ChiRi 硬教训的护栏, 改动本文件时勿回退:
//   [1] 死区 gate 必须**双向对称**且放在升/降分支**之前**。只有降频侧有 gate 时, 任意
//       +1e-6 的目标抬升都会走 up 分支并清零 down_wait, 于是"本 tick 根本写不出一次频差"
//       的单 tick 噪声也在翻转方向(ChiRi 实测 big 簇 52~86 次/分)。
//   [2] 浮点停滞防护: 死区 gate 用 <= 比较已覆盖 α≤0.5 慢升时的 1ulp 舍入停滞
//       (ChiRi 早期靠 +1e-6 epsilon, 引入死区后语义等价)。
//   [3] perf_ceil 默认恒 1.0, 不许当省电旋钮。省电靠升频门槛/审批/平滑, 不靠砍上限
//       (ChiRi 能效策略 R2)。要省电请调 up_threshold / smoothing_up。
//
// 死区口径说明: ChiRi 在 kHz 空间比 (find_floor_freq(target) vs current_freq);
// 本层在性能比空间比 (|Δperf| <= write_deadzone)。两者在 freq = perf × hw_max 时等价,
// 差异仅来自频率档位量化, 而单档步长通常小于 3% 死区带, 故可接受。落盘前的精确 kHz 比较
// 由 scheduler 侧在 floor 对齐之后做。
#pragma once

namespace kin {

// ── 调参（缺省值 = ChiRi 在 SM8550 真机调过的那一组）──────────────────────────
struct LoadGovParams {
    // 升/降频门槛。两者之间的差值决定"滞回带"宽度：带内升频按比例减速。
    float upThreshold        = 0.88f;
    float downThreshold      = 0.62f;
    // 低于此负载直接降频，跳过 down_rate_limit_ticks 确认（极低负载没有"再等等"的价值）
    float downFastThreshold  = 0.30f;

    // 性能余量：util 达 upThreshold 后目标放大到此系数（>= 1，给突发留余量）
    float headroomFactor     = 1.20f;
    // headroom 在 upThreshold 附近的过渡带宽度：从 upThreshold-ramp 到 upThreshold
    // 线性由 1.0 渐变至 headroomFactor，避免阶跃导致振荡
    float headroomRamp       = 0.08f;

    // 决策负载 EMA 系数（1.0 = 关闭）。抖动负载下 max*util 每 tick 大幅摆动 →
    // 决策方向反复翻转 → scaling_max 高频改写。ChiRi 8550 QQ 实测：开启后决策反转
    // 降约五成、写频降三成。
    float utilSmoothing      = 0.50f;
    // 升频步长系数（单 tick 最多走掉剩余差距的多少）
    float smoothingUp        = 0.60f;
    // 滞回带内升频对 smoothingUp 的缩放下限（0.02 = 最低只走 2% 的差距）
    float slowUpScale        = 0.02f;
    // 单 tick 跳升超过此值算"显著跳变"，走快路（用 smoothingUp 全速）
    float upJumpThreshold    = 0.45f;
    // 单 tick 跳升超过此值算尖峰，只采纳 spikeDecay 比例的增量（抑制单核毛刺）
    float spikeJumpThreshold = 0.35f;
    float spikeDecay         = 0.10f;

    // 性能比区间
    float perfFloor          = 0.10f;
    float perfCeil           = 1.00f;   // R2: 恒 1.0，不做省电旋钮
    float perfInit           = 0.45f;   // 接管/重建时的初始上限

    // 升/降频确认 tick 数（单位 = 本层的 tick，不是毫秒）。
    // Kin 若按 400ms tick 跑：up 3 tick = 1.2s、down 2 tick = 0.8s。
    int   upRateLimitTicks   = 3;
    int   downRateLimitTicks = 2;

    // 写频死区（× 性能比，等价于 × 簇硬件最高频的占比）
    float writeDeadzone      = 0.03f;
    // 写频最小驻留（ms）。Kin 400ms tick 时取 800 = 2 tick。
    int   writeDwellMs       = 800;

    // 钳位与自愈：任何越界/NaN 回缺省，不把非法值带进决策。
    void normalize();
};

// ── 单簇运行时状态 ──────────────────────────────────────────────────────────
struct LoadGovState {
    float emaUtil     = -1.0f;   // -1 = 未初始化（首 tick 直接采纳原始值）
    float currentPerf = 0.45f;   // 当前已写入的频率上限（性能比）
    float lastUtil    = 0.0f;    // 上一 tick 的平滑负载（供尖峰判定）
    int   upWait      = 0;       // 升频确认计数
    int   downWait    = 0;       // 降频确认计数

    // 接管/重建/模式切换时复位到新的初始上限
    void reset(float initPerf) {
        emaUtil = -1.0f;
        currentPerf = initPerf;
        lastUtil = 0.0f;
        upWait = 0;
        downWait = 0;
    }
};

enum class GovDecision {
    Hold,      // 死区内：不写、不计数、不翻转方向
    UpWait,    // 升频确认中
    Up,        // 升频
    DownWait,  // 降频确认中
    Down,      // 降频
};

struct GovStep {
    GovDecision decision     = GovDecision::Hold;
    float       targetPerf   = 0.0f;   // 本 tick 的瞬时目标（未平滑）
    float       effPerf      = 0.0f;   // 应写入的上限（= 更新后的 currentPerf）
    float       smoothedUtil = 0.0f;   // 平滑后负载（供日志/离线回放）
};

// 推进一个 tick。util = 本簇内各核 busy% 的最大值，取值 0..1（越界自动钳位，NaN 归 0）。
// 返回本 tick 的决策与应写入的性能比；不修改任何全局状态。
inline GovStep step(LoadGovState& s, const LoadGovParams& p, float util) {
    GovStep out;

    // 输入防护：NaN / 负 / 越界一律钳进 [0,1]。`!(v >= 0)` 同时覆盖 NaN。
    if (!(util >= 0.0f)) util = 0.0f;
    if (util > 1.0f) util = 1.0f;

    // 1) 决策负载 EMA 平滑（utilSmoothing >= 0.999 视为关闭）
    //    firstTick: 本 tick 之前没有历史平滑值（刚 reset/刚接管）—— 尖峰抑制需要一个
    //    “上一个平滑值”才能比出跳升，没有历史就没有尖峰可言。不排除首 tick 会出现
    //    “刚接管就遇到高负载 → lastUtil=0 → 被判成巨大跳升 → 抑制到很低 → 反而降频”的
    //    反向决策（实测：perf 0.45 → 0.14 降档一拍）。
    const bool firstTick = (s.emaUtil < 0.0f);
    float smoothed;
    if (p.utilSmoothing >= 0.999f || firstTick) smoothed = util;
    else smoothed = p.utilSmoothing * util + (1.0f - p.utilSmoothing) * s.emaUtil;
    s.emaUtil = smoothed;
    out.smoothedUtil = smoothed;

    // 2) 尖峰抑制：单 tick 跳升过大时只采纳 spikeDecay 比例的增量。
    //    lastUtil 记的是**平滑后**的值（与 ChiRi 一致），不是抑制后的值。
    float u = smoothed;
    if (!firstTick && smoothed > s.lastUtil + p.spikeJumpThreshold)
        u = s.lastUtil + (smoothed - s.lastUtil) * p.spikeDecay;
    s.lastUtil = smoothed;

    // 3) headroom：upThreshold 以上给满余量，ramp 带内线性过渡，带下恒 1.0
    const float rampStart = p.upThreshold - p.headroomRamp;
    float headroom;
    if (u >= p.upThreshold) {
        headroom = p.headroomFactor;
    } else if (p.headroomRamp > 1e-6f && u > rampStart) {
        float t = (u - rampStart) / p.headroomRamp;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        headroom = 1.0f + (p.headroomFactor - 1.0f) * t;
    } else {
        headroom = 1.0f;
    }

    // 4) 瞬时目标 = util × headroom，clamp 进 [floor, ceil]
    float target = u * headroom;
    if (!(target >= p.perfFloor)) target = p.perfFloor;   // 同时吞掉 NaN
    if (target > p.perfCeil) target = p.perfCeil;
    out.targetPerf = target;

    const float oldPerf = s.currentPerf;
    const float delta = target - oldPerf;

    // 5) 死区 gate —— 护栏 [1]：双向对称，且必须放在升/降分支之前。
    //    命中即 hold：不写频、不累加 up/down 计数、不翻转方向。
    //    这样"写不进 sysfs 的 no-op 意图"不会抹掉另一个方向的确认进度。
    const float absDelta = delta < 0.0f ? -delta : delta;
    if (absDelta <= p.writeDeadzone) {
        out.decision = GovDecision::Hold;
        out.effPerf = oldPerf;
        return out;
    }

    if (delta > 0.0f) {
        // ── 升频 ──
        s.downWait = 0;                     // 真实升频意图才清降频进度
        if (++s.upWait < p.upRateLimitTicks) {
            out.decision = GovDecision::UpWait;
            out.effPerf = oldPerf;
            return out;
        }
        out.decision = GovDecision::Up;

        // 抬 ceiling 不强制频率跳变：schedutil 在 [hw-min, ceiling] 内自主取频，
        // 所以这里只需平滑抬高上限，不需要读 scaling_cur_freq 做余量检查。
        const bool highLoad = u >= p.upThreshold;
        const bool bigJump  = delta > p.upJumpThreshold;
        float speed;
        if (highLoad || bigJump) {
            speed = p.smoothingUp;
        } else {
            // 滞回带内：越接近 upThreshold 升得越快
            const float span = (p.upThreshold - p.downThreshold) > 1e-6f
                                   ? (p.upThreshold - p.downThreshold) : 1e-6f;
            float gap = (u - p.downThreshold) / span;
            if (gap < 0.0f) gap = 0.0f;
            if (gap > 1.0f) gap = 1.0f;
            speed = p.smoothingUp * (p.slowUpScale + (1.0f - p.slowUpScale) * gap);
        }
        s.currentPerf = oldPerf + delta * speed;
    } else {
        // ── 降频 ──
        s.upWait = 0;                       // 真实降频意图才清升频进度
        // 极低负载豁免确认期：util < downFastThreshold 时不等待
        const bool fastDown = u < p.downFastThreshold;
        if (!fastDown && ++s.downWait < p.downRateLimitTicks) {
            out.decision = GovDecision::DownWait;
            out.effPerf = oldPerf;
            return out;
        }
        out.decision = GovDecision::Down;
        // 一步到位降上限：降 ceiling 只收窄 schedutil 区间，不会把实际频率抬上去
        s.currentPerf = target;
    }

    // 收尾钳位（升频路径是增量叠加，可能越过边界）
    if (!(s.currentPerf >= p.perfFloor)) s.currentPerf = p.perfFloor;
    if (s.currentPerf > p.perfCeil) s.currentPerf = p.perfCeil;
    out.effPerf = s.currentPerf;
    return out;
}

// 写频最小驻留门：距上次实际落盘是否已满 writeDwellMs。
// lastWriteMs <= 0 表示从未写过 —— 首次受控写频立即生效，不等。
// 豁免路径（触摸提频、极低负载降频、接管/释放、写失败补写）由调用方绕过本函数，
// 不要在这里加"豁免"参数：ChiRi 的教训是豁免点必须少且显式。
inline bool dwellElapsed(long long nowMs, long long lastWriteMs, int dwellMs) {
    if (lastWriteMs <= 0) return true;
    if (dwellMs <= 0) return true;
    const long long age = nowMs - lastWriteMs;
    if (age < 0) return true;   // 时钟回拨按"已过"处理（安全方向：允许写）
    return age >= (long long)dwellMs;
}

inline const char* decisionName(GovDecision d) {
    switch (d) {
        case GovDecision::Hold:     return "hold";
        case GovDecision::UpWait:   return "up_wait";
        case GovDecision::Up:       return "up";
        case GovDecision::DownWait: return "down_wait";
        case GovDecision::Down:     return "down";
    }
    return "?";
}

// ── 钳位实现 ────────────────────────────────────────────────────────────────
namespace detail {
inline float clampf(float v, float lo, float hi) {
    if (!(v >= lo)) return lo;   // NaN / 过小
    if (v > hi) return hi;
    return v;
}
inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
}  // namespace detail

inline void LoadGovParams::normalize() {
    upThreshold        = detail::clampf(upThreshold, 0.01f, 1.0f);
    downThreshold      = detail::clampf(downThreshold, 0.0f, 0.99f);
    // 必须严格 up > down：否则"滞回带"宽度为 0 或负，升频减速公式失去意义
    if (downThreshold >= upThreshold) downThreshold = upThreshold - 0.01f;
    if (downThreshold < 0.0f) downThreshold = 0.0f;

    downFastThreshold  = detail::clampf(downFastThreshold, 0.0f, 0.99f);
    headroomFactor     = detail::clampf(headroomFactor, 1.0f, 2.0f);
    headroomRamp       = detail::clampf(headroomRamp, 0.0f, 0.5f);
    utilSmoothing      = detail::clampf(utilSmoothing, 0.05f, 1.0f);
    smoothingUp        = detail::clampf(smoothingUp, 0.01f, 1.0f);
    slowUpScale        = detail::clampf(slowUpScale, 0.0f, 1.0f);
    upJumpThreshold    = detail::clampf(upJumpThreshold, 0.0f, 1.0f);
    spikeJumpThreshold = detail::clampf(spikeJumpThreshold, 0.0f, 1.0f);
    spikeDecay         = detail::clampf(spikeDecay, 0.0f, 1.0f);

    perfCeil           = detail::clampf(perfCeil, 0.05f, 1.0f);
    perfFloor          = detail::clampf(perfFloor, 0.0f, perfCeil);
    perfInit           = detail::clampf(perfInit, perfFloor, perfCeil);

    upRateLimitTicks   = detail::clampi(upRateLimitTicks, 1, 1000);
    downRateLimitTicks = detail::clampi(downRateLimitTicks, 1, 1000);
    writeDeadzone      = detail::clampf(writeDeadzone, 0.0f, 0.20f);
    writeDwellMs       = detail::clampi(writeDwellMs, 0, 5000);
}

}  // namespace kin
