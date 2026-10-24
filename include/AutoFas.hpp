// Kin FAS (Frame-Aware Scheduling) —— 仅 GAMING 状态启用的闭环帧率控制器。
//
// 移植自 shadow3aaa/fas-rs 的归一化 P 控制器(kp=3e-4, 目标帧时归一化到 1e9ns):
//   误差 error = mean_frametime_ns * target_fps - 1e9   (>0 表示帧偏慢/掉帧)
//   频率增量 dKHz = kp * error                          (一整帧超时 ≈ +300MHz)
//   建议频率 = clamp(cur_fas_max + dKHz, util_ceiling, [floor, hwmax])
//   未掉帧时用 util 上限省电(cur*util/0.5); 掉帧(fps<target-2)放开上限快速追帧。
//
// 帧源(静态二进制可行性): eBPF/libbpf 不可行(-static -s -Os -flto bionic),
//   主源 = 高通 SDE CRTC measured_fps sysfs(缓存 fd, 单 pread); 兜底 = dumpsys SF --latency(≤1Hz, 仅游戏)。
//   两者都拿不到 → engaged=false, 调用方保持现有静态 GAMING 上限(绝不让游戏变差)。
//
// 集成: 仅在 fsm && GAMING && 亮屏 && fas_enable && 帧源可用 时覆盖 scaling_max; 不碰 scaling_min/GPU。
#pragma once

#include "Config.hpp"
#include "Logger.hpp"
#include "AutoFsm.hpp"
#include "FasFrames.hpp"
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

class AutoFas {
public:
    struct Out { bool engaged; int maxL; int maxB; };

    // ── fas-rs 默认常量 ──
    static constexpr double KP            = 3e-4;     // controll.rs params.kp
    static constexpr double TARGET_NS     = 1e9;      // 归一化目标帧时(1.0s)
    static constexpr double UTIL_TARGET   = 0.5;      // util 上限分母(50% 目标利用率)
    static constexpr double JANK_FPS_GAP  = 2.0;      // 掉帧判据基准(@60fps); 实际按目标帧率归一化, 见 jankGap()
    static constexpr double MARGIN_FPS    = 1.0;      // tier 命中容差 & 目标余量基准(@60fps); 对齐 fas-rs balance 默认(原 3.0=powersave 档, 余量过大稳态频率偏高费电)
    // ── 响应性调优(升降频更及时): 步长/预热/EMA 较 fas-rs 直译收紧 ──
    static constexpr int    MAX_STEP_KHZ  = 500000;   // 单步增量钳制(防过冲; 400→500MHz, 配合更快轮询提高追帧斜率)
    static constexpr int    WARMUP_FRAMES = 6;        // 取得控制权前的预热拍数(8→6, 更快接管)
    static constexpr long   ENGAGE_DELAY_MS = 400;    // 进入 GAMING 后驻留再控制(600→400ms)
    static constexpr int    FT_WIN        = 32;       // 滚动帧时窗口
    static constexpr int    EMA_NUM = 1, EMA_DEN = 3; // fps EMA: new = old*2/3 + cur*1/3(1/4→1/3, 响应更快;
                                                      // measured_fps 本身已是 ~1s 窗均值, 噪声可控不震荡)
    static constexpr int    TIERS[7]      = {30, 45, 60, 90, 120, 144, 165};  // fas-rs auto=[30,45,60,90,120,144] + 165(旗舰高刷); 45 适配明潮/幻塔等 [24,30,45,60] 游戏档
    static constexpr long   RETARGET_CONFIRM_MS = 20000; // 目标上修确认: 需连续高于目标达 20s(墙钟)才升
                                                          // —— 按【时长】而非 tick 数, 才能真正把加载页/菜单的
                                                          //    短时高帧(常数秒)挡在外面, 不误把 60fps 游戏锁到 120。
    static constexpr long   RETARGET_DOWN_MS   = 10000; // 目标降档确认: 稳态长窗均值持续低于目标(掉帧线下方)达 10s 才降
                                                          // —— 解决限帧游戏(90→60)/场景切换的目标滞留(原只升不降会持续
                                                          //    误判掉帧、频率钉满); 比上修快(掉帧是真问题要快适应), 但不太快防瞬时误降。
    static constexpr double JANK_BIG_FRAC = 0.85;    // 实测帧率<目标×0.85(掉>15%)=重掉帧, 一拍顶 hwmax 追回
    static constexpr int    POLL_JANK_MS  = 120;     // 掉帧时加快轮询(160→120ms, 追帧更快)
    static constexpr int    POLL_NORM_MS  = 200;     // 常速(250→200ms, 5Hz; 升降频决策更密)
    static constexpr int    POLL_CALM_MS  = 400;     // 明显高于目标且稳→放慢省唤醒/省电(450→400)
    // util 驱动的"目标 fps 下修"慢环(移植 fas-rs adjust_target_fps): 负载有余量且帧达标时缓降目标省电。
    static constexpr int    TGT_OFFSET_MIN_X10 = -20; // 目标 fps 最多下压 2.0(=JANK_FPS_GAP, 十分位): 封顶保证设定点恒高于掉帧线, 不自造掉帧
    static constexpr int    UTIL_LOW_PCT     = 55;   // 负载(max CPU,GPU)≤此值=有余量→缓降目标
    static constexpr int    UTIL_HIGH_PCT    = 65;   // 负载≥此值=吃紧→缓回满目标
    static constexpr int    FAS_CONCURRENT_LOAD = 88; // 加权整机负载≥此值且帧达标=疑似后台并发→本拍不下压(交回 FSM)
    static constexpr long   OFFSET_SAMPLE_MS = 1000; // 慢环采样周期 1s

    explicit AutoFas(Logger& lg) : logger_(lg) {}

    // ──────────── 帧源探测 (一次性) ────────────
    bool available() {
        if (probed_) return src_ != SRC_NONE;
        probed_ = true;
        for (const char* const* pp = Soc::kMeasuredFps; *pp; ++pp) {   // 候选列表见 Soc 适配层
            int fd = open(*pp, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) { close(fd); fpsPath_ = *pp; src_ = SRC_SYSFS; break; }
        }
        if (src_ == SRC_NONE) src_ = SRC_DUMPSYS;   // 无 sysfs 节点 → 走 1Hz dumpsys 兜底
        logger_.Info("FAS: 帧源 = %s", src_ == SRC_SYSFS ? fpsPath_ : "dumpsys 1Hz");
        return true;
    }
    bool usesSysfs()  const { return src_ == SRC_SYSFS; }
    int  targetFps()  const { return tgtFps_; }
    int  lastFpsX10() const { return lastFpsX10_; }
    bool active()     const { return active_; }

    // ──────────── 生命周期 ────────────
    // targetOverride>0: 该游戏的显式目标帧率(perapp 第三列, 优先于全局 fas_target_fps 与自动学习)。
    void onEnterGaming(const AutoFsm::Caps& seed, long nowMs, int targetOverride = 0) {
        if (active_) return;
        active_ = true; engageMs_ = nowMs; warm_ = 0; ftN_ = 0; ftHead_ = 0;
        // 帧级分析状态复位：新会话不得带着上一局的加载态/驻留/输出基准
        loading_ = kin::LoadingState{};
        gear_    = kin::GearState{};
        lastOutL_ = 0; lastOutB_ = 0; lastTickMs_ = 0;
        int fixed = (targetOverride > 0) ? targetOverride
                  : (Config::AutoMode::fas_target_fps > 0 ? Config::AutoMode::fas_target_fps : 0);
        emaFpsX10_ = 0; tgtFps_ = fixed;
        autoTarget_ = (fixed <= 0); aboveSinceMs_ = 0; belowSinceMs_ = 0;
        fasMaxL_ = seed.maxL; fasMaxB_ = seed.maxB; pollMs_ = POLL_NORM_MS;
        tgtOffX10_ = 0; offsetMs_ = 0;
        pendingTgt_ = 0; pendingCnt_ = 0;
    }
    void onExitGaming() { active_ = false; warm_ = 0; ftN_ = 0; }

    void setForegroundPkg(const char* pkg) {
        if (pkg && pkg[0]) {
            // 换前台(游戏→游戏移交等): 清 dumpsys 帧源的图层缓存 —— 否则继续对旧游戏的
            // 图层测帧(旧游戏小窗仍在渲染时拿别家帧率闭环)。帧史/目标/预热窗的整套重置
            // 由调用方(scheduler)在换包沿做 onExitGaming+onEnterGaming 完成(需要 caps/nowMs)。
            if (fgPkg_[0] && strcmp(fgPkg_, pkg) != 0) {
                dumpLayer_[0] = '\0'; dumpLastMs_ = 0; dumpFpsX10_ = 0;
                layerProbeMs_ = 0; layerSeen_ = false;
            }
            strncpy(fgPkg_, pkg, sizeof(fgPkg_) - 1); fgPkg_[sizeof(fgPkg_)-1] = '\0';
        }
        else fgPkg_[0] = '\0';
    }

    // 设置前台进程 pid(供 util_max 单线程 CPU 比例采样; 移植 fas-rs v4.4 process_monitor)。
    // 换 pid 时清线程 cputime 缓存(旧 pid 的线程统计不再适用)。pid<=0 表示未知, util_max 失效。
    void setFgPid(int pid) {
        if (pid != fgPid_) { fgPid_ = pid; thrN_ = 0; thrUtilMs_ = 0; lastThrUtil_ = -1.0; }
    }

    // 帧时辅助进入 GAMING 的【渲染层判据】: 前台包是否拥有游戏渲染层(SurfaceView/BLAST/"TX -")。
    // 复用 pickLayer 同款过滤(普通 UI 滚动渲染进 App 自身 ViewRoot 层, 不是 SurfaceView, 故能区分滚动)。
    // 注意: 视频播放器也常用 SurfaceView/TextureView —— 故调用方必须再 AND 一个 GPU 地板(视频硬解 GPU 很低),
    //       本函数【只】回答"有没有渲染层", 不单独构成游戏判定。dumpsys --list 节流 ≥2s, 防频繁 popen。
    template <class UtilsT>
    bool hasGameLayer(UtilsT& utils, const char* pkg, long nowMs) {
        if (!pkg || !pkg[0]) return false;
        if (nowMs - layerProbeMs_ < 2000) return layerSeen_;   // 节流复用上次结果
        layerProbeMs_ = nowMs;
        std::vector<char> outBuf(16384, 0);
        char* out = outBuf.data();
        utils.popenRead("dumpsys SurfaceFlinger --list 2>/dev/null", out, outBuf.size());
        layerSeen_ = false;
        const char* p = out;
        while (*p) {
            const char* eol = strchr(p, '\n'); size_t len = eol ? (size_t)(eol - p) : strlen(p);
            if (len > 0 && len < 256) {
                char line[256]; memcpy(line, p, len); line[len] = '\0';
                if (strstr(line, pkg) &&
                    (strstr(line, "BLAST") || strstr(line, "SurfaceView") || strstr(line, "TX -"))) {
                    layerSeen_ = true;
                    // Reuse the matching layer for the imminent FAS latency probe;
                    // otherwise the same tick would launch a second --list command.
                    if (dumpLayer_[0] == '\0') {
                        memcpy(dumpLayer_, line, len + 1);
                    }
                    break;
                }
            }
            if (!eol) break; p = eol + 1;
        }
        return layerSeen_;
    }

    // FAS 轮询周期(ms): measured_fps 是 ~1s 窗口均值, 5Hz(200ms)复查已足够取到新信号,
    // 比逐帧(8-33ms)省掉大量无谓唤醒 —— 游戏时也不浪费电。dumpsys 兜底本就 ≤1Hz。
    int pollIntervalMs() const { return pollMs_; }

    // 预热期(已进会话、尚未取得控制权): 调用方据此把 tick 提到 FAS 轮询节奏 —— 否则预热按
    // FSM 2s 一拍走, 6 拍要 12s 才接管, "快速接管"名存实亡; 按 200ms 拍则 ~1.2s 完成预热。
    // 超过 8s 仍没接管(帧源死/锁不到目标 fps)则放弃快节奏, 避免整场 5Hz 空转费电。
    bool warmingUp(long nowMs) const {
        return active_ && (warm_ < WARMUP_FRAMES || ftN_ < 3 || tgtFps_ <= 0) &&
               nowMs - engageMs_ < 8000;
    }

    // ──────────── 每 tick: 喂帧 + 计算上限 ────────────
    // sysLoadW: 能效加权整机负载%(-1=未知)。用于并发防劣化(GearDVFS 教训): 只盯前台单游戏帧率
    // 的闭环在有后台重负载时会误压 scaling_max 饿死后台 —— 帧达标但整机负载很高时不再下压, 交回静态档。
    template <class Ctl, class UtilsT>
    Out tick(Ctl& ctl, UtilsT& utils, const AutoFsm::Caps& staticCaps,
             int hwMaxL, int hwMaxB, int gpuPct, int cpuBusy, long nowMs, int sysLoadW = -1) {
        Out o{false, staticCaps.maxL, staticCaps.maxB};
        if (!active_) return o;

        syncFrameParams();   // 配置热重载即时生效（三个 int，每拍读一遍的代价可忽略）

        int fpsX10 = readFpsX10(ctl, utils, nowMs);
        if (fpsX10 <= 0) return o;                       // 帧源失效 → 静态回退
        lastFpsX10_ = fpsX10;

        emaFpsX10_ = (emaFpsX10_ <= 0) ? fpsX10
                   : (emaFpsX10_ * (EMA_DEN - EMA_NUM) + fpsX10 * EMA_NUM) / EMA_DEN;

        long long ftNs = (long long)(1e10 / (double)emaFpsX10_);  // 1e9 / (emaFps10/10)
        ft_[ftHead_] = ftNs; ftHead_ = (ftHead_ + 1) % FT_WIN;
        if (ftN_ < FT_WIN) ftN_++;
        if (warm_ < WARMUP_FRAMES) warm_++;

        if (nowMs - engageMs_ < ENGAGE_DELAY_MS) return o;
        if (ftN_ < 3 || warm_ < WARMUP_FRAMES)    return o;

        // 长窗(~8s)均值用于【稳态目标锁定/上修】(避免瞬时高帧把目标锁偏); 控制/掉帧判定用响应快的 EMA。
        double sum = 0; for (int i = 0; i < ftN_; i++) sum += (double)ft_[i];
        double meanFt = sum / (double)ftN_;              // ns
        double meanFps = (meanFt > 0.0) ? 1e9 / meanFt : 0.0;
        // Kin 优化: 控制/掉帧判定用【响应信号 EMA(~1s)】而非 8s 长窗 —— fas-rs 直译拿 8s(250ms×32 拍)
        // 长窗均值做误差项, 重场景要好几秒才追上频率。EMA 时间常数~1s, 追帧快; 而 P 项有单步钳制+util上限+地板, 更灵敏也不震荡。
        double emaFps = (double)emaFpsX10_ / 10.0;
        double emaFt  = (emaFps > 0.0) ? 1e9 / emaFps : meanFt;

        // 初始目标锁定加 2 拍确认(连续两拍同 tier 才落定): 上修路径有 20s 墙钟确认, 但初始锁定
        // 原本一拍定音且自学目标只升不降 —— 加载页/启动动画的高帧若恰好撑过预热, 60fps 游戏会被
        // 永久锁成 90/120, 整场 janked 白烧唤醒。两拍确认代价 ~200ms, 可忽略。
        if (tgtFps_ <= 0) {
            int nt = detectTargetFps(meanFt);
            if (nt > 0 && nt == pendingTgt_) {
                if (++pendingCnt_ >= 2) {
                    tgtFps_ = nt;
                    ftN_ = 0; ftHead_ = 0;   // 目标锁定: 清帧时缓冲, 避免预热期帧史污染新目标控制(对齐 fas-rs calculate.rs reset_frametime_state)
                }
            } else {
                pendingTgt_ = nt; pendingCnt_ = (nt > 0) ? 1 : 0;
            }
        }
        if (tgtFps_ <= 0) return o;                      // 锁不到 tier → 静态回退

        double curFps = emaFps;

        // 自学目标【只升不降】上修: 用【稳态长窗均值 meanFps】判定, 且要求连续高于目标达 RETARGET_CONFIRM_MS
        // (墙钟 20s)才升 —— 否则菜单/加载页的短时高帧(如 60fps 游戏里跑 120fps 的加载页, 常持续数秒)会把
        // 目标永久锁高, 回到正常帧率后每拍误判掉帧、频率钉死满档白烧电。按【时长】而非 tick 数计, 才能真正挡住
        // (tick 时长在 160~450ms 间浮动)。真正持续高帧的游戏仍会在 20s 后正确上修 —— 只延后, 不误伤。
        if (autoTarget_ && meanFps > (double)tgtFps_ + marginFps()) {
            if (aboveSinceMs_ == 0) aboveSinceMs_ = nowMs;
            if (nowMs - aboveSinceMs_ >= RETARGET_CONFIRM_MS) {
                int nt = detectTargetFps(meanFt);
                if (nt > tgtFps_) {
                    logger_.Info("FAS: 自学目标上修 %d→%dfps (实测帧率持续 20s 高于目标)", tgtFps_, nt);
                    tgtFps_ = nt;
                    ftN_ = 0; ftHead_ = 0;   // 目标上修: 清帧时缓冲 —— 旧目标(低 fps 大帧时)会拉高 error 误提频, 清掉让新目标干净起步
                }
                aboveSinceMs_ = nowMs;   // 上修后重新计时(继续观察是否还要再上一档)
            }
        } else {
            aboveSinceMs_ = 0;           // 一旦跌回目标即清零, 要求【连续】高帧
        }

        // 自学目标【降档】: 稳态长窗均值持续明显低于目标(掉帧线下方: tgt-margin-jankGap)达 RETARGET_DOWN_MS
        // → 目标锁高了, 降到 detectTargetFps(meanFt)。解决限帧游戏(90→60)/场景切换的目标滞留
        // (原只升不降会持续误判掉帧、频率钉满)。用稳态长窗 meanFps 判(抗瞬时低帧); 10s 比上修 20s 快。
        if (autoTarget_ && tgtFps_ > 0 && meanFps < (double)tgtFps_ - marginFps() - jankGap()) {
            if (belowSinceMs_ == 0) belowSinceMs_ = nowMs;
            if (nowMs - belowSinceMs_ >= RETARGET_DOWN_MS) {
                int nt = detectTargetFps(meanFt);
                if (nt > 0 && nt < tgtFps_) {
                    logger_.Info("FAS: 自学目标降档 %d→%dfps (实测帧率持续 10s 低于目标)", tgtFps_, nt);
                    tgtFps_ = nt;
                    ftN_ = 0; ftHead_ = 0;   // 降档: 清缓冲, 旧目标(高 fps 小帧时)会拉低 error 误降频
                    aboveSinceMs_ = 0; belowSinceMs_ = 0;
                } else {
                    belowSinceMs_ = nowMs;   // 检测不到更低 tier: 重新计时继续观察
                }
            }
        } else {
            belowSinceMs_ = 0;               // 一旦回到目标附近即清零, 要求【连续】低帧
        }

        const double jgap = jankGap();                           // 帧率归一化掉帧阈值
        bool janked  = curFps < (double)tgtFps_ - jgap;
        bool bigJank = curFps < (double)tgtFps_ * JANK_BIG_FRAC;  // 重掉帧: 本拍输出直接顶满追回(不污染积分器)

        // 负载代理: GPU / 平均 cpuBusy / 能效加权整机负载 / 单线程 CPU 比例 四者取最大 ——
        // CPU-bound 游戏 GPU 低但 CPU 忙; 平均 cpuBusy 会被大核忙小核闲摊薄, 加权负载捕捉"整机满载",
        // 单线程比例(util_max, fas-rs v4.4)捕捉"一个大核打满但整机不忙"(cpuBusyW 会漏判); 四者取大最不易误判"芯片不忙"。
        int loadPct = gpuPct; if (cpuBusy > loadPct) loadPct = cpuBusy;
        if (sysLoadW > loadPct) loadPct = sysLoadW;
        double thrUtil = readThreadUtilMax(nowMs);              // v4.4: 单线程视角补充(节流 300ms)
        if (thrUtil >= 0.0) { int thrPct = (int)(thrUtil * 100.0 + 0.5); if (thrPct > loadPct) loadPct = thrPct; }

        // 并发防劣化(GearDVFS): 帧已达标(未掉帧)但整机加权负载很高 → 大概率有后台重任务在抢核,
        // 此时 FAS 继续下压 scaling_max 会饿死它们。本 tick 不下压, 保持静态 FSM 上限, 把决策交回 FSM。
        if (!janked && sysLoadW >= FAS_CONCURRENT_LOAD) {
            pollMs_ = POLL_NORM_MS;
            o.engaged = true; o.maxL = staticCaps.maxL; o.maxB = staticCaps.maxB;
            return o;
        }

        // util 驱动的目标 fps 下修(移植 fas-rs adjust_target_fps, ~1s 慢环, ±0.1fps 缓动, 整数十分位防浮点漂移):
        // 负载(max CPU,GPU)有明显余量且帧达标时缓降有效目标 → 频率降一点省电(回收限帧游戏浪费功耗);
        // 吃紧→缓回满目标; 掉帧→立刻收回。下压封顶=JANK_FPS_GAP, 保证设定点(effTgt+margin)恒高于掉帧线,
        // 绝不因省电把游戏自己推进掉帧。原用整机平均 cpuBusy 会误压 CPU-bound 游戏成锯齿, 现改 loadPct。
        if (janked) {
            tgtOffX10_ = 0;
        } else if (nowMs - offsetMs_ >= OFFSET_SAMPLE_MS) {
            offsetMs_ = nowMs;
            if      (loadPct < 0)              { /* 负载读不到: 不动 */ }
            else if (loadPct <= 10)            tgtOffX10_ = 0;                    // 近乎空闲(过场/暂停): 复位
            else if (loadPct <= UTIL_LOW_PCT)  tgtOffX10_ -= 1;                   // 有余量: 缓降目标省电
            else if (loadPct >= UTIL_HIGH_PCT) tgtOffX10_ += 1;                   // 吃紧: 缓回满目标
            // 下压封顶随目标帧率归一化: 恒 < jankGap, 保证省电下修后的设定点始终高于掉帧线(不自造掉帧)。
            int offMin = -(int)(jgap * 10.0);
            if (tgtOffX10_ < offMin) tgtOffX10_ = offMin;
            if (tgtOffX10_ > 0)      tgtOffX10_ = 0;
        }
        double effTgt = (double)tgtFps_ + (double)tgtOffX10_ / 10.0;  // 省电下修后的有效目标(掉帧判定仍用真目标)

        double tFps  = effTgt + (janked ? 0.0 : marginFps());
        double errNs = emaFt * tFps - TARGET_NS;         // >0 太慢 → 提频(用响应信号 EMA)
        double dKHz  = KP * errNs;
        if (dKHz >  MAX_STEP_KHZ) dKHz =  MAX_STEP_KHZ;
        if (dKHz < -MAX_STEP_KHZ) dKHz = -MAX_STEP_KHZ;
        int delta = (int)std::lround(dKHz);

        double util = (loadPct >= 0) ? (double)loadPct / 100.0 : -1.0;
        // 积分器(运行设定点 fasMaxL_/B_)只按 P 控制器 + util 上限推进, 【不含重掉帧跳变】——
        // 否则一次瞬时卡顿会把设定点永久钉在满档、抖动过后拉不回来(积分器被污染)。
        fasMaxL_ = applyCluster(fasMaxL_, util, delta, janked, gpuPct, floorOf(staticCaps.maxL), hwMaxL);
        fasMaxB_ = applyCluster(fasMaxB_, util, delta, janked, gpuPct, floorOf(staticCaps.maxB), hwMaxB);

        // 热配合: FAS 永不超过当前静态(alpha 整形)上限 —— 高温时静态上限自然下移, FAS 只能更低。
        if (fasMaxL_ > staticCaps.maxL) fasMaxL_ = staticCaps.maxL;
        if (fasMaxB_ > staticCaps.maxB) fasMaxB_ = staticCaps.maxB;

        // 重掉帧: 本拍【输出】直接顶到静态上限快速追帧, 但不写回积分器 —— 真持续掉帧时积分器会自行经 P 项
        // 在数拍内爬上来; 一次性瞬时卡顿则抖动过后立即回到积分器的真实工作点, 不留后患。
        // GPU 瓶颈(≥95%)时该升的是 GPU, 顶 CPU 输出无益且费电 → 重掉帧也不顶(与积分器里的 GPU 保护一致)。
        int outL = fasMaxL_, outB = fasMaxB_;
        if (bigJank && gpuPct < 95) { outL = staticCaps.maxL; outB = staticCaps.maxB; }

        // ── 帧级分析：加载态 / 档位驻留（P1-1）───────────────────────────────
        // 窗口路径（measured_fps 为主源）没有逐帧事件，用窗口信号驱动同一份加载态。
        // 逐帧路径已在 readDumpsysFpsX10 里喂过，此处不重复喂（否则同一批帧计两次）。
        if (frameOn_ && !haveFrames_) {
            const float dtMs = (lastTickMs_ > 0) ? (float)(nowMs - lastTickMs_) : 0.0f;
            kin::feedLoadingWindow(loading_, (float)curFps, tgtFps_, gpuPct, dtMs, loadingParams_);
            // 驻留按窗口内真实帧数推进：60fps × 600ms ≈ 36 帧。
            // 若按"1 帧/拍"推进，120 帧驻留在 600ms 拍下要 72 秒才走完 = 降档被永久禁止。
            int framesInWindow = (int)(curFps * (double)dtMs / 1000.0 + 0.5);
            kin::gearTickFrames(gear_, gearParams_, nowMs, framesInWindow);
        }
        lastTickMs_ = nowMs;

        // 加载态：读条/过图压着跑（钳到 [perfFloor, perfCeiling]），别浪费满频
        const float loadOv = kin::loadingPerfOverride(loading_, loadingParams_);
        if (loadOv > 0.0f && hwMaxL > 0 && hwMaxB > 0) {
            const int capL = (int)(loadOv * (float)hwMaxL);
            const int capB = (int)(loadOv * (float)hwMaxB);
            if (capL < outL) outL = capL;
            if (capB < outB) outB = capB;
        }
        // 快升慢降：升档后驻留期内 + 加载后保护期内禁止降档（防读条结束瞬间的频率断崖）。
        // 注意排除 loading_ 本身 —— 加载态的上限压制**就是一次降档**，不能把它挡回去。
        const bool holdOutput = !loading_.loading &&
                                (loading_.postGuard > 0 || !kin::gearAllowsDowngrade(gear_, gearParams_));
        if (holdOutput) {
            if (lastOutL_ > 0 && outL < lastOutL_) outL = lastOutL_;
            if (lastOutB_ > 0 && outB < lastOutB_) outB = lastOutB_;
        }
        // 显著抬升才记一次"升档"（否则每个 +1kHz 噪声都会重置驻留，等价于永不降档）
        if (lastOutL_ > 0 && outL > lastOutL_ + lastOutL_ / 32) kin::gearNoteUpgrade(gear_);
        else if (lastOutB_ > 0 && outB > lastOutB_ + lastOutB_ / 32) kin::gearNoteUpgrade(gear_);
        lastOutL_ = outL; lastOutB_ = outB;

        // 自适应轮询(快追帧/省唤醒): 掉帧→加快查; 稳态明显高于目标(用稳态长窗)→放慢省电; 否则常速。
        pollMs_ = janked ? POLL_JANK_MS
                : (meanFps > (double)tgtFps_ + marginFps() ? POLL_CALM_MS : POLL_NORM_MS);

        o.engaged = true; o.maxL = outL; o.maxB = outB;
        return o;
    }

private:
    enum Src { SRC_NONE, SRC_SYSFS, SRC_DUMPSYS };

    // 把 config.json 的 FAS 帧级参数同步进来（每拍调一次，纯值类型）。
    // fas_loading_enable=false 时把累计窗口设成极大值 + 关闭 perf 覆盖，等价于退回纯 P 控制器。
    void syncFrameParams() {
        const bool en = Config::AutoMode::fas_loading_enable;
        const int pct = Config::AutoMode::fas_loading_perf_pct;
        // 关掉时：perfCeiling=0 让 loadingPerfOverride 恒返回"不覆盖"，
        // 驻留帧数=0 让 gearAllowsDowngrade 恒 true，并且**不再喂帧**（frameOn_ 门控）
        // —— 三者齐备才是真正的逐位回退；只关其中一项会让加载态照样冻结输出。
        frameOn_ = en;
        loadingParams_.perfCeiling = (float)pct / 100.0f;
        loadingParams_.perfFloor   = (float)(pct - 10) / 100.0f;
        if (loadingParams_.perfFloor < 0.0f) loadingParams_.perfFloor = 0.0f;
        gearParams_.minDwellFrames = en ? Config::AutoMode::fas_gear_dwell_frames : 0;
    }
    Logger& logger_;
    bool probed_ = false; Src src_ = SRC_NONE;
    const char* fpsPath_ = nullptr;
    bool active_ = false; long engageMs_ = 0; int warm_ = 0;
    int fasMaxL_ = 0, fasMaxB_ = 0;
    int tgtFps_ = 0, lastFpsX10_ = 0, emaFpsX10_ = 0;
    bool autoTarget_ = false; long aboveSinceMs_ = 0;   // 自学目标(fas_target_fps<=0)及"连续高于目标"计时起点(墙钟)
    long belowSinceMs_ = 0;                             // "连续低于目标(掉帧线下方)"计时起点(墙钟), 驱动降档
    int pollMs_ = POLL_NORM_MS;                          // 自适应轮询周期(随掉帧/稳态调整)
    int tgtOffX10_ = 0; long offsetMs_ = 0;             // util 驱动的目标 fps 下修(省电; 十分位, [-20,0])及采样时刻
    int pendingTgt_ = 0, pendingCnt_ = 0;               // 初始目标锁定的 2 拍确认(防加载页高帧误锁)
    long long ft_[FT_WIN] = {}; int ftN_ = 0, ftHead_ = 0;
    char fgPkg_[160] = {0};
    // util_max(单线程 CPU 比例, fas-rs v4.4 process_monitor): 前台进程最忙线程的单核占用, 单核打满=1.0
    int fgPid_ = 0;
    long thrUtilMs_ = 0; double lastThrUtil_ = -1.0;              // 采样节流(300ms) + 上次结果(节流期返回)
    struct ThrCpu { int tid; long long cpu; };                    // tid → utime+stime 快照项
    static constexpr int kMaxThreads = 256;                       // 游戏线程通常 <100, 256 兜底
    ThrCpu thrTab_[kMaxThreads] = {}; int thrN_ = 0;              // 上次采样快照(算增量)
    long dumpLastMs_ = 0; int dumpFpsX10_ = 0; char dumpLayer_[256] = {0};
    // ── 帧级分析（P1-1）────────────────────────────────────────────────────
    // 加载态（读条/过图）与档位驻留。逐帧路径走 feedLoadingFrame（--latency 免费拿到），
    // 窗口路径（只有 measured_fps）走 feedLoadingWindow。两者驱动同一份状态。
    kin::LoadingParams loadingParams_;
    kin::LoadingState  loading_;
    kin::GearParams    gearParams_;
    kin::GearState     gear_;
    bool  frameOn_     = true;               // 帧级分析总开关（config fas_loading_enable）
    bool  haveFrames_  = false;              // 本会话是否用过逐帧路径（dumpsys 兜底设备才有）
    int   lastOutL_    = 0, lastOutB_ = 0;   // 上一拍实际输出（加载后保护期"禁止降档"的基准）
    long  lastTickMs_  = 0;                  // 上一拍时刻（窗口路径折算 dt 用）
    long layerProbeMs_ = 0; bool layerSeen_ = false;   // hasGameLayer 节流缓存

    // 前台进程最忙线程的单核 CPU 时间比例(移植 fas-rs v4.4 process_monitor util_max):
    // 单核打满=1.0。节流 300ms 采样, 遍历 /proc/{pid}/task/*/stat 取 utime+stime(字段14+15, 单位 CLK_TCK=100),
    // 用 tid→上次 cputime 缓存算增量/墙钟, 取所有线程的最大比例。
    // 单线程视角能捕捉"一个大核打满"(cpuBusyW 是整机加权, 单线程打满 8 核之一只 ~12%),
    // 让 adjust_target_fps 不误判"有余量"下修目标。作为 loadPct 的【补充信号】取 max, 不替换 cpuBusyW(保留并发防劣化)。
    double readThreadUtilMax(long nowMs) {
        if (fgPid_ <= 0) { lastThrUtil_ = -1.0; return -1.0; }
        if (nowMs - thrUtilMs_ < 300) return lastThrUtil_;   // 节流 300ms
        long prevMs = thrUtilMs_; thrUtilMs_ = nowMs;
        char taskPath[48];
        snprintf(taskPath, sizeof(taskPath), "/proc/%d/task", fgPid_);
        DIR* d = opendir(taskPath);
        if (!d) { lastThrUtil_ = -1.0; return -1.0; }
        double dtS = (prevMs > 0) ? (double)(nowMs - prevMs) / 1000.0 : 0.0;
        double maxUtil = -1.0;
        ThrCpu snap[kMaxThreads]; int snapN = 0;              // 本拍快照(写回 thrTab_)
        struct dirent* e;
        while ((e = readdir(d)) != nullptr && snapN < kMaxThreads) {
            const char* name = e->d_name;
            bool num = (*name != '\0');
            for (const char* p = name; *p; ++p) if (*p < '0' || *p > '9') { num = false; break; }
            if (!num) continue;
            int tid = atoi(name);
            char statPath[80];
            snprintf(statPath, sizeof(statPath), "%s/%d/stat", taskPath, tid);
            char buf[512];
            int fd = open(statPath, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            ssize_t n = read(fd, buf, sizeof(buf) - 1); close(fd);
            if (n <= 0) continue;
            buf[n] = '\0';
            char* rp = strrchr(buf, ')');                    // comm 可能含空格/括号, 从最后一个 ')' 后数字段
            if (!rp) continue;
            ++rp;                                             // 现指向字段3(state)前
            // /proc/[pid]/stat: 1 pid 2 comm 3 state 4 ppid 5 pgrp 6 session 7 tty 8 tpgid 9 flags
            //                   10 minflt 11 cminflt 12 majflt 13 cmajflt 14 utime 15 stime
            // 从字段3起跳过 11 个(state..cmajflt)到字段14(utime)
            char* p = rp;
            for (int i = 0; i < 11; ++i) { while (*p == ' ') ++p; while (*p && *p != ' ') ++p; }
            while (*p == ' ') ++p;
            long long utime = strtoll(p, &p, 10);
            while (*p == ' ') ++p;
            long long stime = strtoll(p, nullptr, 10);
            long long cpu = utime + stime;                    // CLK_TCK 单位
            snap[snapN].tid = tid; snap[snapN].cpu = cpu; ++snapN;
            if (dtS > 0.0) {
                long long prev = 0; bool found = false;
                for (int i = 0; i < thrN_; ++i) if (thrTab_[i].tid == tid) { prev = thrTab_[i].cpu; found = true; break; }
                if (found && cpu >= prev) {
                    long long dcpu = cpu - prev;              // ticks (1 tick = 1/100s @CLK_TCK=100)
                    double util = (double)dcpu / (100.0 * dtS);  // 归一化单核: 满一个核=1.0
                    if (util > maxUtil) maxUtil = util;
                }
            }
        }
        closedir(d);
        thrN_ = snapN;
        for (int i = 0; i < thrN_; ++i) thrTab_[i] = snap[i];
        if (maxUtil < 0.0) { lastThrUtil_ = -1.0; return -1.0; }  // 首次/无增量
        if (maxUtil > 1.0) maxUtil = 1.0;                          // 多核线程(>1 核)钳到 1.0
        lastThrUtil_ = maxUtil;
        return maxUtil;
    }

    // 帧率归一化的容差(移植 fas-rs margin 缩放 target_fps/60*base):
    // 绝对帧数阈值在高刷下过敏(120fps 掉 2 帧仅 1.6%)、低帧下过松(30fps 掉 2 帧达 6.7%)。
    // 按目标帧率比例缩放后, "掉多少算掉帧""留多少余量"在各刷新率下一致(都相当于 @60fps 的 base)。
    // 钳到 [0.5×base, 3×base], 兜住 tgtFps 异常值(如未锁定/超高刷)。
    double jankGap() const {
        double g = JANK_FPS_GAP * (double)tgtFps_ / 60.0;
        double lo = JANK_FPS_GAP * 0.5, hi = JANK_FPS_GAP * 3.0;
        return g < lo ? lo : (g > hi ? hi : g);
    }
    double marginFps() const {
        double m = MARGIN_FPS * (double)tgtFps_ / 60.0;
        double lo = MARGIN_FPS * 0.5, hi = MARGIN_FPS * 3.0;
        return m < lo ? lo : (m > hi ? hi : m);
    }

    static int floorOf(int staticMax) {
        int f = (int)((double)staticMax * 0.62);
        return f > 0 ? f : 300000;
    }

    // 积分器推进: 只做 P 控制器步进 + util 上限 + GPU 瓶颈保护 + 地板/hwmax 钳制。重掉帧的一拍性顶满
    // 由调用方作用在【输出】上, 不进这里(不污染运行设定点)。
    static int applyCluster(int fasMax, double util, int delta, bool janked,
                            int gpuPct, int floor, int hwmax) {
        long sugg = (long)fasMax + delta;
        if (!janked && util >= 0.0 && fasMax > 0) {       // util 上限: cur*util/0.5
            long ceil = (long)((double)fasMax * util / UTIL_TARGET);
            if (sugg > ceil) sugg = ceil;
        }
        // GPU 瓶颈(≥95%): 此时该升的是 GPU, 提 CPU 频无益且费电 → 任何抬频都按兵不动。
        if (gpuPct >= 95 && sugg > fasMax) sugg = fasMax;
        if (hwmax > 0 && sugg > hwmax) sugg = hwmax;
        if (sugg < floor) sugg = floor;
        return (int)sugg;
    }

    static int detectTargetFps(double meanFt) {
        if (meanFt <= 0) return 0;
        double fps = 1e9 / meanFt;
        double firstFloor = TIERS[0] - 10.0; if (firstFloor < 10.0) firstFloor = 10.0;
        if (fps < firstFloor) return 0;
        for (int t : TIERS) if (fps <= (double)t + MARGIN_FPS) return t;
        return TIERS[sizeof(TIERS) / sizeof(TIERS[0]) - 1];
    }

    // 主源 sysfs: "fps: 119.8 ..." / 裸 "119.8" / "119" → fps×10
    template <class Ctl>
    int readSysfsFpsX10(Ctl& ctl) {
        char b[96];
        ssize_t n = ctl.fasPread(fpsPath_, b, sizeof(b));   // 专用 slot 16
        if (n <= 0) return -1;
        const char* p = strstr(b, "fps:"); if (p) p += 4; else p = b;
        while (*p == ' ') ++p;
        char* q = nullptr;
        long whole = strtol(p, &q, 10);
        int frac = 0;
        // 取【十分位】(首个小数字符), 而非 strtol(全部小数)%10(那是最后一位): "119.85" 应得 8 不是 5。
        if (q && *q == '.' && q[1] >= '0' && q[1] <= '9') frac = q[1] - '0';
        int x10 = (int)(whole * 10 + frac);
        return (x10 > 0 && x10 < 3000) ? x10 : -1;
    }

    template <class UtilsT>
    int readDumpsysFpsX10(UtilsT& utils, long nowMs) {
        if (nowMs - dumpLastMs_ < 1000) return dumpFpsX10_ > 0 ? dumpFpsX10_ : -1;
        dumpLastMs_ = nowMs;
        if (fgPkg_[0] == '\0') return -1;
        if (dumpLayer_[0] == '\0') pickLayer(utils);
        if (dumpLayer_[0] == '\0') return -1;
        char cmd[512];
        std::vector<char> outBuf(8192, 0);
        char* out = outBuf.data();
        // 防 shell 注入: 图层名仅保留安全字符 (A-Za-z0-9._-), 其余替换为下划线
        char safeLayer[256];
        size_t j = 0;
        for (const char* p = dumpLayer_; *p && j < sizeof(safeLayer) - 1; p++) {
            char c = *p;
            safeLayer[j++] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ? c : '_';
        }
        safeLayer[j] = '\0';
        snprintf(cmd, sizeof(cmd),
                 "dumpsys SurfaceFlinger --latency '%s' 2>/dev/null", safeLayer);
        utils.popenRead(cmd, out, outBuf.size());
        double fps = fpsFromLatencyBlock(out);
        if (fps <= 0) { dumpLayer_[0] = '\0'; dumpFpsX10_ = 0; return -1; }
        dumpFpsX10_ = (int)(fps * 10.0 + 0.5);

        // ── 逐帧喂入（P1-1）────────────────────────────────────────────────
        // 这块文本本来就已经读进来了，拆回逐帧是零额外 IO。
        // ChiRi 硬教训「帧投喂必须全量」：只喂末条会把 100ms 窗口内 6 帧丢 5 帧，
        // 所有"帧数"常数的时间尺度被拉长 ~6 倍，加载态判定与驻留全部钝化。
        kin::FrameBatch batch;
        if (frameOn_ && kin::parseLatencyFrames(out, batch, loadingParams_.heavyFrameMs)) {
            haveFrames_ = true;
            for (int i = 0; i < batch.n; ++i) {
                kin::feedLoadingFrame(loading_, batch.deltaMs[i], loadingParams_);
                kin::gearTick(gear_, gearParams_, nowMs);
            }
        }
        return dumpFpsX10_;
    }

    template <class UtilsT>
    void pickLayer(UtilsT& utils) {
        std::vector<char> outBuf(16384, 0);
        char* out = outBuf.data();
        utils.popenRead("dumpsys SurfaceFlinger --list 2>/dev/null", out, outBuf.size());
        const char* p = out;
        while (*p) {
            const char* eol = strchr(p, '\n'); size_t len = eol ? (size_t)(eol - p) : strlen(p);
            if (len > 0 && len < sizeof(dumpLayer_)) {
                char line[256]; memcpy(line, p, len); line[len] = '\0';
                if (strstr(line, fgPkg_) &&
                    (strstr(line, "BLAST") || strstr(line, "SurfaceView") || strstr(line, "TX -"))) {
                    memcpy(dumpLayer_, line, len + 1); break;
                }
            }
            if (!eol) break; p = eol + 1;
        }
    }

    // SF --latency: 行1=刷新周期; 后续每行 3 列 ns(A draw / B actualPresent / C set)。
    // 取 B 列, 去 0 与 INT64_MAX 哨兵, fps=(n-1)*1e9/(B_last-B_first)。
    static double fpsFromLatencyBlock(const char* text) {
        long long first = 0, last = 0; int cnt = 0; const char* p = text;
        while (*p && *p != '\n') ++p; if (*p == '\n') ++p;
        while (*p) {
            char* q = nullptr;
            (void)strtoll(p, &q, 10);
            if (q == p) { while (*p && *p != '\n') ++p; if (*p) ++p; continue; }
            long long bcol = strtoll(q, &q, 10);
            (void)strtoll(q, &q, 10);
            if (bcol > 0 && bcol != INT64_MAX) {
                if (!first) first = bcol; last = bcol; ++cnt;
            }
            while (*p && *p != '\n') ++p; if (*p) ++p;
        }
        if (cnt < 2 || last <= first) return -1.0;
        return (double)(cnt - 1) * 1e9 / (double)(last - first);
    }

    template <class Ctl, class UtilsT>
    int readFpsX10(Ctl& ctl, UtilsT& utils, long nowMs) {
        if (src_ == SRC_SYSFS) {
            int v = readSysfsFpsX10(ctl);
            return v > 0 ? v : -1;
        }
        return readDumpsysFpsX10(utils, nowMs);
    }
};
