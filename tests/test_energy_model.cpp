// 帕累托前沿 / 相对能效模型（EEM）单测 —— P2-1 的算法内核。
//
// 断言重点（对应文件头两条反面教训 + 缩放不变性）：
//   1) 甜点判据必须**缩放不变**：相对向量整体 ×k 后甜点下标不能漂
//   2) Δpow == 0 的段视为无限能效，甜点必须越过它
//   3) 成本/供给按**忙核数 k** 算，k 越大同一需求的落档越低、总成本越低
//   4) 退化输入一律返回 false，不许猜
#include "EnergyModel.hpp"
#include <cstdio>
#include <initializer_list>

namespace {

int g_fail = 0;

bool eqi(int got, int want, const char* what) {
    if (got == want) return true;
    std::printf("FAIL %s: got %d want %d\n", what, got, want);
    g_fail = 1;
    return false;
}

bool approx(float got, float want, float eps, const char* what) {
    const float d = got > want ? got - want : want - got;
    if (d <= eps) return true;
    std::printf("FAIL %s: got %.6f want %.6f\n", what, got, want);
    g_fail = 1;
    return false;
}

bool eqb(bool got, bool want, const char* what) {
    if (got == want) return true;
    std::printf("FAIL %s: got %d want %d\n", what, (int)got, (int)want);
    g_fail = 1;
    return false;
}

// ── 1. 甜点判据：手工构造一条「前段高效、后段低效」的曲线 ──
// 档位:   0     1     2     3     4
// capRel: 0   0.25  0.50  0.75  1.00
// powRel: 0   0.10  0.25  0.60  1.00
//   Δeff:    --   2.50  1.67  0.714 0.625
// 最后一次 ≥1.0 落在档 2 → sweet = 2
void testSweetBasic() {
    const float cap[5] = {0.0f, 0.25f, 0.50f, 0.75f, 1.00f};
    const float pow[5] = {0.0f, 0.10f, 0.25f, 0.60f, 1.00f};
    eqi(kin::sweetIndex(cap, pow, 5), 2, "sweet index from hand-computed curve");
}

// ── 2. 缩放不变性（教训 [2] 的直接守卫）──────────────────────────────────
void testSweetScaleInvariant() {
    const float cap[6] = {0.0f, 0.15f, 0.32f, 0.55f, 0.78f, 1.0f};
    const float pow[6] = {0.0f, 0.08f, 0.20f, 0.42f, 0.70f, 1.0f};
    const int base = kin::sweetIndex(cap, pow, 6);
    for (float s : {0.5f, 2.0f, 0.01f, 100.0f}) {
        float c2[6], p2[6];
        for (int i = 0; i < 6; ++i) { c2[i] = cap[i] * s; p2[i] = pow[i] * s; }
        if (kin::sweetIndex(c2, p2, 6) != base) {
            std::printf("FAIL sweet index drifted under scale %.4f\n", s);
            g_fail = 1;
            return;
        }
    }
}

// ── 3. Δpow == 0 的段视为无限能效，甜点必须越过它 ────────────────────────
void testSweetZeroPowerDelta() {
    // 档 2 的功耗增量恰好为 0（免费）→ 甜点至少要到 2
    const float cap[4] = {0.0f, 0.30f, 0.60f, 1.00f};
    const float pow[4] = {0.0f, 0.20f, 0.20f, 1.00f};
    if (kin::sweetIndex(cap, pow, 4) < 2) {
        std::printf("FAIL sweet must cross a zero-power-delta segment\n");
        g_fail = 1;
    }
}

// ── 4. 全程斜率 < 1.0 → 返回 0（首档即最优）──────────────────────────────
void testSweetFlatSlope() {
    const float cap[4] = {0.0f, 0.10f, 0.20f, 1.00f};
    const float pow[4] = {0.0f, 0.40f, 0.80f, 1.00f};
    // 前两段 Δcap/Δpow = 0.25 / 0.20 < 1；最后一段 0.8/0.2 = 4 ≥ 1 → 落到 3
    eqi(kin::sweetIndex(cap, pow, 4), 3, "last segment decides");
    // 真·全程低效
    const float cap2[3] = {0.0f, 0.05f, 1.0f};
    const float pow2[3] = {0.0f, 0.95f, 1.0f};
    // Δ1 = 0.05/0.95 = 0.053；Δ2 = 0.95/0.05 = 19 ≥ 1 → 3 档里的档 2
    eqi(kin::sweetIndex(cap2, pow2, 3), 2, "steep tail still qualifies");
}

// ── 5. 退化输入 ──────────────────────────────────────────────────────────
void testSweetDegenerate() {
    const float a[2] = {0.0f, 1.0f};
    eqi(kin::sweetIndex(nullptr, a, 2), 0, "null cap -> 0");
    eqi(kin::sweetIndex(a, nullptr, 2), 0, "null pow -> 0");
    eqi(kin::sweetIndex(a, a, 1), 0, "n<2 -> 0");
    eqi(kin::sweetIndex(a, a, 0), 0, "n=0 -> 0");
}

// ── 5b. 微小正增量不得被当成「免费」（直接守卫教训 [2]）─────────────────────
// 用 EPSILON 类绝对阈值时，dp=1e-7 会被判成「零功耗增量」→ 边际能效看成无穷大 →
// 甜点被错误地推过该档。严格 `dp <= 0` 才判免费。
void testSweetTinyPositiveDelta() {
    // i=1: dc=1e-8, dp=1e-7 → eff = 0.1 < 1 → 不该更新甜点
    // i=2: dc≈0.5, dp≈1.0  → eff = 0.5 < 1 → 也不该更新
    // 正确结果 sweet = 0；用 EPSILON 绝对阈值的实现会得 1。
    const float cap[3] = {0.0f, 1e-8f, 0.5f};
    const float pow[3] = {0.0f, 1e-7f, 1.0f};
    eqi(kin::sweetIndex(cap, pow, 3), 0, "tiny positive dp is NOT free");
}

// ── 5c. cost/supply 必须与忙核数 k 成正比（直接守卫教训 [1]）────────────
// 只比较「k 大时更便宜」还不够 —— 成本不乘 k 时那个断言仍会通过（因为低档功耗本来就更低）。
// 这里直接断言公式：cost(g,3) == 3 × cost(g,1)。
void testCostScalesWithK() {
    const int freq[4] = {600000, 1000000, 1600000, 2400000};
    const float capRaw[4] = {600.0f, 1000.0f, 1600.0f, 2400.0f};
    const float powRaw[4] = {12.0f, 22.0f, 55.0f, 190.0f};
    kin::EnergyCurve c;
    if (!eqb(kin::buildCurve(freq, capRaw, powRaw, 4, c), true, "build for k-scaling test")) return;
    for (int g = 0; g < c.n; ++g) {
        approx(c.cost(g, 3),   3.0f * c.cost(g, 1),   1e-5f, "cost scales linearly with k");
        approx(c.supply(g, 3), 3.0f * c.supply(g, 1), 1e-5f, "supply scales linearly with k");
    }
}

// ── 6. buildCurve：正常构造 + 归一正确性 ─────────────────────────────────
void testBuildCurve() {
    const int freq[4] = {300000, 900000, 1500000, 2400000};
    const float capRaw[4] = {300.0f, 900.0f, 1500.0f, 2400.0f};   // capacity × freq 口径
    const float powRaw[4] = {10.0f, 25.0f, 60.0f, 200.0f};
    kin::EnergyCurve c;
    if (!eqb(kin::buildCurve(freq, capRaw, powRaw, 4, c), true, "build ok")) return;
    eqi(c.n, 4, "curve n");
    approx(c.capRel[0], 0.0f, 1e-6f, "capRel first is 0");
    approx(c.capRel[3], 1.0f, 1e-6f, "capRel last is 1");
    approx(c.powRel[0], 0.0f, 1e-6f, "powRel first is 0");
    approx(c.powRel[3], 1.0f, 1e-6f, "powRel last is 1");
    eqi(c.freqKhz[2], 1500000, "freq preserved");
    // 甜点必须落在合法区间
    if (c.sweetIdx < 0 || c.sweetIdx >= c.n) {
        std::printf("FAIL sweet idx out of range\n");
        g_fail = 1;
    }
}

// ── 7. buildCurve：退化一律拒绝（不猜）────────────────────────────────────
void testBuildCurveRejects() {
    kin::EnergyCurve c;
    const int f1[1] = {300000};
    const float p1[1] = {10.0f};
    if (!eqb(kin::buildCurve(f1, p1, p1, 1, c), false, "n<2 rejected")) return;

    const int fBad[3] = {900000, 300000, 1500000};   // 非升序
    const float p3[3] = {10.0f, 20.0f, 30.0f};
    if (!eqb(kin::buildCurve(fBad, p3, p3, 3, c), false, "non-ascending freq rejected")) return;

    const int fOk[3] = {300000, 900000, 1500000};
    const float pFlat[3] = {10.0f, 10.0f, 10.0f};     // 末档不大于首档
    if (!eqb(kin::buildCurve(fOk, p3, pFlat, 3, c), false, "flat power rejected")) return;

    const float capFlat[3] = {5.0f, 5.0f, 5.0f};
    if (!eqb(kin::buildCurve(fOk, capFlat, p3, 3, c), false, "flat capacity rejected")) return;

    const int fZero[3] = {0, 900000, 1500000};
    if (!eqb(kin::buildCurve(fZero, p3, p3, 3, c), false, "zero freq rejected")) return;
}

// ── 8. 成本/供给按忙核数 k（教训 [1]）────────────────────────────────────
void testCostByBusyCores() {
    const int freq[4] = {600000, 1000000, 1600000, 2400000};
    const float capRaw[4] = {600.0f, 1000.0f, 1600.0f, 2400.0f};
    const float powRaw[4] = {12.0f, 22.0f, 55.0f, 190.0f};
    kin::EnergyCurve c;
    if (!eqb(kin::buildCurve(freq, capRaw, powRaw, 4, c), true, "build for k test")) return;

    // 同一份需求：k=1 需要更高档，k=3 可以用更低档
    const float demand = 0.55f;
    const int g1 = kin::gearForDemand(c, demand, 1, true);
    const int g3 = kin::gearForDemand(c, demand, 3, true);
    if (g1 < 0 || g3 < 0) { std::printf("FAIL gear not found\n"); g_fail = 1; return; }
    if (!(g3 <= g1)) {
        std::printf("FAIL k=3 should not need a higher gear than k=1 (g1=%d g3=%d)\n", g1, g3);
        g_fail = 1;
    }
    // 关键：k=3 在更低档上的**总成本**必须低于 k=1 在高档上的成本
    const float cost1 = c.cost(g1, 1);
    const float cost3 = c.cost(g3, 3);
    if (!(cost3 < cost1)) {
        std::printf("FAIL spreading over 3 cores should cost less (%.4f vs %.4f)\n", cost3, cost1);
        g_fail = 1;
    }
}

// ── 9. 甜点封顶：allowAboveSweet=false 时永不越界 ────────────────────────
void testGearAboveSweetGate() {
    const int freq[5] = {300000, 800000, 1400000, 2000000, 2800000};
    const float capRaw[5] = {300.0f, 800.0f, 1400.0f, 2000.0f, 2800.0f};
    const float powRaw[5] = {8.0f, 18.0f, 42.0f, 130.0f, 400.0f};
    kin::EnergyCurve c;
    if (!eqb(kin::buildCurve(freq, capRaw, powRaw, 5, c), true, "build for gate test")) return;

    // 需求超出甜点档供给、但在顶档供给之内：
    //   封顶模式返回 -1（装不下，交给调用方的抗饥饿逻辑）
    //   放行模式能拿到顶档
    // 注：demand 不能超过 capRel 顶档（=1.0），否则放行模式同样返回 -1 —— 那是正确行为，
    //     所以这里取 0.9 而不是一个荒谬的大值。
    const float overSweet = 0.9f;
    if (!(c.supply(c.sweetIdx, 1) < overSweet && overSweet <= c.supply(c.n - 1, 1))) {
        std::printf("FAIL precondition: 0.9 should be above sweet supply and within top supply\n");
        g_fail = 1;
        return;
    }
    eqi(kin::gearForDemand(c, overSweet, 1, false), -1, "capped mode returns -1 when demand exceeds sweet");
    eqi(kin::gearForDemand(c, overSweet, 1, true), c.n - 1, "uncapped mode reaches top gear");
    // 真正装不下的需求：两种模式都返回 -1
    eqi(kin::gearForDemand(c, 99.0f, 1, true), -1, "demand beyond top supply -> -1 even uncapped");

    // 普通需求：封顶模式的落档必须 <= sweetIdx
    const int g = kin::gearForDemand(c, 0.3f, 1, false);
    if (g >= 0 && g > c.sweetIdx) {
        std::printf("FAIL capped mode returned gear %d above sweet %d\n", g, c.sweetIdx);
        g_fail = 1;
    }
}

// ── 10. 解析式功耗模型：单调递增且凸 ─────────────────────────────────────
void testAnalyticPower() {
    const int freq[4] = {300000, 900000, 1500000, 2400000};
    float pw[4] = {};
    if (!eqb(kin::analyticPower(freq, 4, 600.0f, 1100.0f, pw), true, "analytic ok")) return;
    for (int i = 1; i < 4; ++i) {
        if (!(pw[i] > pw[i - 1])) {
            std::printf("FAIL analytic power not increasing at %d\n", i);
            g_fail = 1;
            return;
        }
    }
    // 凸性：二阶差分 > 0（P = f·V² 在 V 随 f 线性增长时是凸的）
    const float d2 = (pw[3] - pw[2]) - (pw[2] - pw[1]);
    if (!(d2 > 0.0f)) {
        std::printf("FAIL analytic power not convex (d2=%.4f)\n", d2);
        g_fail = 1;
    }
    // 非法电压
    if (!eqb(kin::analyticPower(freq, 4, 0.0f, 1100.0f, pw), false, "zero vmin rejected")) return;
    if (!eqb(kin::analyticPower(freq, 4, 1100.0f, 600.0f, pw), false, "inverted vmin/vmax rejected")) return;
}

// ── 11. 解析式模型能走通 buildCurve，且甜点落在合理区间 ──────────────────
void testAnalyticCurveSweet() {
    const int freq[6] = {300000, 700000, 1100000, 1600000, 2100000, 2600000};
    float pw[6] = {};
    float cap[6] = {};
    if (!eqb(kin::analyticPower(freq, 6, 600.0f, 1100.0f, pw), true, "analytic for curve")) return;
    for (int i = 0; i < 6; ++i) cap[i] = (float)freq[i];   // capacity 恒定时 cap ∝ f
    kin::EnergyCurve c;
    if (!eqb(kin::buildCurve(freq, cap, pw, 6, c), true, "analytic curve builds")) return;
    // 甜点不能是首档（首档永远最优说明模型退化），也不该是顶档（顶档永远最优说明没有拐点）
    if (c.sweetIdx <= 0 || c.sweetIdx >= c.n - 1) {
        std::printf("FAIL analytic sweet idx %d looks degenerate (n=%d)\n", c.sweetIdx, c.n);
        g_fail = 1;
    }
    // 甜点频点必须在档位表里
    if (!(c.sweetKhz() > 0)) { std::printf("FAIL sweet khz invalid\n"); g_fail = 1; }
}

// ── 12. 占用门槛 ceil(n/2) ───────────────────────────────────────────────
void testBusyGate() {
    approx(kin::busyGate(6), 3.0f, 1e-6f, "6 cores -> 3.0");
    approx(kin::busyGate(4), 2.0f, 1e-6f, "4 cores -> 2.0");
    approx(kin::busyGate(2), 1.0f, 1e-6f, "2 cores -> 1.0");
    approx(kin::busyGate(1), 1.0f, 1e-6f, "1 core -> 1.0");
    approx(kin::busyGate(0), 1.0f, 1e-6f, "0 cores guarded to 1.0");
    approx(kin::busyGate(3), 2.0f, 1e-6f, "3 cores -> ceil(1.5) = 2.0");
}

// ── 13. 非法档位下标不越界 ───────────────────────────────────────────────
void testOutOfRangeGear() {
    const int freq[3] = {300000, 900000, 1500000};
    const float capRaw[3] = {300.0f, 900.0f, 1500.0f};
    const float powRaw[3] = {10.0f, 25.0f, 70.0f};
    kin::EnergyCurve c;
    if (!eqb(kin::buildCurve(freq, capRaw, powRaw, 3, c), true, "build for range test")) return;
    approx(c.supply(-1, 1), 0.0f, 1e-6f, "supply(-1) = 0");
    approx(c.supply(99, 1), 0.0f, 1e-6f, "supply(99) = 0");
    approx(c.cost(-1, 1), 0.0f, 1e-6f, "cost(-1) = 0");
    approx(c.cost(99, 1), 0.0f, 1e-6f, "cost(99) = 0");
    // k <= 0 按 1 处理（防除零/零供给）
    approx(c.supply(1, 0), c.supply(1, 1), 1e-6f, "k=0 treated as 1");
    approx(c.supply(1, -5), c.supply(1, 1), 1e-6f, "negative k treated as 1");
}

}  // namespace

int main() {
    testSweetBasic();
    testSweetScaleInvariant();
    testSweetZeroPowerDelta();
    testSweetFlatSlope();
    testSweetDegenerate();
    testSweetTinyPositiveDelta();
    testCostScalesWithK();
    testBuildCurve();
    testBuildCurveRejects();
    testCostByBusyCores();
    testGearAboveSweetGate();
    testAnalyticPower();
    testAnalyticCurveSweet();
    testBusyGate();
    testOutOfRangeGear();

    if (g_fail) {
        std::puts("FAIL energy model: see above");
        return 1;
    }
    std::puts("PASS energy model: sweet (basic/scale-invariant/zero-delta/flat/degenerate/"
              "tiny-positive-delta), curve (build/rejects), cost-by-busy-cores/k-scaling, "
              "sweet gate, analytic power, busy gate, range");
    return 0;
}
