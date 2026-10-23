// 负载闭环调频决策层的单测（P0-2 / P0-3 的算法内核）。
//
// 断言重点不是"能跑"，而是三条来自 ChiRi 实测的护栏：
//   1) 死区 gate 双向对称 —— no-op 的升/降意图不得抹掉另一个方向的确认进度
//      （ChiRi 实测 bug：只有降频侧有 gate 时 big 簇方向翻转 52~86 次/分）
//   2) 死区内的目标变化必须是 Hold，不写频、不计数
//   3) perf_ceil 不得被当成省电旋钮（R2：省电靠门槛，不靠砍上限）
#include "LoadGovernor.hpp"
#include <cmath>
#include <cstdio>

namespace {

int g_fail = 0;

bool approx(float got, float want, float eps, const char* what) {
    const float d = got > want ? got - want : want - got;
    if (d <= eps) return true;
    std::printf("FAIL %s: got %.6f want %.6f (eps %.6f)\n", what, got, want, eps);
    g_fail = 1;
    return false;
}

bool eqi(int got, int want, const char* what) {
    if (got == want) return true;
    std::printf("FAIL %s: got %d want %d\n", what, got, want);
    g_fail = 1;
    return false;
}

bool decisionIs(kin::GovDecision got, kin::GovDecision want, const char* what) {
    if (got == want) return true;
    std::printf("FAIL %s: got %s want %s\n", what, kin::decisionName(got), kin::decisionName(want));
    g_fail = 1;
    return false;
}

// 测试用参数：关掉 EMA 与尖峰抑制，让 util 直达决策，便于逐 tick 断言。
kin::LoadGovParams plainParams() {
    kin::LoadGovParams p;
    p.utilSmoothing      = 1.0f;    // 关 EMA
    p.spikeJumpThreshold = 1.0f;    // 关尖峰抑制
    p.normalize();
    return p;
}

// ── 1. 默认值必须是 ChiRi 8550 实测调过的那一组（写死字面值，不引用 p.*）──
void testDefaults() {
    const kin::LoadGovParams p;
    approx(p.upThreshold,        0.88f, 1e-6f, "default upThreshold");
    approx(p.downThreshold,      0.62f, 1e-6f, "default downThreshold");
    approx(p.downFastThreshold,  0.30f, 1e-6f, "default downFastThreshold");
    approx(p.headroomFactor,     1.20f, 1e-6f, "default headroomFactor");
    approx(p.headroomRamp,       0.08f, 1e-6f, "default headroomRamp");
    approx(p.utilSmoothing,      0.50f, 1e-6f, "default utilSmoothing");
    approx(p.smoothingUp,        0.60f, 1e-6f, "default smoothingUp");
    approx(p.spikeDecay,         0.10f, 1e-6f, "default spikeDecay");
    approx(p.upJumpThreshold,    0.45f, 1e-6f, "default upJumpThreshold");
    approx(p.writeDeadzone,      0.03f, 1e-6f, "default writeDeadzone");
    eqi(p.upRateLimitTicks,   3, "default upRateLimitTicks");
    eqi(p.downRateLimitTicks, 2, "default downRateLimitTicks");
    // Kin 缺省关闭驻留：确认窗口(up 3/down 2 tick @600ms)已提供 1.8s/1.2s 驻留
    eqi(p.writeDwellMs,       0, "default writeDwellMs (disabled, see header)");

    // R2：天花板恒 1.0，不许当省电旋钮
    approx(p.perfCeil, 1.0f, 1e-6f, "default perfCeil must stay 1.0 (R2)");
}

// ── 2. 死区 gate 双向对称（护栏 1：本文件最重要的一条）────────────────────
void testDeadzoneSymmetry() {
    kin::LoadGovParams p = plainParams();
    p.writeDeadzone = 0.03f;

    // (a) no-op 的**升**意图不得清掉降频确认进度
    {
        kin::LoadGovState s;
        s.currentPerf = 0.50f;
        s.downWait    = 1;      // 之前已积累过一次真实降频意图
        s.lastUtil    = 0.52f;
        const kin::GovStep r = kin::step(s, p, 0.52f);   // target 0.52，Δ=+0.02 落在死区内
        decisionIs(r.decision, kin::GovDecision::Hold, "no-op up -> hold");
        eqi(s.downWait, 1, "no-op up must NOT clear downWait");
        eqi(s.upWait,   0, "no-op up must not accumulate upWait");
        approx(r.effPerf, 0.50f, 1e-6f, "no-op up keeps currentPerf");
    }

    // (b) no-op 的**降**意图不得清掉升频确认进度（对称的另一半）
    {
        kin::LoadGovState s;
        s.currentPerf = 0.50f;
        s.upWait      = 2;
        s.lastUtil    = 0.48f;
        const kin::GovStep r = kin::step(s, p, 0.48f);   // target 0.48，Δ=-0.02
        decisionIs(r.decision, kin::GovDecision::Hold, "no-op down -> hold");
        eqi(s.upWait,   2, "no-op down must NOT clear upWait");
        eqi(s.downWait, 0, "no-op down must not accumulate downWait");
    }

    // (c) 真实方向才翻转：Δ 超过死区时, 另一侧计数必须清零
    {
        kin::LoadGovParams q = plainParams();
        q.upRateLimitTicks = 5;
        kin::LoadGovState s;
        s.currentPerf = 0.50f;
        s.downWait    = 1;
        s.lastUtil    = 0.90f;
        const kin::GovStep r = kin::step(s, q, 0.90f);   // Δ=+0.40，远超死区
        decisionIs(r.decision, kin::GovDecision::UpWait, "real up -> up_wait");
        eqi(s.downWait, 0, "real up clears downWait");
        eqi(s.upWait,   1, "real up accumulates upWait");
    }
}

// ── 3. 升频必须攒满 upRateLimitTicks ──────────────────────────────────────
void testUpRateLimit() {
    kin::LoadGovParams p = plainParams();
    p.upRateLimitTicks = 3;
    kin::LoadGovState s;
    s.currentPerf = 0.30f;
    s.lastUtil    = 0.95f;

    decisionIs(kin::step(s, p, 0.95f).decision, kin::GovDecision::UpWait,   "up tick 1/3");
    decisionIs(kin::step(s, p, 0.95f).decision, kin::GovDecision::UpWait,   "up tick 2/3");
    const kin::GovStep r = kin::step(s, p, 0.95f);
    decisionIs(r.decision, kin::GovDecision::Up, "up tick 3/3");
    // util 0.95 × headroom 1.20 = 1.14 → 钳到 1.0；升频是平滑逼近，不是一步到位
    if (!(r.effPerf > 0.30f)) { std::printf("FAIL up did not raise perf\n"); g_fail = 1; }
    if (r.effPerf > 1.0f + 1e-6f) { std::printf("FAIL up exceeded ceil\n"); g_fail = 1; }
    approx(r.targetPerf, 1.0f, 1e-6f, "target clamped by perfCeil");
}

// ── 4. 降频必须攒满 downRateLimitTicks（且非极低负载）────────────────────
void testDownRateLimit() {
    kin::LoadGovParams p = plainParams();
    p.downRateLimitTicks = 2;
    p.downFastThreshold  = 0.30f;
    kin::LoadGovState s;
    s.currentPerf = 0.90f;
    s.lastUtil    = 0.40f;

    decisionIs(kin::step(s, p, 0.40f).decision, kin::GovDecision::DownWait, "down tick 1/2");
    const kin::GovStep r = kin::step(s, p, 0.40f);
    decisionIs(r.decision, kin::GovDecision::Down, "down tick 2/2");
    // 降频一步到位写目标档
    approx(r.effPerf, 0.40f, 1e-6f, "down jumps straight to target");
}

// ── 5. 极低负载立即降频，跳过确认期 ──────────────────────────────────────
void testFastDown() {
    kin::LoadGovParams p = plainParams();
    p.downRateLimitTicks = 5;      // 刻意放大确认期
    p.downFastThreshold  = 0.30f;
    kin::LoadGovState s;
    s.currentPerf = 0.90f;
    s.lastUtil    = 0.10f;

    const kin::GovStep r = kin::step(s, p, 0.10f);
    decisionIs(r.decision, kin::GovDecision::Down, "fast down skips confirmation");
    approx(r.effPerf, 0.10f, 1e-6f, "fast down reaches floor target");

    // 边界：刚好不到 downFastThreshold 才豁免；等于阈值不豁免
    kin::LoadGovState s2;
    s2.currentPerf = 0.90f;
    s2.lastUtil    = 0.30f;
    decisionIs(kin::step(s2, p, 0.30f).decision, kin::GovDecision::DownWait,
               "util == downFastThreshold is NOT fast down");
}

// ── 6. 稳态必须收敛成 Hold（否则就是 ChiRi 说的"假 down 循环"）──────────
void testSteadyStateHolds() {
    kin::LoadGovParams p = plainParams();
    kin::LoadGovState s;
    s.reset(0.50f);
    s.lastUtil = 0.50f;

    // 负载恒定 0.50：第 1 tick 因 Δ=0 直接 Hold，之后一直 Hold
    for (int i = 0; i < 20; ++i) {
        const kin::GovStep r = kin::step(s, p, 0.50f);
        if (!decisionIs(r.decision, kin::GovDecision::Hold, "steady state stays hold")) return;
        if (!approx(r.effPerf, 0.50f, 1e-6f, "steady state keeps perf")) return;
    }
    eqi(s.upWait,   0, "steady state no upWait accumulation");
    eqi(s.downWait, 0, "steady state no downWait accumulation");
}

// ── 7. EMA 平滑：开启后瞬变被滤掉，关闭时直达 ────────────────────────────
void testEmaSmoothing() {
    // 关闭：smoothed == util
    {
        kin::LoadGovParams p = plainParams();
        kin::LoadGovState s;
        s.emaUtil  = 0.0f;
        s.lastUtil = 0.0f;
        const kin::GovStep r = kin::step(s, p, 1.0f);
        approx(r.smoothedUtil, 1.0f, 1e-6f, "EMA off -> smoothed == util");
    }
    // 开启 0.5：smoothed = 0.5*1.0 + 0.5*0.0 = 0.5
    {
        kin::LoadGovParams p = plainParams();
        p.utilSmoothing = 0.50f;
        kin::LoadGovState s;
        s.emaUtil  = 0.0f;
        s.lastUtil = 0.0f;
        const kin::GovStep r = kin::step(s, p, 1.0f);
        approx(r.smoothedUtil, 0.50f, 1e-6f, "EMA 0.5 -> half");
    }
    // 首 tick（未初始化）直接采纳原始值，不被 -1 拖低
    {
        kin::LoadGovParams p = plainParams();
        p.utilSmoothing = 0.50f;
        kin::LoadGovState s;   // emaUtil = -1
        const kin::GovStep r = kin::step(s, p, 0.80f);
        approx(r.smoothedUtil, 0.80f, 1e-6f, "EMA first tick adopts raw");
    }
}

// ── 8. 尖峰抑制：单 tick 跳升只采纳 spikeDecay 比例 ──────────────────────
void testSpikeSuppression() {
    kin::LoadGovParams p;
    p.utilSmoothing      = 1.0f;    // 关 EMA，隔离尖峰逻辑
    p.spikeJumpThreshold = 0.35f;
    p.spikeDecay         = 0.10f;
    p.normalize();

    // emaUtil 必须已初始化（>=0），否则算“首 tick”不做抑制，见 testFirstTickNoSuppression
    kin::LoadGovState s;
    s.emaUtil  = 0.20f;
    s.lastUtil = 0.20f;
    // 跳升 0.70 > 阈值 0.35 → 只采纳 0.20 + 0.70*0.10 = 0.27
    const kin::GovStep r = kin::step(s, p, 0.90f);
    approx(r.targetPerf, 0.27f, 1e-5f, "spike suppressed to decayed value");

    // 未超阈值的正常爬升不被抑制
    kin::LoadGovState s2;
    s2.emaUtil  = 0.50f;
    s2.lastUtil = 0.50f;
    const kin::GovStep r2 = kin::step(s2, p, 0.70f);   // 跳升 0.20 < 0.35
    approx(r2.targetPerf, 0.70f, 1e-5f, "normal rise not suppressed");
}

// ── 8b. 首 tick 不做尖峰抑制（否则“刚接管就遇高负载”会被判成降频）──────
void testFirstTickNoSuppression() {
    kin::LoadGovParams p;
    p.utilSmoothing      = 1.0f;
    p.spikeJumpThreshold = 0.35f;
    p.spikeDecay         = 0.10f;
    p.normalize();

    // 刚接管：无历史（emaUtil = -1、lastUtil = 0），首个采样就是 0.95。
    // 若在这里做尖峰抑制：u = 0 + 0.95*0.10 = 0.095，target 降到 perfFloor，
    // 而 currentPerf 还是 perfInit(0.45) → 得出“降频”的反向决策（实测就是这条）。
    kin::LoadGovState s;
    s.reset(p.perfInit);   // currentPerf = 0.45, emaUtil = -1, lastUtil = 0
    const kin::GovStep r = kin::step(s, p, 0.95f);
    if (r.decision == kin::GovDecision::Down || r.decision == kin::GovDecision::DownWait) {
        std::printf("FAIL first tick with high load must not go down (got %s)\n",
                    kin::decisionName(r.decision));
        g_fail = 1;
        return;
    }
    if (!(r.targetPerf > p.perfInit)) {
        std::printf("FAIL first tick target %.6f should exceed perfInit %.6f\n",
                    r.targetPerf, p.perfInit);
        g_fail = 1;
    }
}

// ── 9. 目标随负载单调不减（不出现"越忙越降"）─────────────────────────────
void testMonotonicTarget() {
    kin::LoadGovParams p = plainParams();
    float prev = -1.0f;
    for (int i = 0; i <= 100; ++i) {
        const float u = (float)i / 100.0f;
        kin::LoadGovState s;
        s.lastUtil = u;
        const kin::GovStep r = kin::step(s, p, u);
        if (r.targetPerf + 1e-6f < prev) {
            std::printf("FAIL target not monotonic at util=%.2f: %.6f < %.6f\n",
                        u, r.targetPerf, prev);
            g_fail = 1;
            return;
        }
        prev = r.targetPerf;
    }
}

// ── 10. 输入域遍历：任意输入都不得产生 NaN，且 effPerf 恒在 [floor, ceil] ──
void testInputDomain() {
    kin::LoadGovParams p;
    p.normalize();
    kin::LoadGovState s;
    s.reset(p.perfInit);

    const float inputs[] = {-1.0f, 0.0f, 0.01f, 0.5f, 0.99f, 1.0f, 2.0f, 1e9f, -1e9f};
    for (float u : inputs) {
        for (int i = 0; i < 30; ++i) {
            const kin::GovStep r = kin::step(s, p, u);
            if (!(r.effPerf >= p.perfFloor - 1e-6f && r.effPerf <= p.perfCeil + 1e-6f)) {
                std::printf("FAIL effPerf out of range: util=%.3f -> %.6f\n", u, r.effPerf);
                g_fail = 1;
                return;
            }
            if (!(r.targetPerf >= p.perfFloor - 1e-6f && r.targetPerf <= p.perfCeil + 1e-6f)) {
                std::printf("FAIL targetPerf out of range: util=%.3f -> %.6f\n", u, r.targetPerf);
                g_fail = 1;
                return;
            }
            if (!(r.smoothedUtil >= 0.0f && r.smoothedUtil <= 1.0f)) {
                std::printf("FAIL smoothedUtil out of range: %.6f\n", r.smoothedUtil);
                g_fail = 1;
                return;
            }
        }
    }
}

// ── 11. 滞回带内升频必须比高位升频慢（否则"慢升"设计失效）───────────────
void testSlowUpInHysteresisBand() {
    kin::LoadGovParams p = plainParams();
    p.upRateLimitTicks = 1;
    p.smoothingUp      = 0.60f;
    p.slowUpScale      = 0.02f;
    p.upThreshold      = 0.88f;
    p.downThreshold    = 0.62f;

    // 高位（util >= upThreshold）：走全速 smoothingUp
    kin::LoadGovState hi;
    hi.currentPerf = 0.40f;
    hi.lastUtil    = 0.90f;
    const float hiPerf = kin::step(hi, p, 0.90f).effPerf;

    // 滞回带内（util 略高于 downThreshold）：走 slowUpScale 缩放的慢速
    kin::LoadGovState lo;
    lo.currentPerf = 0.40f;
    lo.lastUtil    = 0.65f;
    const float loPerf = kin::step(lo, p, 0.65f).effPerf;

    if (!(hiPerf > loPerf)) {
        std::printf("FAIL hysteresis-band up not slower than high-load up: hi=%.6f lo=%.6f\n",
                    hiPerf, loPerf);
        g_fail = 1;
    }
}

// ── 12. normalize：非法/越界/NaN 一律回安全值，且保证 up > down ──────────
void testNormalize() {
    {
        kin::LoadGovParams p;
        p.upThreshold   = 0.30f;
        p.downThreshold = 0.90f;    // 反了
        p.normalize();
        if (!(p.downThreshold < p.upThreshold)) {
            std::printf("FAIL normalize did not enforce down < up\n");
            g_fail = 1;
        }
    }
    {
        kin::LoadGovParams p;
        p.perfFloor = 0.90f;
        p.perfCeil  = 0.20f;        // 反了
        p.normalize();
        if (p.perfFloor > p.perfCeil) {
            std::printf("FAIL normalize did not clamp floor <= ceil\n");
            g_fail = 1;
        }
        p.perfInit = 0.99f;
        p.normalize();
        if (p.perfInit > p.perfCeil) {
            std::printf("FAIL normalize did not clamp init <= ceil\n");
            g_fail = 1;
        }
    }
    {
        // NaN 输入：clampf 用 !(v>=lo) 判，NaN 应落到下界而不是穿透
        kin::LoadGovParams p;
        const float nan = std::nanf("");
        p.upThreshold = nan;
        p.writeDeadzone = nan;
        p.normalize();
        if (!(p.upThreshold >= 0.01f && p.upThreshold <= 1.0f)) {
            std::printf("FAIL normalize left upThreshold invalid: %.6f\n", p.upThreshold);
            g_fail = 1;
        }
        if (!(p.writeDeadzone >= 0.0f && p.writeDeadzone <= 0.20f)) {
            std::printf("FAIL normalize left writeDeadzone invalid\n");
            g_fail = 1;
        }
    }
    {
        // 速率限制下界：0 会让确认期永不成立（等价于永不升/降频）
        kin::LoadGovParams p;
        p.upRateLimitTicks   = 0;
        p.downRateLimitTicks = -5;
        p.writeDwellMs       = -1;
        p.normalize();
        eqi(p.upRateLimitTicks,   1, "normalize upRateLimitTicks >= 1");
        eqi(p.downRateLimitTicks, 1, "normalize downRateLimitTicks >= 1");
        eqi(p.writeDwellMs,       0, "normalize writeDwellMs >= 0");
    }
}

// ── 13. 写频驻留门 ────────────────────────────────────────────────────────
void testDwell() {
    if (!kin::dwellElapsed(1000, 0, 800))    { std::printf("FAIL first write must be immediate\n"); g_fail = 1; }
    if (kin::dwellElapsed(1000, 500, 800))   { std::printf("FAIL dwell 500ms must block\n"); g_fail = 1; }
    if (!kin::dwellElapsed(1300, 500, 800))  { std::printf("FAIL dwell 800ms must pass\n"); g_fail = 1; }
    if (!kin::dwellElapsed(100, 500, 800))   { std::printf("FAIL clock rollback must allow\n"); g_fail = 1; }
    if (!kin::dwellElapsed(1000, 500, 0))    { std::printf("FAIL dwell=0 must be disabled\n"); g_fail = 1; }
}

// ── 14. 闭环响应性：负载跳升后必须在 upRateLimitTicks 个 tick 内抬上限 ──
// 这条是 P0-2 的立论依据：FSM 要 2s 才可能换状态，闭环 3 tick × 400ms = 1.2s。
void testResponsiveness() {
    kin::LoadGovParams p;
    p.normalize();
    p.upRateLimitTicks = 3;

    kin::LoadGovState s;
    s.reset(p.perfInit);
    s.lastUtil = 0.05f;

    int upTick = -1;
    for (int i = 1; i <= 10; ++i) {
        const kin::GovStep r = kin::step(s, p, 0.95f);
        if (r.decision == kin::GovDecision::Up) { upTick = i; break; }
    }
    if (upTick < 0) {
        std::printf("FAIL closed loop never raised perf under sustained high load\n");
        g_fail = 1;
        return;
    }
    if (upTick > p.upRateLimitTicks) {
        std::printf("FAIL up took %d ticks, expected <= %d\n", upTick, p.upRateLimitTicks);
        g_fail = 1;
    }
}

}  // namespace

int main() {
    testDefaults();
    testDeadzoneSymmetry();
    testUpRateLimit();
    testDownRateLimit();
    testFastDown();
    testSteadyStateHolds();
    testEmaSmoothing();
    testSpikeSuppression();
    testFirstTickNoSuppression();
    testMonotonicTarget();
    testInputDomain();
    testSlowUpInHysteresisBand();
    testNormalize();
    testDwell();
    testResponsiveness();

    if (g_fail) {
        std::puts("FAIL load governor: see above");
        return 1;
    }
    std::puts("PASS load governor: defaults, deadzone symmetry, rate limits, fast-down, "
              "steady hold, EMA, spike, first-tick, monotonic, domain, slow-up, normalize, "
              "dwell, responsiveness");
    return 0;
}
