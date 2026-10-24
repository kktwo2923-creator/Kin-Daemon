// FAS 帧级分析层（P1-1）：逐帧解析 + 加载态识别 + 档位驻留
//
// 背景：Kin 的 FAS 是**纯 P 控制器**，输入是 measured_fps（~1s 窗口均值）。
// 窗口均值能回答"这一秒平均多少帧"，但回答不了三件对省电/手感都关键的事：
//   1) 这一秒里有没有**单帧**超长（一次 300ms 卡顿被 60 帧摊薄成 5ms 的均值变化）
//   2) 现在是**读条/过图**还是真在跑游戏（前者帧率天然低，不该拉满频）
//   3) 读条结束瞬间的**频率断崖**（刚把上限压到 0.6，场景切回来第一秒必然掉帧）
//
// ChiRi 的答案是把帧逐帧投喂进状态机（见 agentsdocs/04-hard-lessons.md「FAS 帧投喂必须全量」）：
// 只投喂窗口末条 delta 会把 100ms 内 6 帧丢掉 5 帧，所有"帧数"常数的时间尺度被拉长 ~6 倍，
// PID/防抖/jank 全面钝化。本文件按同一口径设计：**能拿到逐帧就逐帧喂，拿不到才退化成窗口喂**。
//
// 逐帧数据来源（免费）：`dumpsys SurfaceFlinger --latency <layer>` 一次返回最多 128 行历史帧，
// Kin 的 fpsFromLatencyBlock 本来就已经把整块读进来了，只是压成了一个 fps。本文件把它拆回逐帧。
//
// 零 IO：解析器输入 const char*，状态机输入纯值，全部可在 Windows 主机单测。
#pragma once

#include <cstdint>
#include <cstdlib>

namespace kin {

// ── 逐帧批次（来自 --latency 的 B 列 actualPresent）────────────────────────────
struct FrameBatch {
    static constexpr int kMax = 128;
    float deltaMs[kMax] = {};   // 相邻帧间隔（ms），严格 > 0
    int   n       = 0;
    float meanMs  = 0.0f;
    float p95Ms   = 0.0f;
    float maxMs   = 0.0f;
    int   heavyCount = 0;       // > heavyThresholdMs 的帧数
    float heavyMs    = 0.0f;    // 这些重帧的累计时长（判加载态用）
};

// 解析 `dumpsys SurfaceFlinger --latency` 输出。
// 行 1 = 刷新周期；后续每行 3 列 ns（A draw / B actualPresent / C set）。
// 取 B 列，丢掉 0 与 INT64_MAX 哨兵，相邻差即帧间隔。
// 返回 false = 数据不可用（有效帧 < 2 / 时间戳非单调），调用方应回退窗口信号。
inline bool parseLatencyFrames(const char* text, FrameBatch& out, float heavyThresholdMs) {
    out.n = 0; out.meanMs = 0; out.p95Ms = 0; out.maxMs = 0;
    out.heavyCount = 0; out.heavyMs = 0;
    if (!text) return false;

    const char* p = text;
    while (*p && *p != '\n') ++p;          // 跳过首行（刷新周期）
    if (*p == '\n') ++p;

    long long prev = 0;
    int n = 0;
    while (*p) {
        char* q = nullptr;
        (void)strtoll(p, &q, 10);          // A 列
        if (q == p) { while (*p && *p != '\n') ++p; if (*p) ++p; continue; }
        const long long bcol = strtoll(q, &q, 10);   // B 列 actualPresent
        (void)strtoll(q, &q, 10);                    // C 列
        if (bcol > 0 && bcol != INT64_MAX) {
            if (prev > 0 && bcol > prev && n < FrameBatch::kMax) {
                const float ms = (float)(bcol - prev) / 1e6f;
                out.deltaMs[n++] = ms;
            }
            prev = bcol;
        }
        while (*p && *p != '\n') ++p; if (*p) ++p;
    }
    if (n < 1) return false;               // 只有 1 帧算不出间隔
    out.n = n;

    // 统计量：均值 / max / 重帧累计
    float sum = 0.0f, mx = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float d = out.deltaMs[i];
        sum += d;
        if (d > mx) mx = d;
        if (d > heavyThresholdMs) { out.heavyCount++; out.heavyMs += d; }
    }
    out.meanMs = sum / (float)n;
    out.maxMs  = mx;

    // p95：128 帧做插入排序足够快，且不需要额外缓冲/动态分配（FAS 在热路径上）
    float tmp[FrameBatch::kMax];
    for (int i = 0; i < n; ++i) tmp[i] = out.deltaMs[i];
    for (int i = 1; i < n; ++i) {
        const float v = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; --j; }
        tmp[j + 1] = v;
    }
    int idx = (int)(0.95f * (float)(n - 1) + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    out.p95Ms = tmp[idx];
    return true;
}

// ── 加载态（读条/过图）──────────────────────────────────────────────────────
// ChiRi 口径：单帧 > heavyFrameMs(150ms) 记重帧，累计 > cumulativeMs(2500ms) 进加载态；
// 加载态把 perf 钳在 [perfFloor, perfCeiling]（压着加载即可，别浪费）；
// 加载结束后 postIgnoreFrames 帧观察期 + postGuardFrames 帧内禁止降档（防频率断崖）。
struct LoadingParams {
    float heavyFrameMs     = 150.0f;
    float cumulativeMs     = 2500.0f;
    int   normalTolerance  = 3;       // 加载态中连续正常帧达到此数才退出
    float perfFloor        = 0.60f;
    float perfCeiling      = 0.70f;
    int   postIgnoreFrames = 5;
    float postPerf         = 0.65f;
    int   postGuardFrames  = 90;
};

struct LoadingState {
    bool  loading        = false;
    float heavyAccumMs   = 0.0f;   // 重帧累计（加载态判定）
    int   normalStreak   = 0;      // 加载态内的连续正常帧数
    int   postGuard      = 0;      // >0 = 加载后保护期剩余帧数
};

// 逐帧喂入。调用方应把**整批**帧逐条喂进来（ChiRi 硬教训：不能只喂末条）。
// heavyThresholdMs 用 p.heavyFrameMs，保持一致。
inline void feedLoadingFrame(LoadingState& s, float frameMs, const LoadingParams& p) {
    if (frameMs <= 0.0f) return;

    if (s.loading) {
        if (frameMs > p.heavyFrameMs) {
            s.normalStreak = 0;
            s.heavyAccumMs += frameMs;
        } else if (++s.normalStreak >= p.normalTolerance) {
            // 退出加载态 → 进入加载后保护期
            s.loading = false;
            s.heavyAccumMs = 0.0f;
            s.normalStreak = 0;
            s.postGuard = p.postGuardFrames;
        }
        return;
    }

    if (s.postGuard > 0) --s.postGuard;

    if (frameMs > p.heavyFrameMs) {
        s.heavyAccumMs += frameMs;
        if (s.heavyAccumMs >= p.cumulativeMs) {
            s.loading = true;
            s.normalStreak = 0;
            s.postGuard = 0;
        }
    } else {
        // 正常帧缓慢衰减重帧累计：否则零星长帧会无限累加，跑几十分钟必然误判成加载态。
        s.heavyAccumMs -= frameMs * 0.5f;
        if (s.heavyAccumMs < 0.0f) s.heavyAccumMs = 0.0f;
    }
}

// 窗口兜底：只有 measured_fps 时用（~1s 一个采样点）。
// 判据不同：逐帧看"单帧超长"，窗口只能看"整段帧率远低于目标"。
// loadingFpsRatio 缺省 0.4 —— 目标 60fps 时低于 24fps 且 GPU 不忙，大概率在读条/过图。
inline void feedLoadingWindow(LoadingState& s, float fps, int targetFps, int gpuPct,
                              float dtMs, const LoadingParams& p,
                              float loadingFpsRatio = 0.40f, int gpuIdlePct = 40) {
    if (targetFps <= 0 || fps <= 0.0f || dtMs <= 0.0f) return;
    const bool slow  = fps < (float)targetFps * loadingFpsRatio;
    const bool gpuIdle = (gpuPct < 0) || (gpuPct < gpuIdlePct);

    if (s.loading) {
        // 窗口口径下"正常"= 帧率回到目标的 80% 以上
        if (fps >= (float)targetFps * 0.80f) {
            if (++s.normalStreak >= p.normalTolerance) {
                s.loading = false;
                s.heavyAccumMs = 0.0f;
                s.normalStreak = 0;
                s.postGuard = p.postGuardFrames;
            }
        } else {
            s.normalStreak = 0;
        }
        return;
    }

    if (s.postGuard > 0) --s.postGuard;

    if (slow && gpuIdle) {
        s.heavyAccumMs += dtMs;
        if (s.heavyAccumMs >= p.cumulativeMs) {
            s.loading = true;
            s.normalStreak = 0;
            s.postGuard = 0;
        }
    } else {
        s.heavyAccumMs -= dtMs * 0.5f;
        if (s.heavyAccumMs < 0.0f) s.heavyAccumMs = 0.0f;
    }
}

// 加载态 / 加载后保护期给出的 perf 覆盖值。返回 0 = 不覆盖（走正常闭环）。
inline float loadingPerfOverride(const LoadingState& s, const LoadingParams& p) {
    if (s.loading) {
        // 钳在 [floor, ceiling]：加载只需要"压着跑"，给满频纯烧电
        return p.perfCeiling > 0.0f ? p.perfCeiling : 0.0f;
    }
    if (s.postGuard > 0) return p.postPerf;
    return 0.0f;
}

// 加载后是否禁止降档（防读条结束瞬间的频率断崖）
inline bool loadingBlocksDowngrade(const LoadingState& s) {
    return s.loading || s.postGuard > 0;
}

// ── 档位驻留（快升慢降）────────────────────────────────────────────────────
// ChiRi 口径：升档后最短驻留 DOWNGRADE_MIN_DWELL_FRAMES(120 帧 ≈ 60fps 下 2s)才允许降档，
// 让新档位的 PID/阻尼先收敛、短谷被吸收；降档退避计数按 60s 时间衰减，
// 否则一次抖动期的 16 倍冷却会咬住后续孤立降档。
struct GearParams {
    int  minDwellFrames          = 120;
    long downgradeBackoffResetMs = 60000;
    float minUpgradePerfCeil     = 0.62f;   // 升档瞬间的落点上限（稳态由闭环收敛）
};

struct GearState {
    int  framesSinceUpgrade = 0;
    int  downgradeBackoff   = 0;
    long lastDowngradeMs    = 0;
};

// 每帧推进（与 feedLoadingFrame 同步调用即可）
inline void gearTick(GearState& g, const GearParams& p, long nowMs) {
    if (g.framesSinceUpgrade < p.minDwellFrames) ++g.framesSinceUpgrade;
    // 退避的时间衰减：距上次降档超过窗口即视为新一轮，计数归 1（有降档才算）
    if (g.downgradeBackoff > 1 && g.lastDowngradeMs > 0 &&
        nowMs - g.lastDowngradeMs >= p.downgradeBackoffResetMs) {
        g.downgradeBackoff = 1;
    }
}

// 按 N 帧批量推进。**窗口路径（measured_fps）必须用这个**：
// 该路径每拍只拿到一个 ~1s 窗口均值、没有逐帧事件，若只调 gearTick(1 帧/拍)，
// 120 帧驻留在 600ms 拍下要 72 秒才走完，等价于降档被永久禁止。
// 用 fps × dt 折算窗口内的真实帧数推进，才能保持"120 帧 ≈ 2s @60fps"的原意。
inline void gearTickFrames(GearState& g, const GearParams& p, long nowMs, int frames) {
    if (frames < 1) frames = 1;
    g.framesSinceUpgrade += frames;
    if (g.framesSinceUpgrade > p.minDwellFrames) g.framesSinceUpgrade = p.minDwellFrames;
    if (g.downgradeBackoff > 1 && g.lastDowngradeMs > 0 &&
        nowMs - g.lastDowngradeMs >= p.downgradeBackoffResetMs) {
        g.downgradeBackoff = 1;
    }
}

inline void gearNoteUpgrade(GearState& g) {
    g.framesSinceUpgrade = 0;
}

inline void gearNoteDowngrade(GearState& g, long nowMs) {
    g.lastDowngradeMs = nowMs;
    if (g.downgradeBackoff < 4) ++g.downgradeBackoff;   // 上限 4（ChiRi 的 min(4)）
}

// 是否允许降档：升档后未住满 minDwellFrames 一律不允许。
inline bool gearAllowsDowngrade(const GearState& g, const GearParams& p) {
    return g.framesSinceUpgrade >= p.minDwellFrames;
}

// 升档瞬间的 perf 落点：不超过 minUpgradePerfCeil（从低处起步，稳态由闭环收敛，
// 突发卡顿另有 jank 紧急跳频抬回）。targetPerf<=0 时返回 ceil 本身。
inline float gearUpgradePerf(float targetPerf, const GearParams& p) {
    if (targetPerf <= 0.0f) return p.minUpgradePerfCeil;
    return targetPerf > p.minUpgradePerfCeil ? p.minUpgradePerfCeil : targetPerf;
}

}  // namespace kin
