// FAS 帧级分析层单测（P1-1）：逐帧解析 / 加载态识别 / 档位驻留。
//
// 断言重点：
//   1) 解析器必须能正确处理哨兵（0 / INT64_MAX）、非单调时间戳、首行刷新周期
//   2) 加载态必须能进能出，且**不会因为零星长帧无限累加而误判**（衰减项）
//   3) 加载后保护期必须真的挡住降档（读条结束瞬间的频率断崖）
//   4) 档位驻留：升档后未住满帧数不许降档；退避计数按时间衰减
#include "FasFrames.hpp"
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_fail = 0;

bool eqi(int got, int want, const char* what) {
    if (got == want) return true;
    std::printf("FAIL %s: got %d want %d\n", what, got, want);
    g_fail = 1;
    return false;
}

bool eqb(bool got, bool want, const char* what) {
    if (got == want) return true;
    std::printf("FAIL %s: got %d want %d\n", what, (int)got, (int)want);
    g_fail = 1;
    return false;
}

bool approx(float got, float want, float eps, const char* what) {
    const float d = got > want ? got - want : want - got;
    if (d <= eps) return true;
    std::printf("FAIL %s: got %.4f want %.4f\n", what, got, want);
    g_fail = 1;
    return false;
}

// 构造一段 --latency 文本：首行刷新周期，随后每帧一行三列（A/B/C），B 递增 deltaNs。
std::string buildLatency(const float* deltas, int n, long long startNs = 1000000000LL) {
    std::string s = "16666666\n";
    long long t = startNs;
    char line[96];
    for (int i = 0; i <= n; ++i) {
        std::snprintf(line, sizeof(line), "%lld %lld %lld\n", t, t, t);
        s += line;
        if (i < n) t += (long long)(deltas[i] * 1e6f);
    }
    return s;
}

// ── 1. 解析：正常 16.6ms 一帧 ────────────────────────────────────────────────
void testParseNormal() {
    const float d[5] = {16.67f, 16.67f, 16.67f, 16.67f, 16.67f};
    const std::string txt = buildLatency(d, 5);
    kin::FrameBatch b;
    if (!eqb(kin::parseLatencyFrames(txt.c_str(), b, 150.0f), true, "parse ok")) return;
    eqi(b.n, 5, "frame count");
    approx(b.meanMs, 16.67f, 0.1f, "mean frame ms");
    approx(b.maxMs, 16.67f, 0.1f, "max frame ms");
    eqi(b.heavyCount, 0, "no heavy frames");
    approx(b.heavyMs, 0.0f, 1e-3f, "no heavy ms");
}

// ── 2. 解析：哨兵与非单调必须被跳过 ────────────────────────────────────────
void testParseSentinels() {
    // 首行 + 一帧 + 两行哨兵(0 / INT64_MAX) + 一帧
    const char* txt =
        "16666666\n"
        "1000 2000000000 2000000000\n"          // 基线帧（无间隔）
        "0 0 0\n"                               // 哨兵：B=0
        "1000 9223372036854775807 9223372036854775807\n"  // 哨兵：B=INT64_MAX
        "1000 2030000000 2030000000\n";         // +30ms
    kin::FrameBatch b;
    if (!eqb(kin::parseLatencyFrames(txt, b, 150.0f), true, "parse with sentinels")) return;
    eqi(b.n, 1, "only one valid interval");
    approx(b.deltaMs[0], 30.0f, 0.1f, "interval across sentinels is 30ms");
}

// ── 3. 解析：不足 2 帧 → 不可用 ────────────────────────────────────────────
void testParseInsufficient() {
    kin::FrameBatch b;
    if (!eqb(kin::parseLatencyFrames("16666666\n", b, 150.0f), false, "empty body unusable")) return;
    const char* one = "16666666\n1000 2000000000 2000000000\n";
    if (!eqb(kin::parseLatencyFrames(one, b, 150.0f), false, "single frame unusable")) return;
    if (!eqb(kin::parseLatencyFrames(nullptr, b, 150.0f), false, "null text unusable")) return;
}

// ── 4. 解析：重帧识别 + p95 ────────────────────────────────────────────────
void testParseHeavy() {
    // 8 帧 16.67ms + 1 帧 300ms
    float d[9];
    for (int i = 0; i < 8; ++i) d[i] = 16.67f;
    d[8] = 300.0f;
    const std::string txt = buildLatency(d, 9);
    kin::FrameBatch b;
    if (!eqb(kin::parseLatencyFrames(txt.c_str(), b, 150.0f), true, "parse heavy")) return;
    eqi(b.n, 9, "heavy frame count n");
    eqi(b.heavyCount, 1, "one heavy frame");
    approx(b.heavyMs, 300.0f, 0.5f, "heavy ms");
    approx(b.maxMs, 300.0f, 0.5f, "max is the heavy frame");
    // p95 落在排序后靠后位置：9 帧里 8 个 16.67，第 9 个 300 → idx=round(0.95*8)=8 → 300
    approx(b.p95Ms, 300.0f, 0.5f, "p95 picks the heavy tail");
}

// ── 5. 加载态：累计重帧进加载态 ────────────────────────────────────────────
void testLoadingEnter() {
    kin::LoadingParams p;   // 150ms / 2500ms / tolerance 3
    kin::LoadingState s;
    // 一次 900ms 长帧不够（< 2500ms）
    kin::feedLoadingFrame(s, 900.0f, p);
    if (!eqb(s.loading, false, "900ms alone is not loading")) return;
    // 再来两次 900ms → 累计 2700 ≥ 2500
    kin::feedLoadingFrame(s, 900.0f, p);
    kin::feedLoadingFrame(s, 900.0f, p);
    if (!eqb(s.loading, true, "cumulative heavy enters loading")) return;
}

// ── 6. 加载态：零星长帧不能无限累加（衰减项）──────────────────────────────
void testLoadingDecay() {
    kin::LoadingParams p;
    kin::LoadingState s;
    // 模拟 30 分钟：每 10 秒一次 200ms 长帧，中间全是 16.67ms 正常帧
    // 若没有衰减，200ms × 180 = 36000ms 早就误判成加载态了。
    for (int round = 0; round < 180; ++round) {
        kin::feedLoadingFrame(s, 200.0f, p);
        for (int i = 0; i < 600; ++i) kin::feedLoadingFrame(s, 16.67f, p);
    }
    if (!eqb(s.loading, false, "isolated long frames must not accumulate into loading")) return;
    approx(s.heavyAccumMs, 0.0f, 1.0f, "heavy accumulator decayed to zero");
}

// ── 7. 加载态：退出需要连续正常帧 + 进入保护期 ────────────────────────────
void testLoadingExitAndPostGuard() {
    kin::LoadingParams p;
    kin::LoadingState s;
    for (int i = 0; i < 4; ++i) kin::feedLoadingFrame(s, 900.0f, p);   // 进加载态
    if (!eqb(s.loading, true, "in loading")) return;

    // 只给 2 个正常帧（< tolerance 3）→ 仍在加载态
    kin::feedLoadingFrame(s, 16.67f, p);
    kin::feedLoadingFrame(s, 16.67f, p);
    if (!eqb(s.loading, true, "2 normal frames not enough to exit")) return;

    // 第 3 个正常帧 → 退出并进入保护期
    kin::feedLoadingFrame(s, 16.67f, p);
    if (!eqb(s.loading, false, "3 normal frames exit loading")) return;
    if (!eqb(s.postGuard > 0, true, "post-guard armed after exit")) return;
    eqi(s.postGuard, p.postGuardFrames, "post-guard length");

    // 保护期内必须禁止降档（防读条结束瞬间的频率断崖）
    if (!eqb(kin::loadingBlocksDowngrade(s), true, "post-guard blocks downgrade")) return;
    // 保护期的 perf 覆盖是 postPerf
    approx(kin::loadingPerfOverride(s, p), p.postPerf, 1e-4f, "post-guard perf override");
}

// ── 8. 加载态中的"正常帧"判定：间隔性重帧必须把 normalStreak 清零 ─────────
void testLoadingToleranceReset() {
    kin::LoadingParams p;
    kin::LoadingState s;
    for (int i = 0; i < 4; ++i) kin::feedLoadingFrame(s, 900.0f, p);
    if (!eqb(s.loading, true, "in loading")) return;
    kin::feedLoadingFrame(s, 16.67f, p);
    kin::feedLoadingFrame(s, 16.67f, p);
    kin::feedLoadingFrame(s, 300.0f, p);          // 又一个重帧 → 清零
    eqi(s.normalStreak, 0, "heavy frame resets normal streak");
    kin::feedLoadingFrame(s, 16.67f, p);
    kin::feedLoadingFrame(s, 16.67f, p);
    if (!eqb(s.loading, true, "still loading after reset + 2 normals")) return;
}

// ── 9. 加载态 perf 覆盖：加载中给 ceiling，非加载给 0（不覆盖）───────────
void testLoadingPerfOverride() {
    kin::LoadingParams p;
    kin::LoadingState s;
    approx(kin::loadingPerfOverride(s, p), 0.0f, 1e-6f, "idle -> no override");
    for (int i = 0; i < 4; ++i) kin::feedLoadingFrame(s, 900.0f, p);
    approx(kin::loadingPerfOverride(s, p), p.perfCeiling, 1e-4f, "loading -> ceiling");
    // 边界：ceiling 必须在 [floor, ceiling] 区间内且不高于 1.0
    if (!(p.perfCeiling >= p.perfFloor && p.perfCeiling <= 1.0f)) {
        std::printf("FAIL loading perf ceiling out of range\n");
        g_fail = 1;
    }
}

// ── 10. 窗口兜底：慢帧 + GPU 闲 → 加载态 ──────────────────────────────────
void testLoadingWindow() {
    kin::LoadingParams p;
    kin::LoadingState s;
    // 目标 60fps，实测 15fps（< 0.4×60=24），GPU 10%（闲），每拍 600ms
    // 累计 2500ms 需要 5 拍（3000ms）
    for (int i = 0; i < 4; ++i) kin::feedLoadingWindow(s, 15.0f, 60, 10, 600.0f, p);
    if (!eqb(s.loading, false, "4 ticks of slow not yet loading")) return;
    kin::feedLoadingWindow(s, 15.0f, 60, 10, 600.0f, p);
    if (!eqb(s.loading, true, "5 ticks of slow enters loading")) return;

    // 退出：帧率回到目标的 80% 以上，连续 3 次
    for (int i = 0; i < 3; ++i) kin::feedLoadingWindow(s, 55.0f, 60, 10, 600.0f, p);
    if (!eqb(s.loading, false, "fps back to 80% exits loading")) return;
}

// ── 11. 窗口兜底：GPU 忙时不判加载态（GPU-bound 掉帧不是读条）─────────────
void testLoadingWindowGpuBusy() {
    kin::LoadingParams p;
    kin::LoadingState s;
    for (int i = 0; i < 10; ++i) kin::feedLoadingWindow(s, 15.0f, 60, 95, 600.0f, p);
    if (!eqb(s.loading, false, "GPU-busy slow frames are not loading")) return;
}

// ── 12. 档位驻留：升档后未住满不许降档 ────────────────────────────────────
void testGearDwell() {
    kin::GearParams p;   // minDwellFrames 120
    kin::GearState g;
    kin::gearNoteUpgrade(g);
    eqi(g.framesSinceUpgrade, 0, "upgrade resets dwell");
    if (!eqb(kin::gearAllowsDowngrade(g, p), false, "no downgrade right after upgrade")) return;

    for (int i = 0; i < 119; ++i) kin::gearTick(g, p, (long)i);
    if (!eqb(kin::gearAllowsDowngrade(g, p), false, "119 frames still blocked")) return;

    kin::gearTick(g, p, 120);
    if (!eqb(kin::gearAllowsDowngrade(g, p), true, "120 frames allows downgrade")) return;
}

// ── 13. 档位驻留：退避计数按时间衰减 ──────────────────────────────────────
void testGearBackoffDecay() {
    kin::GearParams p;   // reset 60s
    kin::GearState g;
    kin::gearNoteDowngrade(g, 1000);
    eqi(g.downgradeBackoff, 1, "first downgrade");
    kin::gearNoteDowngrade(g, 2000);
    eqi(g.downgradeBackoff, 2, "second downgrade");

    // 未到 60s：退避保持
    kin::gearTick(g, p, 3000);
    eqi(g.downgradeBackoff, 2, "backoff kept before reset window");

    // 超过 60s（距**上次降档** 2000ms 起算）：归 1（新一轮）
    kin::gearTick(g, p, 2000 + p.downgradeBackoffResetMs + 1);
    eqi(g.downgradeBackoff, 1, "backoff decays after reset window");
}

// ── 14. 档位驻留：退避计数上限 4 ─────────────────────────────────────────
void testGearBackoffCap() {
    kin::GearParams p;
    kin::GearState g;
    for (int i = 0; i < 10; ++i) kin::gearNoteDowngrade(g, 1000 + i);
    eqi(g.downgradeBackoff, 4, "backoff capped at 4");
}

// ── 15. 升档落点 perf 上限 ───────────────────────────────────────────────
void testGearUpgradePerf() {
    kin::GearParams p;   // minUpgradePerfCeil 0.62
    approx(kin::gearUpgradePerf(0.95f, p), 0.62f, 1e-5f, "upgrade perf capped");
    approx(kin::gearUpgradePerf(0.40f, p), 0.40f, 1e-5f, "lower target not raised");
    approx(kin::gearUpgradePerf(0.0f, p),  0.62f, 1e-5f, "unknown target uses cap");
}

// ── 15b. 按帧数批量推进（窗口路径必须用它，否则驻留永远走不完）────────────
void testGearTickFrames() {
    kin::GearParams p;   // minDwellFrames 120
    kin::GearState g;
    kin::gearNoteUpgrade(g);
    // 窗口路径：每拍折算 36 帧（60fps × 600ms），4 拍 = 144 帧 ≥ 120
    kin::gearTickFrames(g, p, 0, 36);
    if (!eqb(kin::gearAllowsDowngrade(g, p), false, "36 frames still blocked")) return;
    kin::gearTickFrames(g, p, 600, 36);
    kin::gearTickFrames(g, p, 1200, 36);
    if (!eqb(kin::gearAllowsDowngrade(g, p), false, "108 frames still blocked")) return;
    kin::gearTickFrames(g, p, 1800, 36);
    if (!eqb(kin::gearAllowsDowngrade(g, p), true, "144 frames allows downgrade")) return;
    // 计数不得越过上限（否则时间衰减/重置逻辑会被巨大值撑爆）
    kin::gearTickFrames(g, p, 2400, 100000);
    eqi(g.framesSinceUpgrade, p.minDwellFrames, "framesSinceUpgrade clamped at dwell");
}

// ── 16. 输入域：任意帧时长都不得产生 NaN / 负累计 ─────────────────────────
void testLoadingDomain() {
    kin::LoadingParams p;
    kin::LoadingState s;
    const float inputs[] = {-1.0f, 0.0f, 0.001f, 16.67f, 150.0f, 1000.0f, 1e6f};
    for (float f : inputs) {
        for (int i = 0; i < 50; ++i) {
            kin::feedLoadingFrame(s, f, p);
            if (!(s.heavyAccumMs >= 0.0f)) {
                std::printf("FAIL heavyAccumMs went negative: %.3f (frame %.3f)\n", s.heavyAccumMs, f);
                g_fail = 1;
                return;
            }
        }
    }
    // 窗口路径同样
    kin::LoadingState w;
    for (int i = 0; i < 200; ++i) {
        kin::feedLoadingWindow(w, (float)(i % 120), 60, i % 100, 600.0f, p);
        if (!(w.heavyAccumMs >= 0.0f)) {
            std::printf("FAIL window heavyAccumMs negative\n");
            g_fail = 1;
            return;
        }
    }
}

}  // namespace

int main() {
    testParseNormal();
    testParseSentinels();
    testParseInsufficient();
    testParseHeavy();
    testLoadingEnter();
    testLoadingDecay();
    testLoadingExitAndPostGuard();
    testLoadingToleranceReset();
    testLoadingPerfOverride();
    testLoadingWindow();
    testLoadingWindowGpuBusy();
    testGearDwell();
    testGearBackoffDecay();
    testGearBackoffCap();
    testGearUpgradePerf();
    testGearTickFrames();
    testLoadingDomain();

    if (g_fail) {
        std::puts("FAIL fas frames: see above");
        return 1;
    }
    std::puts("PASS fas frames: parse (normal/sentinel/heavy), loading (enter/decay/exit/post-guard/"
              "window/gpu-busy), gear (dwell/backoff/decay/cap/upgrade), domain");
    return 0;
}
