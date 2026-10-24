// Kin 自动模式控制器 (mode=auto)
//
// 本文件的【学习/决策算法】移植自 AutoSystemBoost(ASB) 的 Smart Mode, 已对照其真实源码逐项核对:
//   上游: https://github.com/krilikd/AutoSystemBoost  (src/asb_smart_defs.h, src/asb_smart.h)
//   上游协议: MIT License — Copyright (c) 2026 Dima Krylov
//
// 与 ASB 真源码一致的部分: 12 桶(bucket_id = daypart*2 + weekend)、置信度公式
// (eff_obs/EFF_OBS_FULL × 7/37 天衰减)、置信门控有效缩放 eff_scale、与中性 500 混合、
// 分级回退(EXACT→DAYPART_ONLY→CLASS→GLOBAL→SAFE, 阈值 CONF_LOW=350)、会话学习方向信号
// (very_hot+80 / hot+40 / drainy+30 / sustained+20 / thermal_hit+100 / clean_cool-60)×
// 学习率 LEARN_RATE_X1000/1000、时长权×信任权仅累加进 eff_obs、夜间/低电/热否决覆盖与强制 alpha 值。
//
// 适配 Kin 的差异(非 ASB 原有): 输出离散映射到 powersave/balance/performance 三个【整模式】,
// 而非 ASB 逐 tick 的 battery↔balanced 频率插值 —— 以契合 Kin"只设上下限、governor 自主调频"理念;
// 其中 performance 档为 Kin 扩展(ASB Smart 封顶 balanced), 默认仅 cool_lean=performance 时启用。
//
// MIT(上游) → GPL-3.0(本项目 Kin) 许可证兼容; 按 MIT 要求保留上述版权与许可声明。
// 本移植实现 © 2026 Ktwo, 以 GPL-3.0 随 Kin 一并发布。
#pragma once

#include "Config.hpp"
#include "Logger.hpp"
#include "Soc.hpp"
#include "PolicyStatus.hpp"
#include "BatteryPolicy.hpp"
#include "ThermalStatus.hpp"
#include "ThermalDecision.hpp"
#include "SmartLearning.hpp"
#include <atomic>
#include <mutex>
#include <cerrno>
#include <string>
#include <utility>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <time.h>      // clock_gettime / CLOCK_MONOTONIC (POSIX)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

class AutoModeCtl {
public:
    struct Metrics {
        int  tempC    = -1;   // CPU 结温(℃), -1=未知(遥测/日志/热趋势/anti-clamp 仍用真实结温)
        int  skinC    = -1;   // 皮肤/外壳温(℃), -1=无物理外壳传感器/未知(ASB skin_temp_c)
        int  battPct  = -1;   // 电量(%), -1=未知
        bool charging = false;
        int  screenOn = 1;    // 1=亮 0=灭 -1=未知(按亮处理)
        int  gpuPct   = -1;   // GPU 占用%, -1=未知
        float load1   = 0;    // /proc/loadavg 1 分钟负载(安卓含 I/O 等待 D 态, 偏高)
        int  cpuBusy  = -1;   // CPU 真实活跃%(/proc/stat, 排除 idle+iowait), -1=未知; 用作 load 闸门
        int  cpuBusyW = -1;   // 能效加权整机负载%(uperf 式: 按 cpu_capacity×当前频率 加权); -1=未知
        // 每簇最忙核 busy%(0..100, -1=读不到)。负载闭环调频(LoadGovernor)的输入:
        // 取簇内 max 而非均值 —— 1 线程跑在 6 核簇上是「1 核忙 5 核闲」, 均值会误判成轻载。
        int  clusterBusy[4] = { -1, -1, -1, -1 };
        int  cpuPsi   = -1;   // PSI cpu some avg10(%): 任务因 CPU 争用等待的时间比; -1=无 PSI
        int  adpfHr   = -1;   // ADPF 热余量%(0=凉快 100=即将节流, App 经 getThermalHeadroom 推送); -1=无
        int  currentMa = -1;  // 电池放电电流(mA, 绝对值), -1=未知; 移植 ASB 电流佐证(真在干活才升档)
        int  powerW    = -1;  // 电池放电功率(W = currentMa × voltageMv / 1e6), -1=未知/充电; 功率驱动 boost/压制(powerd 式)
        int  hour     = 12;
        int  daypart  = 3;    // 0..5 (sleep/wake/morn/day/eve/late)
        bool weekend  = false;
        int  bucket   = 6;    // 0..11, = daypart*2 + weekend (同 ASB bucket_id)
        // 厂商压频检测(移植 ASB headroom/vendor-clamp; 仅在 msm_performance 信号可信时有效)
        int  headroom      = -1;  // 热余量%: 厂商强制频率上限/硬件上限; -1=信号不可用/死信号
        int  headroomValid = 0;   // 1=本 tick headroom 可信(已过死信号检测)
        int  softClamp     = 0;   // 软钳: headroom<soft_pct(降激进, 不算硬压频)
        int  hardClamp     = 0;   // 硬钳: headroom<hard_pct
        int  vendorClamp   = 0;   // 疑似厂商压频: 硬钳 且 温度不高(非热因 → anti-clamp 目标场景)
    };

    struct PwrSample { long ts; int level, curMa, voltMv, tempDc, chg, screen; };

    // 附加运行态：不由本类产生，而是调度层把闭环/帧级分析的状态传进来一起上报。
    // 全字段都是 bool/int —— 不引入任何外部文本，保证状态协议里不会出现注入面。
    // 默认值 = "未启用/无数据"，因此老调用点（不传 aux）行为逐位不变。
    struct AuxRuntime {
        bool loadGovEnable     = false;
        int  loadGovBusyL      = -1;   // 小核簇最忙核 busy%，-1=未接管/读不到
        int  loadGovBusyB      = -1;
        int  loadGovPerfLX1000 = -1;   // 闭环算出的小核上限（x1000 性能比），-1=未接管
        int  loadGovPerfBX1000 = -1;
        bool fasFrameAnalysis  = false;  // 帧级分析开关
        bool fasLoading        = false;  // 当前是否在读条/过图
        int  fasPostGuard      = 0;      // 加载后保护期剩余帧数
        int  fasGearDwell      = 0;      // 升档后已驻留帧数
        bool fasPerFrame       = false;  // true=逐帧(dumpsys)，false=窗口(measured_fps)
    };

    explicit AutoModeCtl(Logger& lg) : logger_(lg) {}

    // 供 AutoFas 复用缓存 fd 读 measured_fps(专用 slot 16, 单 pread, 无 open/close)。
    ssize_t fasPread(const char* path, char* buf, size_t cap) { return preadFd(16, path, buf, cap); }

    // 调度每 tick 注入当前前台包名(供 App 发热学习 / 凉爽游戏提前介入)。空/null=无。
    void setForegroundApp(const char* pkg) {
        curAppHash_ = (pkg && pkg[0] && strcmp(pkg, "null") != 0) ? fnv1a(pkg) : 0;
        curAppName_.clear();
        if (!pkg || !*pkg || strcmp(pkg, "null") == 0) return;
        // Android 包名只允许安全 token 字符；异常字符被替换为 '_'，避免污染 key=value 文件。
        for (const char* p = pkg; *p && curAppName_.size() < 160; ++p) {
            const unsigned char c = (unsigned char)*p;
            curAppName_.push_back((c == '.' || c == '_' || c == '-' ||
                                   (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9')) ? (char)c : '_');
        }
    }

    bool lowBatteryActive(bool enabled, bool charging) const {
        return enabled && !charging && batteryPolicy_.engaged();
    }
    void setForegroundCategory(int category) { curAppCategory_ = category; }
    void setForegroundSource(const char* source) {
        curFgSource_ = (source && *source) ? source : "unknown";
    }
    void setLastWriteStatus(kin::WriteStatus status, std::string nodeSummary = {},
                            std::string failedNodes = {}) {
        utilsLastWriteStatus_ = status;
        utilsLastWriteNodes_ = std::move(nodeSummary);
        utilsLastWriteFailedNodes_ = std::move(failedNodes);
    }

    // 功耗记录: 由独立线程每 ~30s 调用一次(与情景模式无关, 始终记录)。
    // 只记原始电池量(ts,电量,电流mA绝对值,电压mV,温度deci℃,充电), App 据 cellCount 自算功率/会话/曲线。
    void appendPowerSample() {
        std::lock_guard<std::recursive_mutex> lk(mtx_);   // 与 sample/accountTick 串行(pwrRing/会话/fd)
        if (!pwrLoaded_) loadPowerLog();
        PwrSample s;
        s.ts     = (long)time(nullptr);
        s.level  = readBatteryPct();
        s.curMa  = readCurrentMaRaw();
        s.voltMv = readVoltageMv();
        s.tempDc = readBatteryTempDc();
        s.chg    = readCharging() ? 1 : 0;
        s.screen = (readScreenOn() != 0) ? 1 : 0;
        pwrRing_[pwrTail_] = s;
        pwrTail_ = (pwrTail_ + 1) % PWR_N;
        if (pwrCount_ < PWR_N) pwrCount_++;
        writePowerLog();
        trackSession(s);
    }

    // 充放电会话累计: 充放电状态翻转时, 结算上一会话并落盘汇总。
    void trackSession(const PwrSample& s) {
        const long rawMw = (s.curMa > 0 && s.voltMv > 0) ? ((long)s.curMa * s.voltMv / 1000) : 0;
        if (!sessActive_ || sessType_ != s.chg) {
            if (sessActive_) finalizeSession();
            sessActive_ = true; sessType_ = s.chg;
            sessStartTs_ = s.ts; sessStartLevel_ = s.level;
            sessSumMw_ = 0; sessSumVolt_ = 0; sessCnt_ = 0;
        }
        sessSumMw_ += rawMw; sessSumVolt_ += s.voltMv; sessCnt_++;
        sessLastTs_ = s.ts; sessLastLevel_ = s.level;
    }
    void finalizeSession() {
        if (!sessActive_ || sessCnt_ <= 0) return;
        const long dur = sessLastTs_ - sessStartTs_;
        if (dur < 120) return;   // 太短(<2min)不记
        const int avgMw   = (int)(sessSumMw_ / sessCnt_);
        const int avgVolt = (int)(sessSumVolt_ / sessCnt_);
        char line[160];
        int n = snprintf(line, sizeof(line), "%d,%ld,%ld,%d,%d,%d,%d\n",
                         sessType_, sessStartTs_, sessLastTs_,
                         sessStartLevel_, sessLastLevel_, avgMw, avgVolt);
        if (n > 0) appendSessionLine(line, n);
    }
    void appendSessionLine(const char* line, int len) {
        static constexpr int SESS_MAX = 300;   // 保留最近 300 条会话
        static char old[24576];
        int oldLen = 0;
        FILE* fp = fopen(kSessionsPath, "r");
        if (fp) { oldLen = (int)fread(old, 1, sizeof(old) - 1, fp); fclose(fp); }
        if (oldLen < 0) oldLen = 0;
        old[oldLen] = '\0';
        int lines = 0; for (int i = 0; i < oldLen; i++) if (old[i] == '\n') lines++;
        int skip = (lines >= SESS_MAX) ? (lines - (SESS_MAX - 1)) : 0;
        int start = 0;
        for (int k = 0; k < skip; k++) {
            while (start < oldLen && old[start] != '\n') start++;
            if (start < oldLen) start++;
        }
        static char buf[24576 + 256];
        int off = 0, keep = oldLen - start;
        if (keep > 0) { memcpy(buf, old + start, (size_t)keep); off = keep; }
        if (off + len < (int)sizeof(buf)) { memcpy(buf + off, line, (size_t)len); off += len; }
        if (off > 0) writeFile(kSessionsPath, buf, (size_t)off);
    }

    Metrics sample() {
        std::lock_guard<std::recursive_mutex> lk(mtx_);   // 与 appendPowerSample 串行
        ensureInit();
        Metrics m;
        m.tempC    = readCpuTempC();
        m.skinC    = readSkinTempC();   // only-if-exists: 无物理外壳传感器则恒 -1
        m.battPct  = readBatteryPct();
        m.charging = readCharging();
        m.screenOn = readScreenOn();
        m.gpuPct   = readGpuBusy();
        m.load1    = readLoad1();
        m.cpuBusy  = readCpuBusy();
        m.cpuBusyW = readWeightedBusy();
        for (int i = 0; i < Config::kClusterCount; ++i) m.clusterBusy[i] = wClBusy_[i];
        m.cpuPsi   = readPsiCpu();
        m.adpfHr   = readAdpfHeadroom();
        m.currentMa = readBatteryCurrentMa();
        // 放电功率 W = currentMa × voltageMv / 1e6(mA×mV=μW, /1e6=W)。仅放电有效(充电电流混入不可用)。
        // powerd 式功率驱动: 卡顿时开系统 boost 补救响应性, 高功耗关 boost + 压 caps。
        m.powerW = -1;
        if (m.currentMa > 0 && !m.charging) {
            int vmv = readVoltageMv();
            if (vmv > 0) m.powerW = (int)(((long long)m.currentMa * vmv) / 1000000LL);
        }
        fillClampMetrics(m);   // 厂商压频/headroom 检测(信号死则自动旁路, 不影响其余逻辑)
        time_t t = time(nullptr);
        struct tm lt;
        if (localtime_r(&t, &lt)) {
            m.hour    = lt.tm_hour;
            m.daypart = daypartOf(lt.tm_hour);
            m.weekend = (lt.tm_wday == 0 || lt.tm_wday == 6);
            m.bucket  = bucketId(m.daypart, m.weekend ? 1 : 0);
        }
        return m;
    }

    // 会话记账: 每个亮屏会话累计峰温/暖比/掉电, 在亮→灭沿(或满 30min)提交学习。
    void accountTick(const Metrics& m) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);   // 与 appendPowerSample 串行(会话状态)
        processControlFile();   // 处理 reset/export 控制命令(内部限频 ~5s)
        const bool on = (m.screenOn != 0);
        const long now = (long)time(nullptr);
        nightTick(m.screenOn, now);   // 夜间时段自学(观测屏幕状态变化)
        if (on && !inSession_) beginSession(now, m);
        if (inSession_ && on) {
            sessTicks_++;
            // 会话峰温/热命中用【皮肤锚定决策温】: 否则 sessMaxTemp_ 恒取高结温 → dt_vhot(+80)误加省电(根因之一)
            const int tdS = thermalDecisionC(m);
            if (tdS > sessMaxTemp_) sessMaxTemp_ = tdS;
            if (tdS >= 0 && tdS >= Config::AutoMode::warm_temp) sessWarmTicks_++;
            if (tdS >= 0 && tdS >= Config::AutoMode::veto_temp) sessThermalHit_ = true;
            // 皮肤/外壳峰温(上游 V64 热否决加权用): 无物理外壳传感器时 skinC 恒 -1, 保持 -1。
            if (m.skinC > 0 && m.skinC > sessMaxSkinC_) sessMaxSkinC_ = m.skinC;
            if (m.gpuPct >= 65) sessGpuHeavyTicks_++;   // ASB GAMING 阈值, 计为重载 tick
            if (m.charging) sessCharged_ = true;
            if (m.currentMa > 0 && !m.charging) { sessSumCurMa_ += m.currentMa; sessCurTicks_++; }
        }
        if (inSession_ && on && (now - sessStartSec_) >= kMaxSessionSec) {
            commitSession(now, m.battPct);
            beginSession(now, m);
        }
        if (!on && inSession_) {
            commitSession(now, m.battPct);
            inSession_ = false;
        }
    }

    // 计算当前有效 alpha(0..1000, 越大越省电): 分级回退 + 置信门控 + 覆盖层, 并发布遥测。
    // 供离散档决策 decide() 与 FSM 直接调速共用(ASB Smart 的 alpha 即频率插值因子)。
    int alphaFor(const Metrics& m) {
        using namespace Config;
        const int bucket = clampi(m.bucket, 0, 11);

        // 分级回退取学习原值 alpha 与置信 conf(ASB asb_smart_lookup_bucket)
        int srcIdx = resolveBucket(bucket);
        const int alphaRaw = (srcIdx >= 0) ? alphaRaw_[srcIdx] : 500;
        const int conf      = (srcIdx >= 0) ? confOf(srcIdx)   : 0;

        // 置信门控有效缩放(ASB eff_scale)
        int effScale;
        if      (conf < ASB_CONF_LOW)  effScale = 250;
        else if (conf < ASB_CONF_HIGH) effScale = 250 + (350 * (conf - ASB_CONF_LOW)) / (ASB_CONF_HIGH - ASB_CONF_LOW);
        else                           effScale = 600 + (400 * (conf - ASB_CONF_HIGH)) / (1000 - ASB_CONF_HIGH);
        effScale = clampi(effScale, 0, 1000);
        int alpha = 500 + ((alphaRaw - 500) * effScale) / 1000;

        // Smart Battery Bias(ASB): 用户手动把 Smart 往省电推, 直接加到基线 alpha(0=关)。
        // 在覆盖层之前加, 这样温度/低电等安全覆盖仍能在其之上继续上抬。
        if (Config::AutoMode::battery_bias > 0)
            alpha = alpha + Config::AutoMode::battery_bias;

        // 探索扰动(仅本会话、仅当前桶与会话桶一致时施加): 在覆盖层之前, 安全地板仍可上抬钳回。
        if (explOffset_ != 0 && inSession_ && bucket == sessBucket_)
            alpha = alpha + explOffset_;

        // ── 覆盖层(只上抬 alpha, 安全优先; 强制值同 ASB) ──
        // 夜间安静: 用户可配窗口 [night_start, night_end)(支持跨零点, 如 23→6), 息屏且未充电时强制省电。
        // night_quiet_enable=false → 整层跳过(夜里不特殊处理)。窗口由 App 的"夜间时段"控制。
        bool nightOv = false, lowbatOv = false, vetoFlag = false;
        // 夜间窗口: 学满 3 晚后用「自学窗口」(贴合你实际作息), 否则用固定可配窗口。
        const long nowSec = (long)time(nullptr);
        const bool inNight = nightWindowReady()
            ? nightWindowActive(nowSec)
            : inNightWindow(m.hour, AutoMode::night_start, AutoMode::night_end);
        if (Config::AutoMode::night_quiet_enable && inNight &&
            m.screenOn == 0 && !m.charging) {
            alpha = maxi(alpha, ASB_NIGHT_FORCE_ALPHA); nightOv = true;
        }
        // 自动省电: 低电迟滞闩持续更新(防开关切换时状态错乱), 仅在 auto_battery_enable 时上抬 alpha。
        batteryPolicy_.update(m.battPct, m.charging, AutoMode::low_batt);
        const int batteryFloor = batteryPolicy_.alphaFloor(
            m.battPct, m.charging, AutoMode::auto_battery_enable, AutoMode::crit_batt);
        if (batteryFloor > 0) {
            alpha = maxi(alpha, batteryFloor); lowbatOv = true;
        }
        // 热否决/软抬走【皮肤锚定决策温】: 结温恒高但外壳凉时不再误抬 alpha(无皮肤 td=结温=现状)
        const int tdA = thermalDecisionC(m);
        if (tdA >= 0) {
            if (tdA > AutoMode::warm_temp && tdA < AutoMode::veto_temp) {
                int span = AutoMode::veto_temp - AutoMode::warm_temp;
                if (span > 0) alpha = alpha + (150 * (tdA - AutoMode::warm_temp)) / span;
            }
            if (tdA >= AutoMode::veto_temp) { alpha = maxi(alpha, ASB_VETO_FORCE_ALPHA); vetoFlag = true; }
        }
        // 充电感知(移植 ASB charge_aware): 充电时按 电池温度/功率/屏幕 调 alpha(护温 / 满速充 / 去省电延迟)
        alpha = applyChargeAware(alpha, m, nightOv, lowbatOv, vetoFlag);
        // 热趋势 + App 发热学习(移植 ASB thermal_trend + appheat): 升温快→提前压频(凉爽游戏)
        alpha = applyThermalTrend(alpha, m, vetoFlag);
        // 内存压力(移植 ASB memory_pressure): RAM 吃紧时高频无益, 拉向省电(仅亮屏, 息屏已深睡)
        if (m.screenOn != 0) alpha = alpha + readMemPressureShift();
        // 厂商压频软钳(移植 ASB soft_clamp 语义"降激进"): headroom 可信且软钳时略上抬 alpha(更省、安全方向),
        // 不浪费电去硬拼厂商热钳。主动"对抗/抬频"那一步(anti-clamp 攻击侧)需真机 headroom 验证后再单独启用。
        if (m.headroomValid && m.softClamp && Config::AutoMode::anticlamp_backoff > 0)
            alpha = alpha + Config::AutoMode::anticlamp_backoff;
        // 不再钳 0..1000: 学习/覆盖可越过两端(可负=比均衡更激进, >1000=比省电包络更省)。
        // 频率端由 AutoFsm::scale() 钳到 [本机最低频, 最高频], 物理边界仍然安全。

        // 遥测快照
        AutoMode::last_bucket.store(bucket);
        AutoMode::last_temp.store(m.tempC);
        AutoMode::last_batt.store(m.battPct);
        AutoMode::last_conf.store(conf);
        AutoMode::alpha[bucket].store(alpha);
        return alpha;
    }

    // ── ASB v59 皮肤锚定: 返回用于【热闸门(veto/hot/cool/SUSTAINED)】判定的决策温(℃) ──
    // 安全兜底(核心): gate 关 / 无物理外壳传感器 / 皮肤读数不在 (5,70) → 逐字返回真实结温 m.tempC(=移植前行为)。
    // 下界 5 与死传感器判据(<5)对齐: -1/0 sentinel 仍被拒; 6-20℃ 是低温环境的真实外壳读数, 不应误拒
    //(拒了会在室外/冬天退回"结温恒高误降频"的原 bug)。td=s+13 ≤33 远低于一切闸门, 不会误否决。
    // 皮肤有效(物理外壳 5<skin<70): 皮肤刻度线性对齐结温刻度(skin=skin_temp 等价结温到 veto_temp),
    //   使"结温恒高但外壳凉"不再误触发热闸门; 但结温≥junction_hard_temp 仍强制否决(真·失控兜底)。
    // 遥测/日志/热趋势/anti-clamp 仍用真实结温 m.tempC, 不走本函数。
    int thermalDecisionC(const Metrics& m) const {
        using namespace Config;
        return kin::thermalDecisionC(m.tempC, m.skinC, AutoMode::thermal_skin_gate,
            AutoMode::veto_temp, AutoMode::skin_temp, AutoMode::junction_hard_temp);
    }

    // 解析当前基础档(无 FSM 时用): powersave / balance / performance
    std::string decide(const Metrics& m) { return decide(m, alphaFor(m)); }
    // 复用已算 alpha 的重载: auto FSM tick 已算过 alpha, 直接传入避免二次调用 alphaFor(不重复喂学习)。
    std::string decide(const Metrics& m, int alpha) {
        using namespace Config;
        const int td = thermalDecisionC(m);   // 皮肤锚定决策温(无皮肤=结温=现状)
        const bool vetoFlag = (td >= 0 && td >= AutoMode::veto_temp);
        // ── alpha→离散档 (沿用 ASB CONF_LOW/HIGH=350/650 作分带) ──
        // ASB Smart 仅在 battery↔balanced 间插值(封顶 balanced); Kin 扩展: 极低 alpha 且
        // 用户显式 cool_lean=performance 时允许冲 performance。默认行为与 ASB 一致(不超 balance)。
        const bool lowBat = (m.battPct >= 0 && m.battPct <= AutoMode::low_batt);
        const bool noPerf = (td >= 0 && td >= AutoMode::hot_temp) || lowBat || vetoFlag;
        const bool cool   = (td >= 0 && td <  AutoMode::cool_temp);
        std::string base;
        if      (alpha >= ASB_CONF_HIGH) base = "powersave";
        else if (alpha >= ASB_CONF_LOW)  base = "balance";
        else {
            const bool leanPerf = strcmp(AutoMode::cool_lean.c_str(), "performance") == 0;
            base = (leanPerf && !noPerf && (m.charging || cool)) ? "performance" : "balance";
        }
        if (noPerf && base == "performance") base = "balance";
        // ── 采样融入 auto 决策(powerd 式实时过载保护) ──
        // 与学习 alpha 独立: 即便该桶学出 performance/balance, 实时功率过载也压低档位(安全优先)。
        // 仅放电有效(充电电流混入不可用)。临界(≥critical_w)→强制 powersave; 高(≥throttle_w)→performance 降 balance。
        // decide() 结果用于 SysTune 跟档(FSM 路径); 功率过载时 SysTune 也跟着降到省电档, 与 caps 叠加一致。
        if (m.powerW >= 0 && !m.charging) {
            const float pw = (float)m.powerW;
            if (pw >= AutoMode::power_throttle_critical_w) base = "powersave";
            else if (pw >= AutoMode::power_throttle_w && base == "performance") base = "balance";
        }
        return base;
    }

    // FSM 模式下的实时状态(含 FSM 状态名与本 tick 频率上限)
    // aux 无默认实参：嵌套结构的默认成员初始值在类内声明点还不可用
    // （C++ 的 incomplete-class 规则），所以由调用方显式传。
    void writeFsmStatus(const Metrics& m, int alpha, const char* fsmState, int maxL, int maxB,
                        const kin::PolicyDecision& decision, bool fasEngaged, bool romManaged,
                        const AuxRuntime& aux) {
        // 状态文件仅供 App 显示(App ~1Hz 轮询)。FAS/游戏档 tick 可达 ~4Hz, 每次都 open+write+close
        // /sdcard(FUSE/sdcardfs)开销大且白白唤醒 → 限到 ~1Hz 写, 与读端节奏对齐。
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const long nowMs = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
            if (nowMs - lastFsmStatusMs_ < 950) return;
            lastFsmStatusMs_ = nowMs;
        }
        char buf[832];
        const int bucket = clampi(m.bucket, 0, 11);
        int n = snprintf(buf, sizeof(buf),
            "resolved=fsm\nfsm=%s\nbucket=%d\ndaypart=%d\nalpha=%d\nalpha_raw=%d\nconf=%d\n"
            "cpu_max_little=%d\ncpu_max_big=%d\ntemp=%d\nbatt=%d\ngpu=%d\nload1=%.2f\n"
            "cpu_busy=%d\ncur_ma=%d\npower_w=%d\ncharging=%d\nscreen=%d\nlowbat=%d\n"
            "night_sleep=%d\nnight_wake=%d\nnight_samples=%d\n"
            "headroom=%d\nheadroom_valid=%d\nsoft_clamp=%d\nhard_clamp=%d\nvendor_clamp=%d\nvendor_clamp_1h=%d\n"
            "anticlamp_attack=%d\nanticlamp_attack_total=%ld\n"
            "explore=%d\nbase_cost=%d\nconsistency=%d\n",
            fsmState ? fsmState : "?", bucket, m.daypart, alpha,
            alphaRaw_[bucket],
            Config::AutoMode::last_conf.load(), maxL, maxB,
            m.tempC, m.battPct, m.gpuPct, m.load1,
            m.cpuBusy, m.currentMa, m.powerW, m.charging ? 1 : 0, m.screenOn, batteryPolicy_.engaged() ? 1 : 0,
            nightSleepMin_, nightWakeMin_, nightSamples_,
            m.headroom, m.headroomValid, m.softClamp, m.hardClamp, m.vendorClamp, clamp1hCount_,
            0, 0L, // Legacy attack telemetry: vendor caps are read-only.
            explDir_, baseCostMa_[bucket],
            (dtSamples_[bucket] >= 3 ? consistencyX1000(bucket) : 1000));
        if (n > 0 && (size_t)n < sizeof(buf)) {
            // 这些字段使用固定枚举值，避免把未经校验的外部文本带入状态协议。
            std::string status(buf, (size_t)n);
            status += "schema_version=3\n";
            status += "seq=" + std::to_string(++fsmStatusSeq_) + "\n";
            status += "timestamp=" + std::to_string((long long)time(nullptr)) + "\n";
            status += "owner=kin_daemon\nreason=smart_policy\nevent=steady\n";
            status += "foreground_package=" + curAppName_ + "\n";
            status += "foreground_category=" + std::to_string(curAppCategory_) + "\n";
            status += "foreground_source=" + curFgSource_ + "\n";
            status += kin::policyStatusFields(decision, fasEngaged, romManaged,
                                               utilsLastWriteStatus_, "fsm_cpu_batch",
                                               utilsLastWriteNodes_, utilsLastWriteFailedNodes_);
            status += "battery_state=";
            status += batteryPolicy_.state(m.battPct, m.charging,
                Config::AutoMode::auto_battery_enable, Config::AutoMode::crit_batt);
            status += "\nthermal_state=";
            status += kin::thermalStatus(thermalDecisionC(m), m.skinC, m.headroom,
                                         m.headroomValid, m.hardClamp);
            status += "\n";
            // ── 负载闭环（P0-2/P0-3）与 FAS 帧级分析（P1-1）的上报 ──
            // 口径：这些是**决策目标**，不是硬件写确认（见 STATUS_PROTOCOL.md）。
            // loadgov_perf_* 是闭环自己算出的该簇上限占硬件最高频的比例（x1000）；
            // 与 cpu_max_* 的差别正是要能看出来的：cpu_max_* 是最终落盘值（已过 FAS/类别/热/
            // PolicyResolver），两者不等 = 闭环意图被后层收紧了。
            status += "loadgov_enable=";
            status += aux.loadGovEnable ? "1" : "0";
            status += "\nloadgov_busy_little=" + std::to_string(aux.loadGovBusyL);
            status += "\nloadgov_busy_big=" + std::to_string(aux.loadGovBusyB);
            status += "\nloadgov_perf_little=" + std::to_string(aux.loadGovPerfLX1000);
            status += "\nloadgov_perf_big=" + std::to_string(aux.loadGovPerfBX1000);
            status += "\nfas_frame_analysis=";
            status += aux.fasFrameAnalysis ? "1" : "0";
            status += "\nfas_loading=";
            status += aux.fasLoading ? "1" : "0";
            status += "\nfas_post_guard=" + std::to_string(aux.fasPostGuard);
            status += "\nfas_gear_dwell=" + std::to_string(aux.fasGearDwell);
            status += "\nfas_frame_source=";
            status += aux.fasPerFrame ? "per_frame" : "window";
            status += "\n";
            writeFile(kStatusPath, status.c_str(), status.size());
        }
    }

    // 启动时输出只读能力摘要，供 App 区分设备不支持与运行时失败。
    // 仅探测固定节点，不执行任何写入；单项缺失不会影响调度启动。
    void writeCapabilities() {
        mkdir(kAiDir, 0755);
        std::string out;
        out += "schema_version=1\n";
        out += "timestamp=" + std::to_string((long long)time(nullptr)) + "\n";
        out += "cpu_policy_count=" + std::to_string(Config::kClusterCount) + "\n";
        for (int i = 0; i < Config::kClusterCount; ++i) {
            const int policy = Config::Policy::CpuPolicy[i];
            if (policy < 0) continue;
            char path[160];
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq", policy);
            bool ok = false;
            const long max = readRawLong(path, ok);
            out += "cpu_policy" + std::to_string(i) + "_exists=" + (ok ? "1\n" : "0\n");
            out += "cpu_policy" + std::to_string(i) + "_max_khz=" + std::to_string(ok ? max : -1) + "\n";
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_min_freq", policy);
            const long min = readRawLong(path, ok);
            out += "cpu_policy" + std::to_string(i) + "_min_khz=" + std::to_string(ok ? min : -1) + "\n";
        }
        const char* gpu = "/sys/class/kgsl/kgsl-3d0/max_gpuclk";
        out += std::string("gpu_kgsl_exists=") + (access(gpu, R_OK) == 0 ? "1\n" : "0\n");
        out += "gpu_kgsl_reason=" + std::string(access(gpu, R_OK) == 0 ? "available\n" : "node_missing\n");
        const char* mali = "/sys/class/devfreq/48000000.mali/max_freq";
        out += std::string("gpu_mali_exists=") + (access(mali, R_OK) == 0 ? "1\n" : "0\n");
        out += "gpu_mali_reason=" + std::string(access(mali, R_OK) == 0 ? "available\n" : "node_missing\n");
        out += "thermal_exists=" + std::string(access("/sys/class/thermal", R_OK) == 0 ? "1\n" : "0\n");
        out += "uclamp_exists=" + std::string(access("/proc/sys/kernel", R_OK) == 0 ? "1\n" : "0\n");
        bool fpsExists = false;
        for (const char* const* p = Soc::kMeasuredFps; *p; ++p) {
            if (access(*p, R_OK) == 0) { fpsExists = true; break; }
        }
        out += "fas_measured_fps_exists=" + std::string(fpsExists ? "1\n" : "0\n");
        writeFile(kCapabilitiesPath, out.c_str(), out.size());
    }

    // 屏幕亮/灭状态(复用 slot 15 的缓存 fd 读背光节点): 1=亮 0=灭 -1=未知。
    // 供 Schedule 的 cpuEnforceTask/bgTrimTask 复用, 取代各自的 dumpsys popen(省派生进程与唤醒)。
    // 加锁串行化首次 screenPath_ 探测与 preadFd, 跨线程(autoModeTask/cpuEnforce/bgTrim)安全。
    int screenState() {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        return readScreenOn();
    }

    // 查询当前是否处于夜间安静窗口(inNight): 供 scheduler 的 Quiet Night(ASB V61)停 tick 用。
    // 学满 3 晚用自学窗口, 否则用可配固定窗口 [night_start, night_end)。
    // 注意: 此处只判"夜间窗口命中", 不含 screenOn/charging 门控 —— 调用方(scheduler)自己加这两条件。
    bool inQuietNightWindow() {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        const long nowSec = (long)time(nullptr);
        if (nightWindowReady()) return nightWindowActive(nowSec);
        struct tm tmv;
        localtime_r(&nowSec, &tmv);
        return inNightWindow(tmv.tm_hour, Config::AutoMode::night_start, Config::AutoMode::night_end);
    }

private:
    Logger& logger_;
    // 守护内单例 autoMode_ 被两线程并发驱动: powerRecorderTask(每30s appendPowerSample) 与
    // autoModeTask(每2s sample/accountTick)。两者都经 preadFd 命中同一 cachedFd_/fdInit_ 缓存,
    // 无同步则 use-after-close/double-close/写错节点(UB)。递归锁串行化跨线程入口(嵌套 preadFd 安全)。
    std::recursive_mutex mtx_;

    // 守护内部学习文件收进隐藏子目录 .kin/, 减少 /sdcard/Android/Kin 根目录杂乱。
    // App 读的运行数据文件(auto_status/power_log/power_sessions)统一收进 AI/ 子目录(见 kAiDir)。
    static constexpr const char* kStateDir   = "/sdcard/Android/Kin/.kin";
    static constexpr const char* kStatePath  = "/sdcard/Android/Kin/.kin/auto_state.txt";
    static constexpr const char* kAiDir      = "/sdcard/Android/Kin/AI";
    static constexpr const char* kStatusPath = "/sdcard/Android/Kin/AI/auto_status.txt";   // App 读, 收进 AI/
    static constexpr const char* kCapabilitiesPath = "/sdcard/Android/Kin/AI/capabilities.txt";
    static constexpr const char* kAppheatPath = "/sdcard/Android/Kin/.kin/auto_appheat.txt";
    static constexpr const char* kNightPath   = "/sdcard/Android/Kin/.kin/auto_night.txt";
    static constexpr const char* kPowerLogPath = "/sdcard/Android/Kin/AI/power_log.txt";
    static constexpr const char* kSessionsPath = "/sdcard/Android/Kin/AI/power_sessions.txt";
    static constexpr const char* kControlPath  = "/sdcard/Android/Kin/auto_control.txt";  // 写命令: reset/reset_bucket=N/export
    static constexpr const char* kHeadroomPath = "/sdcard/Android/Kin/AI/thermal_headroom.txt";  // App 写: ADPF 热余量预测
    static constexpr const char* kExportPath   = "/sdcard/Android/Kin/auto_export.txt";   // export 输出的诊断快照
    static constexpr long kMaxSessionSec = 1800;

    // ── ASB Smart 常量(asb_smart_defs.h, MIT) ──
    static constexpr int ASB_CONF_LOW          = 350;   // CONF_LOW_X1000(亦为回退接受阈值)
    static constexpr int ASB_CONF_HIGH         = 650;   // CONF_HIGH_X1000
    static constexpr int ASB_EFF_OBS_FULL_X100 = 800;   // ~8 会话满置信
    static constexpr int ASB_OBS_CAP_X100      = 4000;  // 累计上限(代码值)
    static constexpr int ASB_DRAIN_MIN_ON_SEC  = 600;   // drainy 需亮屏≥600s
    static constexpr int DRAIN_HIGH_MA         = 1200;  // 会话平均放电电流≥此值(mA)也算 drainy(喂功耗记录入学习)
    static constexpr int ASB_NIGHT_FORCE_ALPHA = 900;
    static constexpr int ASB_VETO_FORCE_ALPHA  = 700;

    // 热趋势(asb_smart_apply_thermal_trend): 升温斜率 m℃/min, 越陡越提前压频
    static constexpr int TREND_MIN_TEMP   = 45,  TREND_MIN_SLOPE = 3000;   // 常规
    static constexpr int TREND_HOT_TEMP   = 40,  TREND_HOT_SLOPE = 2000;   // 热 App/凉爽游戏 提前
    static constexpr int TREND_CHG_TEMP   = 38,  TREND_CHG_SLOPE = 1500;   // 边充边玩 更提前
    static constexpr int TREND_MAX_SLOPE  = 12000, TREND_MAX_BUMP = 120;   // 满斜率最多 +120 alpha
    static constexpr int TREND_WINDOW_S   = 30,  TREND_STALE_S   = 180;
    // App 发热学习(asb_smart_appheat): 热 App 评分表
    static constexpr int APPHEAT_N          = 16;
    static constexpr int APPHEAT_HOT_SCORE  = 10;   // ≥10 视为热 App → 凉爽游戏提前介入
    static constexpr int APPHEAT_BUMP       = 2;    // 一次升温学习 +2
    static constexpr int APPHEAT_MAX        = 100;
    static constexpr int APPHEAT_LEARN_SLOPE= 6000; // 斜率≥6000 m℃/min 且升温→记该 App 一笔
    static constexpr int APPHEAT_DECAY_DAY  = 1;
    // 充电感知(asb_smart_apply_charge_aware): 电池温度 deci-℃, 充电功率 W
    static constexpr int CHG_HOT_DC   = 415, CHG_WARN_DC = 390, CHG_SUPER_WARN_BIAS = 10;
    static constexpr int CHG_COOL_ALPHA = 850, CHG_HOT_ALPHA = 800, CHG_ASSIST_MAX = 450;
    static constexpr int CHG_FAST_W = 12, CHG_SUPER_W = 33;
    // 夜间时段自学(asb_governor.c night_window): 学你实际睡/醒分钟数(EWMA 1/4, 跨零点环绕)
    static constexpr long NIGHT_ONSET_HOLD_S = 3600;       // 傍晚息屏持续≥1h 记为入睡
    static constexpr long NIGHT_MIN_SLEEP_S  = 3 * 3600;   // 累计黑屏≥3h 后的亮屏才算起床
    static constexpr int  NIGHT_EWMA_NUM = 1, NIGHT_EWMA_DEN = 4;
    static constexpr int  NIGHT_ONSET_FROM = 19 * 60, NIGHT_ONSET_TO = 5 * 60;   // 入睡合理窗 19:00–05:00
    static constexpr int  NIGHT_WAKE_FROM  = 4 * 60,  NIGHT_WAKE_TO  = 14 * 60;  // 起床合理窗 04:00–14:00
    static constexpr int  NIGHT_MARGIN_PRE = 15, NIGHT_MARGIN_POST = 20;
    static constexpr long NIGHT_WAKE_CONFIRM_S = 600;      // 亮屏需持续≥10min 才确认起床(防半夜瞄一眼)
    static constexpr int  NIGHT_MIN_SAMPLES = 3;           // 满 3 晚后用学到的窗口

    // 信任权重 ×100 (CLEAN/PARTIAL/NOISY/DIRTY)
    enum Trust { TW_DIRTY = 0, TW_NOISY = 15, TW_PARTIAL = 40, TW_CLEAN = 100 };

    char thermalNodes_[8][256] = {};
    int  thermalCount_  = 0;
    bool thermalProbed_ = false;
    // 皮肤/外壳温传感器: 按优先级择一【最佳且经校验】的物理 zone(非子串+max), only-if-exists。
    char skinNode_[256] = {};   // 唯一最佳 zone 的 temp 路径; 空=无有效物理外壳传感器
    bool skinProbed_    = false;
    int  skinPrio_      = 99;   // 已绑定 zone 的优先级(仅日志)
    bool stateLoaded_   = false;

    // 每桶: 学习原值 alpha / 置信累计 obs_x100 / 最近更新时刻
    int  alphaRaw_[12];
    int  obsX100_[12];
    long lastSeen_[12];
    // 每桶: 放电"代价"基线(EWMA, 单位 mA 均流; -1=未知)。供探索的奖励闭环对比"这次比平时更省还是更费"。
    int  baseCostMa_[12];
    // 每桶: 方向信号 dt 的 EWMA 均值与平均绝对偏差(均×100), 及参与会话数。衡量"该时段行为是否可复现",
    // 喂给 confOf 做方差修正 —— MAD 越大(行为越不一致)置信越低, 纠正"样本多≠可信"。
    int  dtMean100_[12];
    int  dtMad100_[12];
    int  dtSamples_[12];

    // ── 探索状态(本会话, 不落盘) ──
    int  explDir_    = 0;   // 本会话探索方向: +1=往省电探, -1=往性能探, 0=不探索
    int  explOffset_ = 0;   // 本会话施加到 alpha 的扰动量(+省电 / -性能)
    unsigned rngState_ = 0; // 轻量 xorshift32 PRNG 状态(首次用 time() 播种); 仅用于探索抽样
    long lastCtrlCheck_ = 0;// 上次检查控制文件的时刻(限频, 减少 FUSE 轮询)

    kin::BatteryPolicy batteryPolicy_;   // 低电迟滞与 alpha 安全地板
    kin::WriteStatus utilsLastWriteStatus_ = kin::WriteStatus::Unknown;
    std::string utilsLastWriteNodes_;        // 上一批逐节点计数摘要(空=无节点记录)
    std::string utilsLastWriteFailedNodes_;  // 上一批失败节点路径(空=无失败)

    // 亮屏会话累计
    bool inSession_      = false;
    long sessStartSec_   = 0;
    int  sessBattStart_  = -1;
    int  sessMaxTemp_    = -1;
    int  sessMaxSkinC_   = -1;   // 会话皮肤/外壳峰温(℃); -1=无物理外壳传感器或未采到
    int  sessWarmTicks_  = 0;
    int  sessTicks_      = 0;
    bool sessCharged_    = false;
    bool sessThermalHit_ = false;
    int  sessGpuHeavyTicks_ = 0;
    long sessSumCurMa_   = 0;   // 会话累计放电电流(mA), 供功耗记录喂入 drainy 学习
    int  sessCurTicks_   = 0;
    int  sessBucket_     = 6;
    int  lastGoodTemp_   = -1;   // 尖峰守卫基线: 上次被接受的 CPU 温度
    int  tempSpikeStreak_ = 0;   // 连续被判尖峰的次数: 连续两 tick 仍高则接受(防真实骤升被永久冻结)

    // 前台 App 哈希(供 appheat / 凉爽游戏): 由调度每 tick 注入
    unsigned long long curAppHash_ = 0;
    // 热趋势状态
    int  trendPrevC_ = 0;
    long trendPrevTs_ = 0;
    int  trendSlopeMcMin_ = 0;
    // App 发热评分表(16 条, 持久化到 auto_appheat.txt)
    struct AppHeat { unsigned long long hash; int score; long ts; };
    AppHeat appheat_[APPHEAT_N] = {};
    bool appheatLoaded_ = false;

    // 夜间时段自学状态(分钟数 0..1439, -1=未学到)
    int  nightSleepMin_ = -1, nightWakeMin_ = -1, nightSamples_ = 0;
    long nightOffSince_ = 0;     // 本次息屏起始时刻
    int  nightOffMinute_ = -1;   // 本次息屏时的分钟数(供入睡判定)
    bool nightOnsetRecorded_ = false;
    long nightDarkAccumS_ = 0;   // 本夜累计黑屏秒
    int  nightPrevScreen_ = -1;
    int  nightPendingWakeMin_ = -1;
    long nightPendingWakeTs_ = 0;

    // 功耗记录环(供首页"当前记录"卡片 + 曲线): 每 30s 一条原始电池采样
    static constexpr int PWR_N = 240;   // 2 小时 @30s
    PwrSample pwrRing_[PWR_N] = {};
    int  pwrCount_ = 0, pwrTail_ = 0;
    bool pwrLoaded_ = false;
    // 充放电会话汇总(持久化, 供"充电/放电总结"次数+平均功率)
    bool sessActive_ = false;
    int  sessType_ = -1;          // 0=放电 1=充电
    long sessStartTs_ = 0, sessLastTs_ = 0;
    int  sessStartLevel_ = -1, sessLastLevel_ = -1;
    long sessSumMw_ = 0, sessSumVolt_ = 0;   // 累加 raw mW / mV
    int  sessCnt_ = 0;

    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static int maxi(int a, int b) { return a > b ? a : b; }
    static int mini(int a, int b) { return a < b ? a : b; }

    // ── 轻量 PRNG(xorshift32): 仅用于探索抽样, 不需要密码学强度。首次用 time() 播种。 ──
    unsigned rng() {
        if (rngState_ == 0) {
            rngState_ = (unsigned)time(nullptr) ^ 0x9E3779B9u;
            if (rngState_ == 0) rngState_ = 0x1234567u;
        }
        unsigned x = rngState_;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        rngState_ = x;
        return x;
    }
    int rngPct() { return (int)(rng() % 100); }   // 0..99

    // ── 控制文件: App/用户写 /sdcard/Android/Kin/auto_control.txt 触发重置/导出 ──
    // 支持命令(每行一条, 处理后删除该文件): reset / reset_bucket=N / export
    //                                     / night_seed=<入睡分>,<起床分> / night_reset
    // 限频 ~5s 检查一次, 避免每 tick 轮询 FUSE。调用方需已持 mtx_。
    void processControlFile() {
        const long now = (long)time(nullptr);
        if (now - lastCtrlCheck_ < 5) return;
        lastCtrlCheck_ = now;
        FILE* fp = fopen(kControlPath, "r");
        if (!fp) return;
        char line[64]; bool didReset = false, didExport = false, didNight = false;
        while (fgets(line, sizeof(line), fp)) {
            int bk = -1;
            if (strncmp(line, "reset_bucket=", 13) == 0 && sscanf(line + 13, "%d", &bk) == 1
                && bk >= 0 && bk < 12) {
                alphaRaw_[bk] = 500; obsX100_[bk] = 0; lastSeen_[bk] = 0; baseCostMa_[bk] = -1;
                dtMean100_[bk] = 0; dtMad100_[bk] = 0; dtSamples_[bk] = 0;
                didReset = true;
                logger_.Info("自动模式: 控制命令 reset_bucket=%d 已重置该桶为中性", bk);
            } else if (strncmp(line, "reset", 5) == 0) {
                for (int i = 0; i < 12; i++) {
                    alphaRaw_[i] = 500; obsX100_[i] = 0; lastSeen_[i] = 0; baseCostMa_[i] = -1;
                    dtMean100_[i] = 0; dtMad100_[i] = 0; dtSamples_[i] = 0;
                }
                didReset = true;
                logger_.Info("自动模式: 控制命令 reset 已重置全部 12 桶学习为中性");
            } else if (strncmp(line, "night_seed=", 11) == 0) {
                // App 问卷"时段不准"的纠正通道: 学到的夜间窗口一旦就绪会覆盖 config 的
                // night_start/night_end, 所以纠正必须直达学习数据本身, 改 config 没用。
                int sl = -1, wk = -1;
                if (sscanf(line + 11, "%d,%d", &sl, &wk) == 2
                    && sl >= 0 && sl < 1440 && wk >= 0 && wk < 1440) {
                    nightSleepMin_ = sl; nightWakeMin_ = wk;
                    // 补足样本门槛 → 窗口立即生效; 后续 EWMA 从纠正后的锚点继续细化
                    if (nightSamples_ < NIGHT_MIN_SAMPLES) nightSamples_ = NIGHT_MIN_SAMPLES;
                    // 丢弃基于旧窗口的待确认候选/累计, 避免污染刚纠正的值
                    nightPendingWakeMin_ = -1; nightPendingWakeTs_ = 0;
                    nightDarkAccumS_ = 0; nightOnsetRecorded_ = false;
                    didNight = true;
                    logger_.Info("夜间自学: 控制命令纠正为 入睡 %02d:%02d 起床 %02d:%02d (样本=%d)",
                                 nightSleepMin_ / 60, nightSleepMin_ % 60,
                                 nightWakeMin_ / 60, nightWakeMin_ % 60, nightSamples_);
                } else {
                    logger_.Warn("夜间自学: night_seed 参数非法(需 0..1439,0..1439 两个分钟数), 已忽略");
                }
            } else if (strncmp(line, "night_reset", 11) == 0) {
                // 学错了/换作息 → 整体清空, 交回自动学习(重新满 NIGHT_MIN_SAMPLES 晚才生效)
                nightSleepMin_ = -1; nightWakeMin_ = -1; nightSamples_ = 0;
                nightPendingWakeMin_ = -1; nightPendingWakeTs_ = 0;
                nightDarkAccumS_ = 0; nightOnsetRecorded_ = false;
                didNight = true;
                logger_.Info("夜间自学: 控制命令 night_reset 已清空学习窗口, 交回自动学习");
            } else if (strncmp(line, "export", 6) == 0) {
                didExport = true;
            }
        }
        fclose(fp);
        ::remove(kControlPath);                 // 命令消费后删除, 避免重复执行
        if (didReset)  saveState();
        if (didNight)  nightWindowSave();
        if (didExport) exportDiagnostics();
    }

    // 导出人类可读的学习诊断快照到 auto_export.txt(供"可证伪/可观测": 每桶 alpha/conf/基线一目了然)。
    void exportDiagnostics() {
        char buf[1024]; int off = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "# Kin 学习诊断 (bucket=daypart*2+weekend; alpha 越大越省电; consist=一致性×1000, 越高越可信)\n"
                        "# bucket alpha_raw conf consist obs_x100 base_cost_ma dt_samples last_seen\n");
        for (int i = 0; i < 12 && off < (int)sizeof(buf); i++) {
            off += snprintf(buf + off, sizeof(buf) - off, "b%d %d %d %d %d %d %d %ld\n",
                            i, alphaRaw_[i], confOf(i),
                            (dtSamples_[i] >= 3 ? consistencyX1000(i) : 1000),
                            obsX100_[i], baseCostMa_[i], dtSamples_[i], lastSeen_[i]);
        }
        if (off > 0) writeFile(kExportPath, buf, (size_t)off);
        logger_.Info("自动模式: 控制命令 export 已写出学习诊断 %s", kExportPath);
    }

    // 夜间窗口判定: 命中 [start, end) 小时返回 true。start>end 视为跨零点(如 23→6);
    // start==end 视为关闭(全天 false)。hour<0(未知)按不命中处理。
    static bool inNightWindow(int hour, int start, int end) {
        if (hour < 0) return false;
        start = clampi(start, 0, 23); end = clampi(end, 0, 23);
        if (start == end) return false;
        if (start < end)  return hour >= start && hour < end;
        return hour >= start || hour < end;
    }

    // 建隐藏子目录 + 一次性把旧的根目录学习文件迁进去(rename 不丢学习; 已迁移则 no-op)。
    void ensureStateDir() {
        if (mkdir(kStateDir, 0755) != 0 && errno != EEXIST) {
            // .kin 被占成普通文件(ENOTDIR)→ 删掉重建一次; 其余失败仅告警, 学习将退回不落盘(不致命)。
            if (errno == ENOTDIR || errno == ENOENT) { ::remove(kStateDir); mkdir(kStateDir, 0755); }
            if (access(kStateDir, W_OK) != 0)
                logger_.Warn("自动模式: 学习目录 %s 不可用(errno=%d), 本次学习不落盘", kStateDir, errno);
        }
        rename("/sdcard/Android/Kin/auto_state.txt",   kStatePath);
        rename("/sdcard/Android/Kin/auto_appheat.txt", kAppheatPath);
        rename("/sdcard/Android/Kin/auto_night.txt",   kNightPath);
    }
    void ensureInit() { if (!stateLoaded_) { ensureStateDir(); loadState(); } }

    // 时段(ASB 真实边界): 睡0-6 醒6-9 上午9-12 白天12-17 傍晚17-21 深夜21-24
    static int daypartOf(int hour) {
        if      (hour <  6) return 0;
        else if (hour <  9) return 1;
        else if (hour < 12) return 2;
        else if (hour < 17) return 3;
        else if (hour < 21) return 4;
        else                return 5;
    }
    // ASB: bucket_id = daypart*2 + is_weekend; 反周末 = bucket ^ 1
    static int bucketId(int daypart, int weekend) { return daypart * 2 + (weekend ? 1 : 0); }
    static int daypartOfBucket(int bucket) { return bucket / 2; }
    // 时段类(ASB daypart_class): {睡,深夜}=0(夜) {醒,上午,白天}=1 {傍晚}=2
    static int classOf(int daypart) {
        if (daypart == 0 || daypart == 5) return 0;
        if (daypart == 1 || daypart == 2 || daypart == 3) return 1;
        return 2;
    }

    // ── 置信度(ASB asb_smart_confidence_x1000) ──
    int confOf(int i) {
        if (i < 0 || i >= 12 || obsX100_[i] <= 0) return 0;
        long ratio = (long)obsX100_[i] * 1000 / ASB_EFF_OBS_FULL_X100;
        if (ratio > 1000) ratio = 1000;
        int decay = decayX100(lastSeen_[i]);
        long conf = ratio * decay / 100;
        // 方差修正(批2): 桶内 dt 波动越大→置信越低。warmup(≥3 会话)后才生效, 之前=纯样本计数(原行为)。
        if (Config::AutoMode::conf_var_enable && dtSamples_[i] >= 3)
            conf = conf * consistencyX1000(i) / 1000;
        return conf > 1000 ? 1000 : (int)conf;
    }
    // 一致性因子(×1000): MAD=0→1000(完全可复现); MAD=mad_scale→500; 越大越低。把"样本多≠可信"纠过来。
    int consistencyX1000(int i) const {
        const int mad = (dtMad100_[i] > 0) ? dtMad100_[i] / 100 : 0;   // 回到 dt 单位
        const int scale = clampi(Config::AutoMode::mad_scale, 1, 500);
        return (int)(1000L * scale / (scale + mad));
    }
    static int decayX100(long lastSeen) {
        if (lastSeen <= 0) return 100;
        long now = (long)time(nullptr);
        if (now <= lastSeen) return 100;
        long days = (now - lastSeen) / 86400;
        if (days < 7)  return 100;
        if (days > 37) return 0;                       // 仅【超过】端点才归 0; 原 >=37 在端点漏掉 30 → 突跳 31→0
        return 100 - (int)((70 * (days - 7)) / 30);   // 7→100 线性到 37→30(端点含)
    }

    // 分级回退(ASB asb_smart_lookup_bucket): EXACT→DAYPART_ONLY→CLASS→GLOBAL→SAFE(-1)
    int resolveBucket(int bucket) {
        const int cx = confOf(bucket);
        if (cx >= ASB_CONF_LOW) return bucket;               // L0 EXACT
        const int alt = bucket ^ 1;                          // 反周末同时段
        const int ca = confOf(alt);
        if (ca >= ASB_CONF_LOW && ca > cx) return alt;       // L1 DAYPART_ONLY
        if (cx > 0 && cx >= ca) return bucket;               // 弱 EXACT
        const int cls = classOf(daypartOfBucket(bucket));    // L2 CLASS
        int best = -1, bestc = -1;
        for (int i = 0; i < 12; i++) {
            if (classOf(daypartOfBucket(i)) != cls) continue;
            int c = confOf(i);
            if (c > bestc) { bestc = c; best = i; }
        }
        if (best >= 0 && bestc >= ASB_CONF_LOW) return best;
        best = -1; bestc = -1;                               // L3 GLOBAL
        for (int i = 0; i < 12; i++) {
            int c = confOf(i);
            if (c > bestc) { bestc = c; best = i; }
        }
        if (best >= 0 && bestc >= ASB_CONF_LOW) return best;
        return -1;                                           // L4 SAFE → 中性 500
    }

    // ── 会话学习(ASB asb_smart_bucket_update_from_session) ──
    void beginSession(long now, const Metrics& m) {
        inSession_      = true;
        sessStartSec_   = now;
        sessBattStart_  = m.battPct;
        // 播种也走皮肤锚定决策温: 若用 m.tempC(SM8850 结温恒 85-95)播种, 皮肤 td(~50)永远压不过初值,
        // dt_vhot(+80)每会话误加、cleanCool(-60 唯一负向项)永不可达 → 学习单向漂向省电(审查确证的回归)。
        sessMaxTemp_    = thermalDecisionC(m);
        sessMaxSkinC_   = (m.skinC > 0) ? m.skinC : -1;
        sessWarmTicks_  = 0;
        sessTicks_      = 0;
        sessCharged_    = false;
        sessThermalHit_ = false;
        sessGpuHeavyTicks_ = 0;
        sessSumCurMa_   = 0;
        sessCurTicks_   = 0;
        sessBucket_     = clampi(m.bucket, 0, 11);

        // ── ε-greedy 探索决策 ── 仅在已过门控(有基线)的桶里, 以 explore_pct% 概率给本会话一个小扰动。
        // 方向偏向"性能侧"(系统结构性偏保守、性能侧无学习动力), 但仅在不低电/不偏热/不充电时才敢往性能探;
        // 否则退而往省电侧探(永远安全)。扰动只改基础 alpha, 覆盖层的温控/低电/夜间地板仍会钳回 → 不破坏安全。
        explDir_ = 0; explOffset_ = 0;
        if (Config::AutoMode::explore_enable && Config::AutoMode::explore_pct > 0 &&
            Config::AutoMode::explore_offset > 0 && confOf(sessBucket_) >= ASB_CONF_LOW) {
            if (rngPct() < Config::AutoMode::explore_pct) {
                const bool perfSafe =
                    (m.battPct < 0 || m.battPct > Config::AutoMode::low_batt) &&
                    (m.tempC   < 0 || m.tempC   < Config::AutoMode::warm_temp) && !m.charging;
                if (perfSafe && (rng() % 3 != 0)) { explDir_ = -1; explOffset_ = -Config::AutoMode::explore_offset; }
                else                              { explDir_ = +1; explOffset_ = +Config::AutoMode::explore_offset; }
                logger_.Debug("自动模式: 桶%d 进入探索 dir=%d offset=%d", sessBucket_, explDir_, explOffset_);
            }
        }
    }

    static int durWeightX100(long dur) {
        if (dur < 600)  return 25;
        if (dur < 1800) return 50;
        if (dur < 5400) return 100;
        return 125;
    }

    void commitSession(long nowSec, int battEnd) {
        const long dur = nowSec - sessStartSec_;
        // 信任分级(近似 ASB caller-side): 夜间时段=NOISY; 凉爽采样足=CLEAN; 充电/过短=DIRTY; 余=PARTIAL
        const int dp = daypartOfBucket(clampi(sessBucket_, 0, 11));
        int trust;
        if (dur < 60 || sessTicks_ < 3 || sessCharged_)                 trust = TW_DIRTY;
        else if (dp == 0 || dp == 5)                                    trust = TW_NOISY;
        else if (sessMaxTemp_ >= 0 && sessMaxTemp_ < Config::AutoMode::cool_temp && sessTicks_ >= 5)
                                                                        trust = TW_CLEAN;
        else                                                            trust = TW_PARTIAL;
        if (trust == TW_DIRTY) {   // ASB: 脏会话整体丢弃
            logger_.Debug("自动模式: 会话脏数据(时长=%lds tick=%d 充电=%d), 不学习",
                          dur, sessTicks_, sessCharged_ ? 1 : 0);
            return;
        }
        const int bucket = clampi(sessBucket_, 0, 11);

        // eff_obs += (时长权 × 信任权)/100, 封顶 4000(仅影响置信度, 不影响 alpha 步长)
        const int durW = durWeightX100(dur);
        const int effAdd = durW * trust / 100;
        if (effAdd <= 0) return;
        // eff_obs 滑窗(批2): 先按 obs_retain_pct 衰减旧值再累加, 取代原"硬封顶后永驻"——
        // 近期会话权重更高, 也不会再卡在 4000 一动不动。CAP 仍作安全上界(滑窗下通常达不到)。
        const int retain = clampi(Config::AutoMode::obs_retain_pct, 50, 100);
        const long decayed = (long)obsX100_[bucket] * retain / 100;
        obsX100_[bucket] = (int)mini(ASB_OBS_CAP_X100, decayed + effAdd);
        lastSeen_[bucket] = nowSec;

        // 掉电率(%/h); drainy 需亮屏≥600s(ASB DRAIN_MIN_ON_SEC)
        int drainRate = -1;
        if (sessBattStart_ >= 0 && battEnd >= 0 && sessBattStart_ >= battEnd)
            drainRate = (int)((long)(sessBattStart_ - battEnd) * 3600 / (dur > 0 ? dur : 1));
        // 喂功耗记录入学习: 除了粗粒度的掉电率%/h, 实测会话平均放电电流(mA)高也算 drainy ——
        // 大电池上 1% 跨度大、%/h 反应迟钝, 用电流能更快学到"这个时段确实在猛耗电"。
        const int avgCurMa = (sessCurTicks_ > 0) ? (int)(sessSumCurMa_ / sessCurTicks_) : -1;
        const bool drainy = (dur >= ASB_DRAIN_MIN_ON_SEC) &&
                            (drainRate >= Config::AutoMode::drain_high || avgCurMa >= DRAIN_HIGH_MA);
        // ASB sustained_heavy = SUSTAINED 时间≥30%; 这里用 GPU 重载多数(移植自 metrics GPU 读)
        // 或暖 tick 多数 近似 —— 二者任一即视为持续重载。
        const bool sustainedHeavy = (sessGpuHeavyTicks_ * 2 >= sessTicks_) ||
                                    (sessWarmTicks_ * 2 >= sessTicks_);
        const bool cleanCool = (sessMaxTemp_ >= 0 && sessMaxTemp_ < Config::AutoMode::cool_temp
                                && trust == TW_CLEAN);

        // 方向信号(系数默认 = ASB 真源码值, 现可由配置覆盖): 不按时长/信任加权。
        // 纯函数在 include/SmartLearning.hpp: 学习方向一旦反了就是"越热越放开", 必须可单测。
        kin::SmartSessionSignals sig;
        sig.maxTempC       = sessMaxTemp_;
        sig.maxSkinC       = sessMaxSkinC_;
        sig.thermalHit     = sessThermalHit_;
        sig.drainy         = drainy;
        sig.sustainedHeavy = sustainedHeavy;
        sig.cleanCool      = cleanCool;
        kin::SmartLearnWeights w;
        w.hot          = Config::AutoMode::dt_hot;
        w.vhot         = Config::AutoMode::dt_vhot;
        w.drainy       = Config::AutoMode::dt_drainy;
        w.sustained    = Config::AutoMode::dt_sustained;
        w.thermalHit   = Config::AutoMode::dt_thermal_hit;
        w.thermalHitSkinCool = Config::AutoMode::dt_thermal_hit_skin_cool;
        w.cleanCool    = Config::AutoMode::dt_clean_cool;
        w.hotTempC     = Config::AutoMode::hot_temp;
        w.vhotTempC    = Config::AutoMode::vhot_temp;
        const int dt = kin::smartDtAlpha(sig, w);

        // 桶内一致性(批2): 更新该桶 dt 的 EWMA 均值与平均绝对偏差(均×100 定点, a=1/8)。
        // MAD 大=这个时段忽冷忽热/忽闲忽忙→行为不可复现→confOf 会据此压低该桶置信。
        {
            const int dt100 = dt * 100;
            if (dtSamples_[bucket] == 0) { dtMean100_[bucket] = dt100; dtMad100_[bucket] = 0; }
            else {
                const int diff  = dt100 - dtMean100_[bucket];
                dtMean100_[bucket] += diff / 8;
                const int adiff = diff < 0 ? -diff : diff;
                dtMad100_[bucket]  += (adiff - dtMad100_[bucket]) / 8;
            }
            if (dtSamples_[bucket] < 1000000) dtSamples_[bucket]++;
        }

        // 单步 = dt × 学习率/1000(ASB: dt_alpha×LEARN_RATE_X1000/1000), 不再额外封顶
        const int lr = clampi(Config::AutoMode::learn_rate, 1, 1000);
        const int move = (dt * lr) / 1000;

        // ── 探索奖励闭环 + 代价基线 ──
        // 代价 = 本会话亮屏均放电电流(mA)。仅可比会话(非充电 / trust≥PARTIAL / 有有效均流)才更新基线、算奖励。
        // 给系统补上它原本缺的"实测反馈": 探索往省电探→真省到了才奖励, 没省到反而扣(白掉性能);
        // 探索往性能探→电没多耗也不发热(性能近乎白送)才奖励, 否则扣。这也补上了原先缺失的性能侧学习动力。
        int rewMove = 0;
        const bool comparable = (!sessCharged_ && trust >= TW_PARTIAL && avgCurMa > 0);
        if (comparable) {
            const int base = baseCostMa_[bucket];
            if (explDir_ != 0 && base > 0) {
                const int thr   = maxi(base / 20, 30);   // "有意义"的差异: 基线 5% 或 30mA
                const int delta = avgCurMa - base;       // <0 = 比平时更省电(好)
                const bool thermalBad = sessThermalHit_ ||
                    (sessMaxTemp_ >= 0 && sessMaxTemp_ >= Config::AutoMode::hot_temp);
                const int step = clampi(Config::AutoMode::reward_step, 0, 50);
                if (explDir_ > 0) rewMove = (delta <= -thr && !thermalBad) ? +step : -step;
                else              rewMove = (delta <=  thr && !thermalBad) ? -step : +step;
            }
            // 基线 EWMA(同代码其它 EWMA: 旧×3/4 + 新×1/4; 首次直接采用本次)
            baseCostMa_[bucket] = (base > 0) ? (base * 3 + avgCurMa) / 4 : avgCurMa;
        }

        // 学习累积不封顶: 证据可以继续累积(可负/可超 1000), 不在 0/1000 处饱和丢失。
        const int a  = alphaRaw_[bucket];
        const int na = a + move + rewMove;
        alphaRaw_[bucket] = na;
        const int explLog = explDir_;
        explDir_ = 0; explOffset_ = 0;   // 探索仅作用于本会话, 结算后清除
        saveState();

        logger_.Info("自动模式学习: 桶%d alpha %d→%d (dt=%d lr=%d 探索=%d 奖励=%d 均流=%dmA 基线=%dmA 时长权=%d 信任=%d 峰温=%d 热击=%d 掉电=%d%%/h obs=%d conf=%d)",
                     bucket, a, na, dt, lr, explLog, rewMove, avgCurMa, baseCostMa_[bucket],
                     durW, trust, sessMaxTemp_, sessThermalHit_ ? 1 : 0,
                     drainRate, obsX100_[bucket], confOf(bucket));
    }

    // ── 充电感知(移植 ASB asb_smart_apply_charge_aware) ──
    // 充电时: 电池烫→省电护温; 息屏→满速充压性能; 亮屏且凉→朝均衡放(去掉省电带来的延迟)。
    int applyChargeAware(int alpha, const Metrics& m, bool nightOv, bool lowbatOv, bool vetoFlag) {
        if (!m.charging) return alpha;
        const int battDc = readBatteryTempDc();
        if (battDc <= 0) return alpha;
        int warn = CHG_WARN_DC;
        if (chargePowerW(m.currentMa) >= CHG_SUPER_W) warn -= CHG_SUPER_WARN_BIAS;
        if (battDc >= CHG_HOT_DC)   return maxi(alpha, CHG_HOT_ALPHA);   // 充电+电池烫 → 护温
        if (m.screenOn == 0)        return maxi(alpha, CHG_COOL_ALPHA);  // 充电+息屏 → 满速充
        if (battDc < warn && !nightOv && !lowbatOv && !vetoFlag)
            return mini(alpha, CHG_ASSIST_MAX);                          // 充电+亮屏+凉 → 朝均衡(封顶)
        return alpha;
    }
    int chargePowerW(int ma) {
        if (ma <= 0) return 0;
        bool ok; long uv = readRawLong("/sys/class/power_supply/battery/voltage_now", ok);
        if (!ok || uv <= 0) return 0;
        return (int)(((long long)ma * uv) / 1000000000LL);   // W = mA/1e3 × µV/1e6
    }
    int readBatteryTempDc() {   // 电池温度 deci-℃(如 320=32.0℃)
        bool ok; long v = readRawLong("/sys/class/power_supply/battery/temp", ok);
        return ok ? (int)v : -1;
    }

    // ── 热趋势 + App 发热学习(移植 ASB asb_smart_apply_thermal_trend + appheat) ──
    int applyThermalTrend(int alpha, const Metrics& m, bool vetoFlag) {
        const int c = m.tempC;
        const long now = (long)time(nullptr);
        if (c <= 0) { trendPrevTs_ = 0; trendSlopeMcMin_ = 0; return alpha; }
        if (trendPrevTs_ == 0 || (now - trendPrevTs_) > TREND_STALE_S || now < trendPrevTs_) {
            trendPrevC_ = c; trendPrevTs_ = now; trendSlopeMcMin_ = 0; return alpha;
        }
        const long dt = now - trendPrevTs_;
        if (dt >= TREND_WINDOW_S) {
            long raw = ((long)(c - trendPrevC_) * 60000L) / dt;   // m℃/min
            if (raw > 60000L) raw = 60000L; else if (raw < -60000L) raw = -60000L;
            trendSlopeMcMin_ = (trendSlopeMcMin_ + (int)raw) / 2;  // EMA 半衰
            trendPrevC_ = c; trendPrevTs_ = now;
            if (trendSlopeMcMin_ >= APPHEAT_LEARN_SLOPE && c >= TREND_HOT_TEMP)
                appheatBump(curAppHash_, now);   // 学习: 该前台 App 让机子明显升温
        }
        if (vetoFlag) return alpha;   // 已被高温否决拉满, 不再叠加(但学习照做)
        int hotApp = (appheatScore(curAppHash_, now) >= APPHEAT_HOT_SCORE) ? 1 : 0;
        if (m.charging) hotApp = 2;   // 边充边玩 → 更早介入
        const int bump = trendBumpCalc(c, trendSlopeMcMin_, hotApp);
        if (bump > 0) alpha = alpha + bump;
        return alpha;
    }
    static int trendBumpCalc(int c, int slope, int hotApp) {
        int minT, minS;
        if      (hotApp >= 2) { minT = TREND_CHG_TEMP; minS = TREND_CHG_SLOPE; }
        else if (hotApp == 1) { minT = TREND_HOT_TEMP; minS = TREND_HOT_SLOPE; }
        else                  { minT = TREND_MIN_TEMP; minS = TREND_MIN_SLOPE; }
        if (c < minT || slope <= minS) return 0;
        const int span = TREND_MAX_SLOPE - minS;
        if (span <= 0) return 0;
        int over = slope - minS; if (over > span) over = span;
        return (TREND_MAX_BUMP * over) / span;
    }

    // ── App 发热评分表(asb_smart_appheat; 持久化到 auto_appheat.txt) ──
    void appheatEnsure() { if (!appheatLoaded_) appheatLoad(); }
    AppHeat* appheatFind(unsigned long long h) {
        if (!h) return nullptr;
        for (auto& e : appheat_) if (e.hash == h) return &e;
        return nullptr;
    }
    static void appheatDecay(AppHeat* e, long now) {
        if (!e || e->ts == 0) return;
        const long days = (now - e->ts) / 86400L;
        if (days <= 0) return;
        const long dec = days * APPHEAT_DECAY_DAY;
        e->score = (dec >= e->score) ? 0 : (int)(e->score - dec);
    }
    int appheatScore(unsigned long long h, long now) {
        appheatEnsure();
        AppHeat* e = appheatFind(h);
        if (!e) return 0;
        appheatDecay(e, now);
        return e->score;
    }
    void appheatBump(unsigned long long h, long now) {
        if (!h) return;
        appheatEnsure();
        AppHeat* e = appheatFind(h);
        if (!e) {   // 取空位, 否则淘汰最久未见
            e = &appheat_[0];
            for (auto& c : appheat_) { if (c.hash == 0) { e = &c; break; } if (c.ts < e->ts) e = &c; }
            e->hash = h; e->score = 0; e->ts = now;
        }
        appheatDecay(e, now);
        e->score = mini(APPHEAT_MAX, e->score + APPHEAT_BUMP);
        e->ts = now;
        appheatSave();
    }
    void appheatLoad() {
        appheatLoaded_ = true;
        for (auto& e : appheat_) { e.hash = 0; e.score = 0; e.ts = 0; }
        FILE* fp = fopen(kAppheatPath, "r");
        if (!fp) return;
        char line[128]; int i = 0;
        while (fgets(line, sizeof(line), fp) && i < APPHEAT_N) {
            unsigned long long h = 0; int sc = 0; long ts = 0;
            if (sscanf(line, "%llu,%d,%ld", &h, &sc, &ts) == 3 && h != 0) {
                bool dup = false;   // 跳过重复 hash(防手改/损坏文件浪费槽位、score 不一致)
                for (int j = 0; j < i; j++) if (appheat_[j].hash == h) { dup = true; break; }
                if (dup) continue;
                appheat_[i].hash = h; appheat_[i].score = clampi(sc, 0, APPHEAT_MAX);
                appheat_[i].ts = ts; i++;
            }
        }
        fclose(fp);
    }
    void appheatSave() {
        char buf[2048]; int off = 0;
        for (auto& e : appheat_) {
            if (e.hash == 0 || off >= (int)sizeof(buf)) continue;
            off += snprintf(buf + off, sizeof(buf) - off, "%llu,%d,%ld\n",
                            (unsigned long long)e.hash, e.score, e.ts);
        }
        if (off > 0) writeFile(kAppheatPath, buf, (size_t)off);
    }

    // 内存压力位移(asb_smart_memory_pressure_shift): some.avg10 ≥20→+100, ≥5→+50
    static int readMemPressureShift() {
        int fd = open("/proc/pressure/memory", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        char b[256]; ssize_t n = read(fd, b, sizeof(b) - 1); close(fd);
        if (n <= 0) return 0; b[n] = '\0';
        char* p = strstr(b, "some ");
        if (!p) return 0;
        p = strstr(p, "avg10=");
        if (!p) return 0;
        const int whole = atoi(p + 6);
        if (whole >= 20) return 100;
        if (whole >= 5)  return 50;
        return 0;
    }

    static unsigned long long fnv1a(const char* s) {
        unsigned long long h = 1469598103934665603ULL;
        for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
        return h;
    }

    // ── 夜间时段自学(移植 ASB asb_governor.c night_window) ──
    static int minuteOfDay(long now) {
        time_t t = (time_t)now; struct tm lt;
        if (!localtime_r(&t, &lt)) return -1;
        return lt.tm_hour * 60 + lt.tm_min;
    }
    static bool minInWindow(int m, int from, int to) {
        if (m < 0) return false;
        return (from > to) ? (m >= from || m < to) : (m >= from && m < to);
    }
    static void nightEwma(int& cur, int sample) {   // 跨零点环绕的 EWMA
        if (cur < 0) { cur = sample; return; }
        int d = ((sample - cur) % 1440 + 1440) % 1440;
        if (d > 720) d -= 1440;
        cur = ((cur + (d * NIGHT_EWMA_NUM) / NIGHT_EWMA_DEN) + 1440) % 1440;
    }
    bool nightWindowReady() const {
        return nightSamples_ >= NIGHT_MIN_SAMPLES && nightSleepMin_ >= 0 && nightWakeMin_ >= 0;
    }
    bool nightWindowActive(long now) const {
        if (!nightWindowReady()) return false;
        const int m = minuteOfDay(now);
        if (m < 0) return false;
        const int from = (nightSleepMin_ - NIGHT_MARGIN_PRE  + 1440) % 1440;
        const int to   = (nightWakeMin_  - NIGHT_MARGIN_POST + 1440) % 1440;
        return minInWindow(m, from, to);
    }
    // 每 tick 观测屏幕状态变化, 学习入睡/起床分钟数。
    void nightTick(int screenOn, long now) {
        if (!Config::AutoMode::night_quiet_enable) return;
        const int on = (screenOn != 0) ? 1 : 0;
        if (nightPrevScreen_ < 0) {           // 首次
            nightPrevScreen_ = on;
            if (!on) { nightOffSince_ = now; nightOffMinute_ = minuteOfDay(now); }
            return;
        }
        if (on != nightPrevScreen_) {         // 屏幕状态翻转
            if (!on) {                        // 亮→灭
                long pend = (nightPendingWakeTs_ > 0) ? (now - nightPendingWakeTs_) : -1;
                if (pend >= 0 && pend < NIGHT_WAKE_CONFIRM_S) {   // 刚才只是半夜瞄一眼, 取消起床候选
                    nightPendingWakeMin_ = -1; nightPendingWakeTs_ = 0;
                }
                nightOffSince_ = now; nightOffMinute_ = minuteOfDay(now);
                nightOnsetRecorded_ = false;
            } else {                          // 灭→亮
                long offDur = (nightOffSince_ > 0) ? (now - nightOffSince_) : 0;
                if (offDur > 0) nightDarkAccumS_ += offDur;
                if (nightDarkAccumS_ >= NIGHT_MIN_SLEEP_S) {
                    const int m = minuteOfDay(now);
                    if (minInWindow(m, NIGHT_WAKE_FROM, NIGHT_WAKE_TO)) {
                        nightPendingWakeMin_ = m; nightPendingWakeTs_ = now;   // 候选起床, 待确认
                    } else {
                        nightDarkAccumS_ = 0;   // 醒在不合理时段(小睡/出行)→ 丢弃, 不污染
                    }
                }
                nightOffSince_ = 0;
            }
            nightPrevScreen_ = on;
            return;
        }
        if (on) {   // 持续亮屏: 候选起床撑过确认时长 → 学习
            if (nightPendingWakeMin_ >= 0 && now - nightPendingWakeTs_ >= NIGHT_WAKE_CONFIRM_S) {
                nightEwma(nightWakeMin_, nightPendingWakeMin_);
                if (nightSamples_ < 100000) nightSamples_++;
                nightPendingWakeMin_ = -1; nightPendingWakeTs_ = 0; nightDarkAccumS_ = 0;
                logger_.Info("夜间自学: 起床 → %02d:%02d (样本=%d)",
                             nightWakeMin_ / 60, nightWakeMin_ % 60, nightSamples_);
                nightWindowSave();
            }
            return;
        }
        // 持续息屏: 傍晚息屏撑过保持时长且在合理窗 → 记入睡
        if (!nightOnsetRecorded_ && nightOffSince_ > 0 &&
            nightDarkAccumS_ < NIGHT_MIN_SLEEP_S &&
            now - nightOffSince_ >= NIGHT_ONSET_HOLD_S &&
            minInWindow(nightOffMinute_, NIGHT_ONSET_FROM, NIGHT_ONSET_TO)) {
            nightEwma(nightSleepMin_, nightOffMinute_);
            nightOnsetRecorded_ = true;
            logger_.Info("夜间自学: 入睡 → %02d:%02d", nightSleepMin_ / 60, nightSleepMin_ % 60);
            nightWindowSave();
        }
    }
    void nightWindowLoad() {
        FILE* fp = fopen(kNightPath, "r");
        if (!fp) return;
        char line[64]; int v;
        while (fgets(line, sizeof(line), fp)) {
            if      (sscanf(line, "sleep_min=%d", &v) == 1 && v >= -1 && v < 1440) nightSleepMin_ = v;
            else if (sscanf(line, "wake_min=%d",  &v) == 1 && v >= -1 && v < 1440) nightWakeMin_  = v;
            else if (sscanf(line, "samples=%d",   &v) == 1 && v >= 0  && v <= 100000) nightSamples_ = v;
        }
        fclose(fp);
        if (nightSleepMin_ >= 0 && nightWakeMin_ >= 0)
            logger_.Info("夜间自学: 已载入 %02d:%02d→%02d:%02d (样本=%d)",
                         nightSleepMin_ / 60, nightSleepMin_ % 60,
                         nightWakeMin_ / 60, nightWakeMin_ % 60, nightSamples_);
    }
    void nightWindowSave() {
        char buf[96];
        int n = snprintf(buf, sizeof(buf), "sleep_min=%d\nwake_min=%d\nsamples=%d\n",
                         nightSleepMin_, nightWakeMin_, nightSamples_);
        if (n > 0) writeFile(kNightPath, buf, (size_t)n);
    }

    // ── 功耗记录环(无共享可变状态的独立读取, 与情景线程并发安全)──
    static int readCurrentMaRaw() {   // 电流绝对值 mA(不依赖共享单位闩, 供记录线程)
        bool ok; long v = readRawLong("/sys/class/power_supply/battery/current_now", ok);
        if (!ok) { v = readRawLong("/sys/class/power_supply/bms/current_now", ok); if (!ok) return -1; }
        long a = v < 0 ? -v : v;
        return (int)(a >= 100000 ? a / 1000 : a);   // µA→mA(或已是 mA)
    }
    int readVoltageMv() {
        bool ok; long uv = readFdLong(11, "/sys/class/power_supply/battery/voltage_now", ok);
        if (!ok || uv <= 0) return 0;
        return (int)(uv >= 100000 ? uv / 1000 : uv);   // µV→mV
    }
    void loadPowerLog() {
        pwrLoaded_ = true;
        FILE* fp = fopen(kPowerLogPath, "r");
        if (!fp) return;
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            PwrSample s; s.screen = 1;
            if (sscanf(line, "%ld,%d,%d,%d,%d,%d,%d",
                       &s.ts, &s.level, &s.curMa, &s.voltMv, &s.tempDc, &s.chg, &s.screen) >= 6) {
                pwrRing_[pwrTail_] = s;
                pwrTail_ = (pwrTail_ + 1) % PWR_N;
                if (pwrCount_ < PWR_N) pwrCount_++;
            }
        }
        fclose(fp);
    }
    void writePowerLog() {
        static char buf[16384];
        int off = 0;
        const int start = (pwrTail_ - pwrCount_ + PWR_N) % PWR_N;
        for (int k = 0; k < pwrCount_; k++) {
            const PwrSample& s = pwrRing_[(start + k) % PWR_N];
            int n = snprintf(buf + off, sizeof(buf) - off, "%ld,%d,%d,%d,%d,%d,%d\n",
                             s.ts, s.level, s.curMa, s.voltMv, s.tempDc, s.chg, s.screen);
            if (n <= 0 || off + n >= (int)sizeof(buf)) break;
            off += n;
        }
        if (off > 0) writeFile(kPowerLogPath, buf, (size_t)off);
    }

    // ── sysfs ──
    static long readRawLong(const char* path, bool& ok) {
        ok = false;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        char b[64];
        ssize_t n = read(fd, b, sizeof(b) - 1);
        close(fd);
        if (n <= 0) return 0;
        b[n] = '\0';
        ok = true;
        return strtol(b, nullptr, 10);
    }

    // ── 缓存 fd + pread(0) 复读 ──
    // 热路径(每 tick 读的温度/GPU/电流/电压/电量/proc 等)改为复用已打开的 fd, 每次 pread 偏移 0
    // 取最新内容(sysfs/proc 都支持), 省去每 tick 十几次 open/close 系统调用 —— 读更快、唤醒更省。
    // 节点消失/读失败即关闭该 slot 下次重开(热插拔/路径变动安全)。slot 固定分配, 见各读取函数。
    bool fdInit_ = false;
    long lastFsmStatusMs_ = 0;   // writeFsmStatus ~1Hz 写节流的上次写入时刻(CLOCK_MONOTONIC ms)
    unsigned long fsmStatusSeq_ = 0;
    std::string curAppName_;
    std::string curFgSource_ = "unknown";
    int curAppCategory_ = -1;
    static constexpr int kFdSlots = 21;   // slot 0-15 见各读取函数; slot 16 FAS measured_fps; slot 17 headroom; slot 18 皮肤/外壳温; slot 19 加权负载 /proc/stat; slot 20 PSI
    int  cachedFd_[kFdSlots];
    const char* gpuPath_ = nullptr;
    const char* curPath_ = nullptr;
    const char* screenPath_ = nullptr;

    // ── 厂商压频 / headroom(移植 ASB metrics_read_thermal 的 msm_performance 读取 + 死信号检测) ──
    // headroom% = 厂商内核强制的 CPU 最大频(policy0) / 本机硬件最大频。<100 即厂商在压频。
    // ⚠ 8 Elite Gen5(SM8850)上该节点常"死"(恒报满频 100%): 连续 10 次满 100% 判死信号 → 标记不可信
    //    (仅遥测、不参与决策); 之后连续 5 次出现合理(<100%)读数则恢复。与 ASB 真源码一致。
    int  hwMaxP0_       = 0;    // policy0 硬件最大频(kHz); 0=未读, -1=读不到
    int  hrStuck100_    = 0;    // headroom 连续 100% 计数
    int  hrDeadLatch_   = 0;    // 1=已判死信号
    int  hrRecover_     = 0;    // 死信号后连续合理读数计数
    int  lastHeadroom_  = -1;   // 最近 headroom%(供遥测; -1=不可信)
    int  lastHeadroomValid_ = 0;
    // 疑似厂商压频滚动 1h 计数(移植 ASB vendor_clamp_1h) + 累计
    int  clamp1hCount_  = 0;
    long clamp1hStart_  = 0;
    long clampTotal_    = 0;
    // 读 headroom%(0..100); 设 lastHeadroom_/lastHeadroomValid_。信号不可用/死信号 → 返回 -1。
    int readHeadroom() {
        if (hwMaxP0_ == 0) {
            bool ok; long v = readRawLong("/sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq", ok);
            hwMaxP0_ = (ok && v > 0) ? (int)v : -1;
        }
        if (hwMaxP0_ <= 0) { lastHeadroomValid_ = 0; lastHeadroom_ = -1; return -1; }
        char buf[256];
        ssize_t n = preadFd(17, Soc::kVendorFreqCap, buf, sizeof(buf));   // 节点路径见 Soc 适配层
        if (n <= 0) { lastHeadroomValid_ = 0; lastHeadroom_ = -1; return -1; }
        buf[(size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1] = '\0';
        // Read cpu0 headroom without acquiring ownership of vendor limits.
        int cap0 = 0;
        char* p = buf;
        while (*p) {
            int cpu = -1; long freq = 0;
            if (sscanf(p, "%d:%ld", &cpu, &freq) == 2 && freq > 0) {
                if (cpu == 0) cap0 = (int)freq;
            }
            while (*p && *p != ' ' && *p != '\n' && *p != '\t') p++;
            while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        }
        if (cap0 <= 0) { lastHeadroomValid_ = 0; lastHeadroom_ = -1; return -1; }
        int hr = clampi((int)((long)cap0 * 100 / hwMaxP0_), 0, 100);
        // 死信号检测/恢复(与 ASB 真源码一致: 10 次满 100% 锁死, 5 次合理读数恢复)
        if (hrDeadLatch_) {
            if (hr < 100) {
                if (++hrRecover_ >= 5) { hrDeadLatch_ = 0; hrStuck100_ = 0; hrRecover_ = 0; }
                else { lastHeadroomValid_ = 0; lastHeadroom_ = hr; return -1; }
            } else { hrRecover_ = 0; lastHeadroomValid_ = 0; lastHeadroom_ = hr; return -1; }
        } else if (hr >= 100) {
            if (++hrStuck100_ >= 10) { hrDeadLatch_ = 1; lastHeadroomValid_ = 0; lastHeadroom_ = hr; return -1; }
        } else {
            hrStuck100_ = 0;
        }
        lastHeadroom_ = hr; lastHeadroomValid_ = 1;
        return hr;
    }

    // 把 headroom 读数转成 soft/hard/vendor 压频标志, 写入 Metrics, 并维护 1h 滚动计数。
    void fillClampMetrics(Metrics& m) {
        if (!Config::AutoMode::anticlamp_enable) {
            m.headroom = -1; m.headroomValid = 0; return;
        }
        int hr = readHeadroom();
        m.headroom = lastHeadroom_;
        m.headroomValid = lastHeadroomValid_;
        if (!lastHeadroomValid_ || hr < 0) return;   // 死信号/不可用: 不做任何判定
        const int softPct = clampi(Config::AutoMode::anticlamp_soft_pct, 1, 100);
        const int hardPct = clampi(Config::AutoMode::anticlamp_hard_pct, 1, 100);
        m.softClamp = (hr < softPct) ? 1 : 0;
        m.hardClamp = (hr < hardPct) ? 1 : 0;
        // 疑似厂商压频: 硬钳 且 温度不高(< hot_temp) → 非热因压频, 正是 anti-clamp 的目标场景
        const bool notHot = (m.tempC < 0) || (m.tempC < Config::AutoMode::hot_temp);
        m.vendorClamp = (m.hardClamp && notHot) ? 1 : 0;
        if (m.vendorClamp) {
            const long now = (long)time(nullptr);
            if (clamp1hStart_ == 0 || now - clamp1hStart_ >= 3600) { clamp1hStart_ = now; clamp1hCount_ = 0; }
            clamp1hCount_++;
            clampTotal_++;
        }
    }

    ssize_t preadFd(int slot, const char* path, char* buf, size_t cap) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);   // fd 缓存跨线程安全
        if (!fdInit_) { for (int i = 0; i < kFdSlots; i++) cachedFd_[i] = -1; fdInit_ = true; }
        int fd = cachedFd_[slot];
        if (fd < 0) {
            fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) return 0;
            cachedFd_[slot] = fd;
        }
        ssize_t n = pread(fd, buf, cap - 1, 0);
        if (n <= 0) { close(fd); cachedFd_[slot] = -1; return 0; }
        buf[n] = '\0';
        return n;
    }
    long readFdLong(int slot, const char* path, bool& ok) {
        char b[64];
        ssize_t n = preadFd(slot, path, b, sizeof(b));
        ok = (n > 0);
        return ok ? strtol(b, nullptr, 10) : 0;
    }
    static void writeFile(const char* path, const char* data, size_t len) {
        if (!path || !data || len == 0) return;
        // 状态文件使用同目录临时文件+完整写入+fsync+rename，避免 App 读到半份快照。
        // 唯一临时名和进程内互斥避免状态/能力文件并发写入互相清理临时文件。
        static std::mutex writeMtx;
        static std::atomic<unsigned long> writeSeq{0};
        std::lock_guard<std::mutex> lock(writeMtx);
        char tmpBuf[320];
        snprintf(tmpBuf, sizeof(tmpBuf), "%s.tmp.%ld.%lu", path, (long)getpid(), ++writeSeq);
        const std::string tmp(tmpBuf);
        int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0 && errno == ENOENT) {
            char dir[256];
            snprintf(dir, sizeof(dir), "%s", path);
            char* slash = strrchr(dir, '/');
            if (slash) { *slash = '\0'; mkdir(dir, 0755); }
            fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        }
        if (fd < 0) return;
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(fd, data + off, len - off);
            if (w > 0) { off += (size_t)w; continue; }
            if (w < 0 && errno == EINTR) continue;
            close(fd);
            unlink(tmp.c_str());
            return;
        }
        if (fsync(fd) != 0) {
            close(fd);
            unlink(tmp.c_str());
            return;
        }
        close(fd);
        if (rename(tmp.c_str(), path) != 0) unlink(tmp.c_str());
    }
    static int normTemp(long raw) {
        long c;
        if      (raw > 1000) c = raw / 1000;
        else if (raw > 200)  c = raw / 10;
        else                 c = raw;
        if (c <= 0 || c > 120) return -1;
        return (int)c;
    }
    void probeThermal() {
        thermalProbed_ = true;
        thermalCount_  = 0;
        if (!Config::AutoMode::thermal_path.empty()) {
            snprintf(thermalNodes_[0], sizeof(thermalNodes_[0]), "%s",
                     Config::AutoMode::thermal_path.c_str());
            thermalCount_ = 1;
            return;
        }
        for (int z = 0; z < 60 && thermalCount_ < 8; z++) {
            char tp[256], typ[64];
            snprintf(tp, sizeof(tp), "/sys/class/thermal/thermal_zone%d/type", z);
            int fd = open(tp, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            ssize_t n = read(fd, typ, sizeof(typ) - 1);
            close(fd);
            if (n <= 0) continue;
            typ[n] = '\0';
            for (ssize_t i = 0; i < n; i++) typ[i] = (char)tolower((unsigned char)typ[i]);
            bool match = false;   // 关键字列表见 Soc 适配层
            for (const char* const* kk = Soc::kThermalKeywords; *kk; ++kk)
                if (strstr(typ, *kk)) { match = true; break; }
            if (!match) continue;
            snprintf(thermalNodes_[thermalCount_], sizeof(thermalNodes_[0]),
                     "/sys/class/thermal/thermal_zone%d/temp", z);
            thermalCount_++;
        }
        logger_.Info("自动模式: 探测到 %d 个 CPU 温度节点", thermalCount_);
    }
    int readCpuTempC() {
        if (!thermalProbed_) probeThermal();
        // 取最高与次高两路有效读数(用于尖峰交叉校验)
        int max1 = -1, max2 = -1;
        for (int i = 0; i < thermalCount_; i++) {
            bool ok; long raw = readFdLong(i, thermalNodes_[i], ok);   // slot 0..7
            if (!ok) continue;
            int c = normTemp(raw);
            if (c < 0) continue;
            if (c > max1) { max2 = max1; max1 = c; }
            else if (c > max2) { max2 = c; }
        }
        if (max1 < 0) return lastGoodTemp_;   // 本 tick 无有效读数: 沿用上次好值(不更新基线)
        // 尖峰守卫(移植 ASB asb_metrics.h spike guard): 单 tick 跳升 ≥25℃ 且次高传感器不背书
        //(max1 ≥ max2+25, 即孤立离群)→ 判为假尖峰, 保持上次好值且不推进基线(下 tick 仍与尖峰前比)。
        // 若两路同时升高(真实快速升温, max1<max2+25)则照常接受 —— 与 ASB"两传感器一起升才算真"一致。
        if (lastGoodTemp_ > 0 && max1 >= lastGoodTemp_ + 25 && (max2 < 0 || max1 >= max2 + 25)) {
            // 单传感器(thermalCount_==1, max2 恒 -1)设备遇真实骤升时, 若一律拒绝且永不推进基线,
            // 会把温度永久冻结在尖峰前的值 → 热否决/压频彻底失效(安全隐患)。改为: 连续两 tick 仍是
            // 高读数(真实持续升温, 非瞬时毛刺)即接受; 瞬时毛刺下 tick 即回落, streak 清零仍被过滤。
            if (++tempSpikeStreak_ < 2) {
                logger_.Debug("自动模式: 温度假尖峰拒绝(读=%d 次高=%d 基线=%d)", max1, max2, lastGoodTemp_);
                return lastGoodTemp_;
            }
            logger_.Debug("自动模式: 温度骤升连续两 tick, 视为真实升温接受(读=%d 基线=%d)", max1, lastGoodTemp_);
        }
        // 冷启动洞: 无基线(lastGoodTemp_<=0)时上面的守卫被跳过, 开机首读若有单路异常温度
        //(thermal HAL 未稳/乱码 normTemp 落进 90~110 区)会被无条件采信(日志的 温=97/94)。
        // 这里在无基线时也做孤立离群检验: 极热(≥vhot_temp)且次高传感器不背书 → 判可疑, 返回 -1(未知,
        // 不播种基线)。两路同热(真实开机发热, max1<max2+25)仍照常接受, 不掩盖真热。
        if (lastGoodTemp_ <= 0 && max1 >= Config::AutoMode::vhot_temp &&
            (max2 < 0 || max1 >= max2 + 25)) {
            logger_.Debug("自动模式: 冷启动孤立高温拒绝(读=%d 次高=%d 无基线)", max1, max2);
            return -1;
        }
        tempSpikeStreak_ = 0;   // 接受本读数: 清尖峰连击计数
        lastGoodTemp_ = max1;
        return max1;
    }
    // ── ASB v59 皮肤/外壳温 (only-if-exists; 择一最佳物理 zone + 死传感器校验) ──
    // 校验单个 zone: 三读全同且极端(>90 或 <5)= 呆滞死传感器 → 失效; 否则有效。
    // 忠实 ASB thermal_sensor_validate: 拒绝合成/卡死传感器, 防虚拟皮肤读虚值把热保护架空。
    // 必须用一次性非缓存读(readRawLong): preadFd 的 fd 缓存按 slot 不按 path, 探测循环多 zone
    // 复用同一 slot 会把 fd 钉死在第一个候选上 → 后续候选的"校验"读的全是第一个 zone 的数据。
    bool validateThermalZone(const char* node) {
        bool ok; long v1 = readRawLong(node, ok);
        if (!ok) return false;
        int c1 = normTemp(v1);
        if (c1 < 0) return false;
        long v2 = readRawLong(node, ok); if (!ok) return false;
        long v3 = readRawLong(node, ok); if (!ok) return false;
        if (v1 == v2 && v2 == v3 && (c1 > 90 || c1 < 5)) return false;
        return true;
    }
    // 按 Soc::skinThermalPriority 择【优先级最高且校验通过】的唯一物理外壳 zone。
    // 物理 shell_*(prio1-3)优先于虚拟 skin-msm/virt(prio4); 无有效物理外壳 → skinNode_ 留空(恒 -1)。
    void probeSkinThermal() {
        skinProbed_ = true; skinNode_[0] = '\0'; skinPrio_ = 99;
        for (int z = 0; z < 60; z++) {
            char tp[256], typ[64];
            snprintf(tp, sizeof(tp), "/sys/class/thermal/thermal_zone%d/type", z);
            int fd = open(tp, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            ssize_t n = read(fd, typ, sizeof(typ) - 1); close(fd);
            if (n <= 0) continue;
            typ[n] = '\0';
            while (n > 0 && (typ[n-1] == '\n' || typ[n-1] == '\r' || typ[n-1] == ' ')) typ[--n] = '\0';
            for (ssize_t i = 0; i < n; i++) typ[i] = (char)tolower((unsigned char)typ[i]);
            int prio = Soc::skinThermalPriority(typ);
            if (prio <= 0 || prio >= skinPrio_) continue;   // 只接受更优先的
            char tempPath[256];
            snprintf(tempPath, sizeof(tempPath), "/sys/class/thermal/thermal_zone%d/temp", z);
            if (!validateThermalZone(tempPath)) continue;   // 死/合成传感器跳过
            snprintf(skinNode_, sizeof(skinNode_), "%s", tempPath);
            skinPrio_ = prio;
            if (prio == 1) break;                            // shell_frame 最佳
        }
        // 清 slot 18 可能残留的 fd(探测期间 validateThermalZone 已改为非缓存读, 但保险起见仍复位):
        // 确保运行期首次 readSkinTempC 一定按最终选定的 skinNode_ 重新 open, 而非沿用陈旧 fd。
        {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            if (fdInit_ && cachedFd_[18] >= 0) { close(cachedFd_[18]); cachedFd_[18] = -1; }
        }
        if (skinNode_[0]) logger_.Info("自动模式: 皮肤/外壳温传感器已绑定 (prio=%d)", skinPrio_);
        else              logger_.Info("自动模式: 无有效物理外壳温传感器, 热决策回退结温门");
    }
    int readSkinTempC() {
        if (!skinProbed_) probeSkinThermal();
        if (!skinNode_[0]) return -1;
        bool ok; long raw = readFdLong(18, skinNode_, ok);
        if (!ok) return -1;
        return normTemp(raw);
    }
    // GPU 占用%(移植 ASB metrics_read_gpu 的 gpu_busy_percentage), 供会话"重载"判定; -1=不可读
    int readGpuBusy() {
        if (!gpuPath_) {   // 首次解析出可用节点, 之后固定走缓存 fd(slot 8); 候选列表见 Soc 适配层
            for (const char* const* pp = Soc::kGpuBusy; *pp; ++pp) {
                bool ok; readRawLong(*pp, ok); if (ok) { gpuPath_ = *pp; break; }
            }
            if (!gpuPath_) return -1;
        }
        bool ok; long v = readFdLong(8, gpuPath_, ok);
        return ok ? clampi((int)v, 0, 100) : -1;
    }
    // /proc/loadavg 1 分钟负载(供 FSM 负载档判定)
    float readLoad1() {
        char b[64];
        ssize_t n = preadFd(13, "/proc/loadavg", b, sizeof(b));   // slot 13
        if (n <= 0) return 0;
        return (float)atof(b);
    }

    // CPU 真实活跃%(排除 idle+iowait): 读 /proc/stat 首行做增量。安卓 loadavg 含 I/O 等待会偏高,
    // 这个值只反映"CPU 真在干活"的比例, 用作 load 触发的闸门(load 高但 CPU 闲=纯 I/O→不升档)。
    // 首次或读失败返回 -1(调用方据此不启用闸门)。
    long prevCpuNonIdle_ = 0;
    long prevCpuTotal_   = 0;
    int readCpuBusy() {
        char b[256];
        ssize_t n = preadFd(12, "/proc/stat", b, sizeof(b));   // slot 12
        if (n <= 0) return -1;
        // 格式: "cpu  user nice system idle iowait irq softirq steal ..."
        long v[8] = {0};
        char* p = b;
        if (strncmp(p, "cpu ", 4) != 0 && strncmp(p, "cpu\t", 4) != 0) return -1;
        p += 3;
        int cnt = 0;
        while (*p && cnt < 8) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p < '0' || *p > '9') break;
            v[cnt++] = atol(p);
            while (*p >= '0' && *p <= '9') p++;
        }
        if (cnt < 5) return -1;
        long idleAll  = v[3] + v[4];                       // idle + iowait(都算"没在干活")
        long nonIdle  = v[0] + v[1] + v[2] + v[5] + v[6] + v[7];  // user+nice+system+irq+softirq+steal
        long total    = idleAll + nonIdle;
        const bool first = (prevCpuTotal_ == 0);   // 首次无基线: 别用全程平均, 本 tick 跳过闸门
        long dTotal   = total - prevCpuTotal_;
        long dNon     = nonIdle - prevCpuNonIdle_;
        prevCpuTotal_   = total;
        prevCpuNonIdle_ = nonIdle;
        if (first || dTotal <= 0) return -1;   // 首次/无增量 → -1(调用方据此跳过闸门, 回退 loadavg)
        int pct = (int)(dNon * 100 / dTotal);
        return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    }

    // ── 能效加权整机负载(移植 uperf: load = Σ eff(i)×load_pct(i)×freq_mhz(i)) ──
    // 平摊平均把"大核跑 50%"与"小核跑 50%"等同看待, 但两者代表的真实工作量/功耗差数倍。
    // 用内核 EAS 提供的 cpu_capacity(最大核归一到 1024)作能效系数, 乘各核当前频率占比, 归一到 0-100:
    //   weightedPct = Σ(cap_c × busy_c × curFreq_c) / Σ(cap_c × maxFreq_c)   (busy_c 为 0-100)
    // 大核忙贡献远大于小核忙; 且随实际频率缩放(大核低频待机贡献小)。cpu_capacity 读不到则退回
    // 用 cluster 最高频当能效系数(仍捕捉"大核权重高")。任一环缺失 → 返回 -1(调用方回退平均 cpuBusy)。
    bool  wProbed_ = false;
    int   wClOfCore_[8]  = { -1,-1,-1,-1,-1,-1,-1,-1 };  // core→cluster idx(0..3)
    // 每簇最忙核 busy%(0..100, -1=本拍读不到); 由 readWeightedBusy 的逐核增量顺带算出。
    // 负载闭环调频(LoadGovernor)的唯一输入 —— 不新增采样、不新增 fd。
    int   wClBusy_[4]    = { -1,-1,-1,-1 };
    long  wCapCore_[8]   = {0};                          // 各核能效系数(cpu_capacity 或频率回退)
    int   wPolicyCl_[Config::kClusterCount] = { -1,-1,-1,-1 };
    long  wMaxFCl_[Config::kClusterCount]   = {0};
    long  wPrevNon_[8] = {0}, wPrevTot_[8] = {0};
    void probeWeighted() {
        wProbed_ = true;
        for (int i = 0; i < Config::kClusterCount; i++) {
            int pol = Config::Policy::CpuPolicy[i];
            wPolicyCl_[i] = pol;
            if (pol < 0) continue;
            char path[128];
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq", pol);
            bool ok; long mf = readRawLong(path, ok);
            wMaxFCl_[i] = ok && mf > 0 ? mf : 0;
            // related_cpus → 该簇覆盖的核号, 建 core→cluster 映射
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpufreq/policy%d/related_cpus", pol);
            char b[64] = {0};
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) { ssize_t n = read(fd, b, sizeof(b)-1); close(fd); if (n > 0) b[n] = '\0'; }
            char* p = b;
            while (*p) {
                if (*p >= '0' && *p <= '9') {
                    int c = atoi(p);
                    if (c >= 0 && c < 8) wClOfCore_[c] = i;
                    while (*p >= '0' && *p <= '9') p++;
                } else p++;
            }
        }
        // 各核能效系数: cpu_capacity 优先, 缺失退回该簇最高频(GHz 量级, 仍保持大核权重高)
        for (int c = 0; c < 8; c++) {
            int cl = wClOfCore_[c];
            if (cl < 0) { wCapCore_[c] = 0; continue; }
            char path[96];
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
            bool ok; long cap = readRawLong(path, ok);
            wCapCore_[c] = (ok && cap > 0) ? cap : (wMaxFCl_[cl] / 1000);
        }
    }
    int readWeightedBusy() {
        if (!wProbed_) probeWeighted();
        // 逐核 /proc/stat 增量 busy%: 读整个 /proc/stat(含 cpu0..cpuN 行)
        char b[1024];
        ssize_t n = preadFd(19, "/proc/stat", b, sizeof(b));   // slot 19(专用, 不与 slot 12 首行/slot 15 背光冲突)
        if (n <= 0) return -1;
        b[n < (ssize_t)sizeof(b) ? n : (ssize_t)sizeof(b)-1] = '\0';
        long busyC[8] = {0}; bool haveC[8] = {false};
        bool anyFirst = false, anyValid = false;
        char* line = b;
        while (line && *line) {
            char* eol = strchr(line, '\n');
            if (eol) *eol = '\0';
            if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '9') {
                int c = atoi(line + 3);
                if (c >= 0 && c < 8) {
                    long v[8] = {0}; char* p = line + 3;
                    while (*p && (*p < '0' || *p > '9')) p++;   // 跳过核号到首个数值
                    while (*p >= '0' && *p <= '9') p++;
                    int cnt = 0;
                    while (*p && cnt < 8) {
                        while (*p == ' ' || *p == '\t') p++;
                        if (*p < '0' || *p > '9') break;
                        v[cnt++] = atol(p);
                        while (*p >= '0' && *p <= '9') p++;
                    }
                    if (cnt >= 5) {
                        long idle = v[3] + v[4];
                        long non  = v[0]+v[1]+v[2]+v[5]+v[6]+v[7];
                        long tot  = idle + non;
                        long dTot = tot - wPrevTot_[c], dNon = non - wPrevNon_[c];
                        bool first = (wPrevTot_[c] == 0);
                        wPrevTot_[c] = tot; wPrevNon_[c] = non;
                        if (first) anyFirst = true;
                        else if (dTot > 0) {
                            long pct = dNon * 100 / dTot;
                            busyC[c] = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
                            haveC[c] = true; anyValid = true;
                        }
                    }
                }
            }
            line = eol ? eol + 1 : nullptr;
        }
        if (anyFirst || !anyValid) return -1;   // 首采样无基线 → 跳过本 tick
        // 能力加权利用率(不乘当前频率!): weighted = Σ(cap_c × busy_c) / Σ(cap_c)。
        // 【关键修正·防自反馈陷阱】: /proc/stat 的 busy% 是"非空闲时间占比", 与频率无关
        // (核被 FSM 压到低频但仍打满 → busy 照读 100)。若像 uperf 那样再乘 scaling_cur_freq,
        // 而 cur_freq 又被 FSM 自己写的 scaling_max 钳住 → 满载 CPU 密集负载读出的加权值被腰斩,
        // 永远升不到 HEAVY, 频率越压越低、读数也越低, 形成传感器-执行器欠频死锁。去掉频率项后,
        // 大核忙(cap 大)贡献大、小核忙贡献小(仍兑现"大核 50%≠小核 50%"), 且不再有反馈陷阱。
        long long num = 0, den = 0;
        for (int c = 0; c < 8; c++) {
            int cl = wClOfCore_[c];
            if (cl < 0 || wCapCore_[c] <= 0) continue;
            if (!haveC[c]) continue;   // 该核本 tick 无有效增量(离线/热插拔): 跳过, 分母也不计
            num += (long long)wCapCore_[c] * busyC[c];
            den += (long long)wCapCore_[c];
        }
        if (den <= 0) return -1;

        // ── 顺带算出每簇**最忙核**的 busy%（负载闭环调频的输入）──
        // 复用本次 /proc/stat 逐核增量, 不新增任何采样。
        // 取簇内 max 而非均值: 调度器搬的是线程 —— 1 线程跑在 6 核簇上是「1 核忙、5 核闲」,
        // 均值会把这种场景抹成 ~17% 而误判为轻载（ChiRi 旧版「需求桶」就是死于这类"整簇满载"假设）。
        // 频率域是簇内共享的, 只要有一颗核打满, 整簇就得抬频。
        for (int i = 0; i < Config::kClusterCount; ++i) wClBusy_[i] = -1;
        for (int c = 0; c < 8; ++c) {
            const int cl = wClOfCore_[c];
            if (cl < 0 || cl >= Config::kClusterCount || !haveC[c]) continue;
            const int pct = busyC[c] < 0 ? 0 : (busyC[c] > 100 ? 100 : (int)busyC[c]);
            if (pct > wClBusy_[cl]) wClBusy_[cl] = pct;
        }

        long long pct = num / den;   // busy_c 已是 0-100 → 结果自然 0-100
        return pct < 0 ? 0 : (pct > 100 ? 100 : (int)pct);
    }

    // ADPF 热余量(Google 官方跨机型热信号): App 经 PowerManager.getThermalHeadroom(forecast) 读到
    // 归一化余量(0.0=凉快 1.0=重度节流), 写入 kHeadroomPath。守护读文件(mtime 判新鲜, >30s 陈旧作废)。
    // 预测性: 物理温度还没到阈值前就能提前预警节流, 用于游戏态提前削频(见 scheduler 联合决策)。
    // 无文件/无 ADPF 支持/陈旧 → 返回 -1(调用方忽略)。返回 0-100(=headroom×100)。
    int readAdpfHeadroom() {
        struct stat st;
        if (stat(kHeadroomPath, &st) != 0) return -1;
        if ((long)time(nullptr) - (long)st.st_mtime > 30) return -1;   // 陈旧(App 停推)
        FILE* fp = fopen(kHeadroomPath, "r");
        if (!fp) return -1;
        char b[32] = {0};
        size_t n = fread(b, 1, sizeof(b) - 1, fp);
        fclose(fp);
        if (n == 0) return -1;
        b[n] = '\0';
        double v = atof(b);
        if (v < 0) return -1;               // App 写 -1 表示本机无 ADPF/读到 NaN
        int hr = (int)(v * 100.0 + 0.5);
        return hr < 0 ? 0 : (hr > 100 ? 100 : hr);
    }

    // PSI CPU 争用压力(GearDVFS meta-state 简化: 纯利用率混淆"真算"与"等待", PSI 直接量化等待)。
    // /proc/pressure/cpu 的 "some avg10=X.YZ" = 近 10s 内有任务因抢不到 CPU 而等待的时间比。
    // 利用率没到顶但任务在排队(突发/多任务争用)时它会高 —— 补足平均利用率漏掉的真实需求。
    // 无 PSI(旧内核/未开 CONFIG_PSI)→ 节点不存在 → 返回 -1(调用方忽略)。
    bool psiProbed_ = false, psiAvail_ = false;
    int readPsiCpu() {
        char b[192];
        ssize_t n = preadFd(20, "/proc/pressure/cpu", b, sizeof(b));   // slot 20(专用)
        if (n <= 0) { psiProbed_ = true; return -1; }
        psiProbed_ = true; psiAvail_ = true;
        const char* p = strstr(b, "some");
        if (!p) return -1;
        p = strstr(p, "avg10=");
        if (!p) return -1;
        p += 6;
        // "X.YZ" → 百分比整数(四舍五入)
        double v = atof(p);
        if (v < 0) v = 0; if (v > 100) v = 100;
        return (int)(v + 0.5);
    }
    int readBatteryPct() {
        bool ok; long v = readFdLong(9, "/sys/class/power_supply/battery/capacity", ok);   // slot 9
        if (!ok) return -1;
        return clampi((int)v, 0, 100);
    }
    bool readCharging() {
        char b[32];
        ssize_t n = preadFd(14, "/sys/class/power_supply/battery/status", b, sizeof(b));   // slot 14
        if (n <= 0) return false;
        return strstr(b, "Charging") != nullptr || strstr(b, "Full") != nullptr;
    }
    // 电池放电电流绝对值(mA)。移植 ASB current_now 多路径 + 单位自适配:
    // current_now 各机单位不一(µA / mA / 0.1mA), >=100000 视为 µA(÷1000), 否则视为 mA。
    // 充电/读不到返回 -1, 调用方据此跳过电流闸门(与 ASB !ma_valid 旁路一致)。
    int curUnitUa_ = 0;   // 0=未定, 1=µA(÷1000), 2=已是 mA(原值)
    int readBatteryCurrentMa() {
        if (!curPath_) {   // 候选列表见 Soc 适配层
            for (const char* const* pp = Soc::kCurrentNow; *pp; ++pp) {
                bool r; readRawLong(*pp, r); if (r) { curPath_ = *pp; break; }
            }
            if (!curPath_) return -1;
        }
        bool ok = false; long raw = readFdLong(10, curPath_, ok);   // slot 10
        if (!ok) return -1;
        long a = raw < 0 ? -raw : raw;
        if (curUnitUa_ == 0) {                       // 首次定标: 量级 >=10万 ⇒ µA
            curUnitUa_ = (a >= 100000) ? 1 : 2;
        }
        long ma = (curUnitUa_ == 1) ? (a / 1000) : a;
        return (int)(ma > 100000 ? 0 : ma);          // 异常超大值归零, 防误判
    }
    int readScreenOn() {
        if (!screenPath_) {   // 候选列表见 Soc 适配层
            for (const char* const* pp = Soc::kScreenBacklight; *pp; ++pp) {
                bool ok; readRawLong(*pp, ok); if (ok) { screenPath_ = *pp; break; }
            }
            if (!screenPath_) return -1;
        }
        bool ok; long v = readFdLong(15, screenPath_, ok);   // slot 15
        return ok ? (v > 0 ? 1 : 0) : -1;
    }

    // ── 持久化(每桶 alpha,obs,last_seen) ──
    void loadState() {
        stateLoaded_ = true;
        nightWindowLoad();   // 夜间自学窗口随状态一并载入
        for (int i = 0; i < 12; i++) {
            alphaRaw_[i] = 500; obsX100_[i] = 0; lastSeen_[i] = 0; baseCostMa_[i] = -1;
            dtMean100_[i] = 0; dtMad100_[i] = 0; dtSamples_[i] = 0;
        }
        FILE* fp = fopen(kStatePath, "r");
        if (!fp) { logger_.Info("自动模式: 无历史学习状态, 全部桶中性 alpha=500 conf=0"); return; }
        char line[160];
        int loaded = 0;
        while (fgets(line, sizeof(line), fp)) {
            // 字段: alpha,obs,last_seen[,base_cost[,dt_mean,dt_mad,dt_samples]] —— 后段可选, 兼容旧 4/5 字段格式
            int idx, a, obs, bc = -1, dm = 0, dmad = 0, ds = 0; long ls;
            int got = sscanf(line, "b%d=%d,%d,%ld,%d,%d,%d,%d", &idx, &a, &obs, &ls, &bc, &dm, &dmad, &ds);
            if (got >= 4 && idx >= 0 && idx < 12) {
                alphaRaw_[idx] = clampi(a, -1000000, 1000000);   // 仅防损坏文件, 学习本身不设界
                obsX100_[idx]  = clampi(obs, 0, ASB_OBS_CAP_X100);
                lastSeen_[idx] = ls;
                if (got >= 5) baseCostMa_[idx] = (bc > 0 ? bc : -1);
                if (got >= 8) { dtMean100_[idx] = dm; dtMad100_[idx] = (dmad > 0 ? dmad : 0);
                                dtSamples_[idx] = (ds > 0 ? ds : 0); }
                loaded++;
            }
        }
        fclose(fp);
        logger_.Info("自动模式: 已载入 %d/12 桶学习状态", loaded);
    }
    void saveState() {
        char buf[1024];
        int off = 0;   // 不写注释头(用户要求 auto_state.txt 不带注释)
        for (int i = 0; i < 12 && off < (int)sizeof(buf); i++) {
            off += snprintf(buf + off, sizeof(buf) - off, "b%d=%d,%d,%ld,%d,%d,%d,%d\n",
                            i, alphaRaw_[i],
                            clampi(obsX100_[i], 0, ASB_OBS_CAP_X100), lastSeen_[i],
                            baseCostMa_[i], dtMean100_[i], dtMad100_[i], dtSamples_[i]);
        }
        if (off > 0) writeFile(kStatePath, buf, (size_t)off);
    }
};
