// By Ktwo
#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "Json/string.hpp"

using string_t = qlib::string_t;
using bool_t   = qlib::bool_t;

using std::ifstream;
using std::unordered_map;

namespace Config {

    // 拓扑/容量常量：CPU 簇数、核数、单簇调速器参数容量（与白名单 commonParams 条数一致）
    inline constexpr int kClusterCount   = 4;
    inline constexpr int kCoreCount      = 8;
    inline constexpr int kMaxSchedParams = 16;

    namespace Meta {
        inline string_t name;
        inline int      version = -1;
        inline string_t author;
        inline string_t loglevel;
    }

    namespace Policy {
        inline int CpuPolicy[kClusterCount] = { -1, -1, -1, -1 };
    }

    namespace Cpuset {
        inline bool     enable = false;
        inline string_t top_app;
        inline string_t foreground;
        inline string_t background;
        inline string_t system_background;
        inline string_t restricted;
    }

    namespace LaunchBoost {
        inline bool     enable = false;
        inline int      boost_rate_limit_ms = 35;
        inline string_t BoostFreq[kClusterCount];
    }

    namespace OfficialMode {
        inline bool enable = false;
    }

    // 系统级优化(移植 ASB 非 CPU 调优)。SysTune=每档 VM/网络/WiFi(低风险, 默认开);
    // BootTrim=开机停调试服务+关 WiFi 后台扫描(pm disable 跨重启持久, 默认关);
    // BgTrim=后台内存软回收(默认关, 同 ASB)。
    namespace SysTune  { inline bool enable = true;  }
    namespace BootTrim { inline bool enable = false; }
    namespace BgTrim   { inline bool enable = false; inline int interval_s = 90; }
    // 触控加速(Kin 自研, 不依赖内核 input_boost): 触摸时把各簇 scaling_min 顶到 cpuinfo_max 的
    // level_pct%, 停手 duration_ms 后回落到基础 min。所有模式可用, 默认关。
    namespace TouchBoost { inline bool enable = false; inline int duration_ms = 1200; inline int level_pct = 55; }
    // 自动模式(mode=auto): 守护按 温度/电量/屏幕/时段 + 自学习 在 powersave/balance/performance
    // 之间动态选基础档。切换粒度为整个 mode(频率低), 不重写频率上限, 不与 governor 自主调频冲突。
    // App 画像仍优先: auto 只决定"无专属画像时"的基础档。所有阈值可在 config.json 的
    // Function.AutoMode 覆盖, 无需重编译。
    // 阈值默认值对齐 AutoSystemBoost(ASB) Smart Mode 真实源码 asb_smart_defs.h(MIT, Dima Krylov)。
    // 详见 AutoMode.hpp 顶部署名。所有阈值可在 config.json 的 Function.AutoMode 覆盖, 无需重编译。
    namespace AutoMode {
        inline bool     enable      = true;   // 总开关(mode=auto 时是否真正接管; false 则回退 balance)
        inline string_t thermal_path;         // 可选: 指定 CPU 温度 sysfs 节点; 空=自动探测 thermal_zone
        inline int      warm_temp   = 50;     // 软提升起点(℃): >此温度 alpha 线性上抬(ASB 软 bump 50→veto)
        inline int      cool_temp   = 55;     // 凉爽阈值(℃): 会话峰温 < 此值且 CLEAN 才学习偏均衡(ASB clean cool<55)
        inline int      hot_temp    = 70;     // 热信号(℃): 会话峰温 ≥ 此值 → 学习偏省电 +40(ASB hot≥70)
        inline int      vhot_temp   = 80;     // 极热信号(℃): ≥ 此值 → +80(ASB very hot≥80)
        inline int      veto_temp   = 60;     // 热否决(℃): ≥ 此值强制 alpha≥700(对齐 ASB VETO_CPU_TEMP_C=60; 原写 65 系笔误, 上游实为 60, 方向更早否决=更热安全)

        // ── ASB v59 皮肤温度锚定热决策(修"结温恒85-95℃→热闸门永久误触发→恒省电/封performance") ──
        // 有可信的物理外壳温度时, 用皮肤温度(而非负载下恒高的结温 prime 核)驱动热闸门; 结温只做硬上限兜底。
        // 安全兜底: 无物理皮肤传感器 / 皮肤读数不在 (5,70) / gate 关 → 逐字回退结温门(=移植前行为)。
        inline bool     thermal_skin_gate  = true; // 皮肤锚定总开关(ASB thermal_skin_gate=1); false=永久用结温门=现状
        inline int      skin_temp          = 47;   // 皮肤/外壳过热阈(℃): 皮肤≥此值等价结温到 veto(ASB thermal_skin_c=47)
        inline int      junction_hard_temp = 100;  // 结温硬上限(℃, 0=关): 结温≥此值无条件强制否决(真·失控兜底)。
                                                   // 【Kin 专属】ASB 用 95, 但 Kin 结温正常带就是 85-95, 故上调到 100 留余量, 不误触发。
        inline int      low_batt    = 20;     // ≤ 此电量(%): 强制 alpha≥800(ASB LOWBAT_ENGAGE_PCT=20)
        inline int      crit_batt   = 10;     // ≤ 此电量(%): 强制 alpha≥900(ASB LOWBAT_CRIT_PCT=10)
        inline int      night_start = 23;     // 夜间窗口起始小时(对齐 ASB night_quiet_hour_start=23)
        inline int      night_end   = 6;      // 夜间窗口结束小时(ASB NIGHT_HOUR_END=6)
        inline int      tick_sec    = 2;      // 决策周期(秒, 亮屏, 默认2同ASB); 息屏自动放宽

        // ── 负载闭环调频(P0-2/P0-3, 移植 ChiRi CLG 决策内核; 算法见 include/LoadGovernor.hpp) ──
        // 智能档原来只写静态频率界: AutoFsm 每 tick 选一个状态, 状态映射到静态界表, 状态不变则
        // scaling_max 恒定 —— 对负载没有任何反馈。开启后每 loadgov_tick_ms 用**本簇逐核 busy%**
        // 算一次期望上限, 作为「只能收紧」的一层叠在 FSM 预算之内(取 min, 永不突破预算)。
        // 缺省值 = ChiRi 在 SM8550 真机调过的那一组(up 0.88 / down 0.62 / headroom 1.20 /
        // smoothing_up 0.60 / deadzone 0.03)。
        inline bool     loadgov_enable      = true;
        inline int      loadgov_tick_ms     = 600;  // 闭环周期(ms, 200..2000); 600 = ChiRi 160ms 的保守版
        inline int      loadgov_up_pct      = 88;   // 升频门槛(%)
        inline int      loadgov_down_pct    = 62;   // 降频门槛(%)
        inline int      loadgov_headroom_pct= 120;  // 性能余量(%, >=100)
        inline int      loadgov_smooth_up_pct = 60; // 升频步长(%)
        inline int      loadgov_deadzone_pct= 3;    // 写频死区(簇硬件最高频的 %)
        inline int      loadgov_dwell_ms    = 0;    // 写频最小驻留(ms, 0=关); 确认窗口已提供 1.8s/1.2s 驻留

        // ── 稳态下探 T11(见 LoadGovernor.hpp 的 steadyDecay*) ──
        // 稳态时对「写侧上限」做限幅积分下探：目标水位 T = up_threshold × target，
        // util 低于 T-band 就慢慢加深、高于 T+band 就回收、负载上行立即清零。
        // 这是「不封顶、靠门槛省电」的接棒机制 —— 拆掉天花板之后由它把稳态功耗压回来。
        inline bool     loadgov_steady_enable      = true;
        inline int      loadgov_steady_target_pct  = 90;   // T = up_threshold × 本值(%)
        inline int      loadgov_steady_step_x1000  = 10;   // 每拍加深步长(千分比)
        inline int      loadgov_steady_max_pct     = 25;   // 下探幅值上限(%), 抗积分饱和

        // ── FAS 帧级分析(P1-1, 见 include/FasFrames.hpp) ──
        // 读条/过图识别 + 加载后保护 + 升档驻留。逐帧数据来自 dumpsys --latency(免费),
        // 只有 measured_fps 的机型退化成窗口判定(帧率 < 目标 40% 且 GPU 闲)。
        inline bool     fas_loading_enable      = true;  // 加载态识别总开关
        inline int      fas_loading_perf_pct    = 70;    // 加载态上限(%), 下限自动取 上限-10
        inline int      fas_gear_dwell_frames   = 120;   // 升档后最短驻留帧数(60fps ≈ 2s)
        inline int      learn_rate  = 80;     // 学习率(/1000): dt_alpha×此值/1000(ASB LEARN_RATE_X1000=80)
        inline int      drain_high  = 14;     // 亮屏掉电率(%/h) ≥ 此值视为高耗电(学习 +30)
        inline string_t cool_lean;            // 凉爽且低 alpha 时偏好: "performance"=可冲高; 空/其它=最高 balance
        inline int      battery_bias = 0;     // Smart Battery Bias(ASB): 手动把 Smart 往省电推, 直接加到有效 alpha
                                              //   0=关; 越大越省电、略掉跟手。仅影响 auto/Smart(0..300)。

        // ── 会话方向信号 dt 系数(原硬编码经验值, 现可配置; 默认 = ASB 真源码值) ──
        // 会话结束按方向信号累加到该桶 alphaRaw: 正=偏省电, 负=偏均衡。把这些拉出来便于按机型/口味调参,
        // 也是把"reward 信号数据驱动"的第一步(后续可做成按桶实测在线回归)。
        inline int      dt_vhot        = 80;  // 会话峰温 ≥ vhot_temp(极热): +80
        inline int      dt_hot         = 40;  // 会话峰温 ≥ hot_temp(热, 且未达极热): +40
        inline int      dt_drainy      = 30;  // 高耗电(掉电率≥drain_high 或均流≥1200mA): +30
        inline int      dt_sustained   = 20;  // 持续重载(GPU 重载多数 或 暖 tick 多数): +20
        inline int      dt_thermal_hit = 100; // 触发热否决(峰温≥veto_temp): +100
        inline int      dt_thermal_hit_skin_cool = 50; // 同上但皮肤仍凉(<40℃): +50(ASB V64)
                                              //   结温触发 throttle 而皮肤没热→散热吃住了、用户没感觉, 只加一半推力;
                                              //   无物理外壳传感器时仍用 dt_thermal_hit 满推力。
        inline int      dt_clean_cool  = -60; // 干净凉爽会话(CLEAN 且峰温<cool_temp): -60(唯一负向项)

        // ── 息屏背景 uclamp 上限(ASB V64) ──
        // DEEP_IDLE(息屏)时把 background / system-background 的 cpu.uclamp.max 压到 cap 以下。
        // 上游实测 140 mA → 50 mA。只压不抬, 离开 DEEP_IDLE 条件还原原值; 内核无接口则整段跳过。
        // 真机上若发现息屏后大文件下载/备份变慢, 把 enable 设 false 即可(会立即还原)。
        inline bool     bg_uclamp_enable = true;   // 总开关
        inline int      bg_uclamp_cap    = 18;     // 上限百分比(1..99); 18=上游值, 0=不压

        // ── 探索 + 奖励闭环(补齐"纯利用零探索/无奖励"的最致命缺点) ──
        // 仅在已过门控(conf≥350)的桶里, 以小概率对本会话 alpha 做 ±explore_offset 的小扰动;
        // 会话结束用实测放电电流/温度对比该桶基线, 算弱 reward, 给 alphaRaw 一个 ±reward_step 的永久微调。
        // 扰动只作用在覆盖层之前, 温控/低电/夜间等安全地板仍会把 alpha 钳回去 → 不破坏温控安全。
        inline bool     explore_enable = true;  // 总开关; false=完全回到纯利用(原行为)
        inline int      explore_pct    = 4;     // ε: 已门控会话里触发探索的百分比(0..30)
        inline int      explore_offset = 60;    // 单次探索的 alpha 扰动幅度(0..200)
        inline int      reward_step    = 8;     // 每次被奖励/惩罚的探索给 alphaRaw 的永久步长(0..50)

        // ── 厂商压频检测 / anti-clamp(移植 ASB headroom + vendor-clamp; 第1步: 检测+遥测+软钳降激进) ──
        // 读 /sys/kernel/msm_performance/parameters/cpu_max_freq 算 headroom%(厂商强制上限/硬件上限)。
        // ⚠ 8 Elite Gen5(SM8850)上该节点常"死"(恒报满频), 已带 ASB 的死信号检测自动旁路(不可信则啥都不做)。
        // 主动"对抗抬频"那一步暂未启用——需先看真机遥测确认信号在你这台机上是活的。
        inline bool     anticlamp_enable   = true;  // 总开关; false=不读 headroom、相关字段全 -1/0
        inline int      anticlamp_soft_pct = 70;    // headroom<此值=软钳(降激进, 略上抬 alpha)(1..100)
        inline int      anticlamp_hard_pct = 45;    // headroom<此值=硬钳(配合低温→判疑似厂商压频)(1..100)
        inline int      anticlamp_backoff  = 40;    // 软钳时上抬 alpha 的幅度(0=只遥测不调; 0..200)

        // ── anti-clamp 攻击侧(主动对抗抬频; 仅游戏态 + headroom 信号可信 + 非热因压频时介入) ──
        // 检测到"疑似厂商压频"(硬钳 且 温度不高)且当前是游戏态时, 直接回写厂商节点
        // (msm_performance/cpu_max_freq)把各簇上限顶回硬件最高频, 抢回被厂商无谓压掉的频率。
        // ⚠ 默认【关闭】: 该侧会真改厂商频率上限, 需先用第1步遥测在真机确认 headroom_valid 常为 1、
        //    vendor_clamp 真有触发(SM8850 该节点常死), 再打开。任何时刻 温度≥attack_temp / 离开游戏态
        //    / 关检测总开关 → 立即收手并按捕获比例还原厂商钳值(热优先, 绝不把过热机器解钳顶满)。
        // 自遮蔽处理: 抢频后该节点会回读满频(headroom=100), 故进入对抗后按 hold 拍数"硬保持"(不被自写
        //    遮蔽的读数骗回), hold 到点后停写 probe 拍数以读真值, 再决定是否复抢 —— 避免每 tick 抖动。
        inline bool     anticlamp_attack_enable = false; // 攻击侧总开关(默认关; 真机验证 headroom 活后再开)
        inline int      anticlamp_attack_temp   = 46;    // 温度≥此值(℃)绝不抢频/立即收手(热安全硬闸)(30..95)
        inline int      anticlamp_attack_hold    = 8;    // 进入对抗后强制保持的 tick 数(忽略自遮蔽读数)(1..120)
        inline int      anticlamp_attack_probe   = 4;    // hold 到点后停写探测真值的 tick 数(1..120)
        // 预算/退避/futility(对照 ASB asb_governor.c): 防"写了没用"的机器每个游戏会话无限抢频。
        inline int      anticlamp_attack_budget       = 6;  // 每会话抢频次数预算(耗尽→退避)(1..50)
        inline int      anticlamp_attack_backoff_s     = 30; // 预算耗尽后退避秒数(到点回满预算重试)(1..600)
        inline int      anticlamp_attack_max_backoffs  = 2;  // 累计退避达此次仍耗尽→本会话彻底放弃(1..10)

        // ── 置信度方差修正 + eff_obs 滑窗(把"样本计数"升级成"桶内一致性归一的真置信度") ──
        // 原 conf 仅 = 样本计数×时间衰减, 通勤/开会这种桶内高方差场景会"样本多但其实不可信"。
        // 现额外乘一个"一致性因子": 桶内会话方向信号 dt 的平均绝对偏差(MAD)越大→置信越低。
        // eff_obs 也从硬封顶 4000 改成每会话按 obs_retain_pct 衰减再累加(滑窗: 近期会话权重更高)。
        inline bool     conf_var_enable = true;  // false=回到纯样本计数置信度(原行为)
        inline int      mad_scale       = 60;    // 一致性尺度: 桶内 dt 的 MAD=此值时置信×0.5(1..500)
        inline int      obs_retain_pct  = 97;    // eff_obs 每会话保留比例(<100→滑窗衰减, 不再永久封顶)(50..100)

        // ── 自动省电 / 夜间安静(对齐 ASB auto_battery / night_quiet 的显式开关) ──
        // 二者默认 true(行为同旧版"永远生效"); 关掉则对应覆盖层不再上抬 alpha, 由学习值/温控自行决定。
        inline bool     auto_battery_enable = true;  // 低电(≤low_batt)自动偏省电; false=不因低电强制省电
        inline bool     night_quiet_enable  = true;  // 夜间窗口(night_start~night_end)息屏强制省电; false=夜间不特殊处理
        // FSM 主动调频的热限进/出温度(℃)。默认对齐 ASB; 暴露给 App 让用户调"多热开始压频/多凉才松手"。
        // enter 必须 > exit, 否则进出抖动; 解析后会做下限保护。
        inline int      fsm_sustained_enter = 54;    // ≥ 此温度且重载 → 压到 SUSTAINED 档(ASB V54 热经济: 原 57)
        inline int      fsm_sustained_exit  = 48;    // 温度仍 ≥ 此值则不退出 SUSTAINED(ASB V54 热经济: 原 49)
        // 预热宽限(移植 ASB balanced_warmup_grace): 亮屏会话开头 N 秒内抑制进 SUSTAINED 热降档, 避免
        // "刚开游戏/应用的加载发热尖峰"把人卡一下。真烫(温度≥bypass_temp)则照常降, 不牺牲安全。0=关。
        inline int      fsm_warmup_grace_s   = 45;   // 会话开头抑制 SUSTAINED 的宽限秒数(0..300; 0=关)
        inline int      fsm_warmup_bypass_temp = 60; // 宽限期内温度≥此值仍允许进 SUSTAINED(热安全旁路)(40..95)
        // FAS(帧感知调度, 移植 fas-rs): 仅在 GAMING(前台已知游戏)时闭环按帧率调 scaling_max。
        // 拿不到帧时间则回退现有静态游戏档(绝不让游戏变差)。
        // FSM 负载信号: true=能效加权整机负载(uperf 式, 大小核按 cpu_capacity×频率 加权, 更准);
        // false=平摊平均 cpuBusy(旧行为)。加权值读不到时自动回退平均, 不影响可用性。
        inline bool     fsm_weighted_load = true;
        // 游戏态 uclamp.min(0-100, 0=关): game_heavy 游戏时给 top-app 设性能下限, 同时影响
        // 调频(schedutil 响应)与选核(EAS 把高需求任务放大核), 仅前台受益、后台不陪跑。
        // 内核无 cpu.uclamp 接口时自动跳过。默认 20(温和下限, 只抬不压, 可 App 调)。
        inline int      game_uclamp_min = 20;
        // CPU+GPU 联合决策(zTT): 高温 GAMING 且 GPU 明确是瓶颈时, 额外下压大核上限省电(不伤帧率,
        // 只削非瓶颈)。只做这一个安全方向(不反向压 GPU)。FAS 接管时交给帧控不叠加。
        inline bool     fsm_joint_dvfs = true;
        // 天玑机型 FPSGO(联发科官方帧感知引擎, 风驰的 MTK 等价物): game_heavy 游戏时强开 FPSGO,
        // 交给厂商帧感知引擎; 与 Kin FAS 互斥(开则该游戏不走 FAS, 避免两个帧控打架)。
        // 内核无 FPSGO 节点(非 MTK)自动跳过。默认关(与 FAS 互斥且无法在非 MTK 机验证, 需用户 opt-in)。
        inline bool     game_fpsgo = false;

        // ── 功率驱动 boost/压制(移植 powerd: boost 作卡顿补救, 功率作压制边界) ──
        // boost 只在【检测到卡顿】时开(补救响应性), 不再因低功耗预防性开 —— 低功耗不卡顿就保持中性省电。
        // 压制段不变: 高/临界功耗关 boost + 降频/关核/限 GPU。
        // 档位: 0=卡顿(开 boost 补救) 1=中性(不 boost, 省电) 2=高功耗(压大核+限 GPU) 3=临界(关核+限 GPU)。
        // boost 开启条件: 卡顿 且 功耗 < throttle_w(高功耗优先压制, 不 boost)。
        // 卡顿判据(任一): PSI cpu some ≥ jank_psi; 或无 PSI 时 cpuBusyW ≥ jank_busy 且 load1 ≥ jank_load。
        // 仅放电生效(充电旁路); 风驰游戏态/相机保持态不叠加。auto 退出还原配置原值。
        // 内核无 UCLAMP 接口则 boost 段自动跳过(压制段仍走 caps, 不受影响)。
        inline bool     power_enable            = true;   // 总开关
        inline int      power_boost_value       = 128;    // 卡顿 boost 值(0-1024, 128≈12.5% 频率底线)
        inline bool     power_boost_jank_enable = true;   // 卡顿触发 boost 开关; false=永不 boost(纯压制)
        inline int      power_boost_jank_psi    = 20;     // PSI cpu some avg10(%) ≥ 此值判卡顿(最准, 任务等 CPU)
        inline int      power_boost_jank_busy   = 85;     // 无 PSI 回退: 能效加权负载% ≥ 此值
        inline float    power_boost_jank_load   = 4.0f;   // 无 PSI 回退: load1 ≥ 此值(busy+load 同时满足才判卡顿)
        // PSI 分支的【绝对需求下限】(0-100, 0=关闭本确认退回纯 PSI 行为)。
        // PSI 衡量相对争用, 不等于真需要算力: 低频轻载下几个后台线程在小核排队就能把
        // some avg10 顶到两位数。故 PSI 过阈后还须 cpuBusyW ≥ 本值(读不到回退 load1)才算
        // "需要抬频的卡顿" —— 这是低功耗场景被误 boost 的主因所在。默认 45(轻载挡在门外)。
        inline int      power_boost_jank_busy_floor = 45;
        inline int      power_boost_cap_pct     = 85;     // 卡顿 boost 时大核上限抬到 hwmax × 此%(留头, 100=满频会把低需求场景直接顶到硬件极限)
        inline float    power_throttle_w        = 7.5f;   // ≥此功率关 boost + 压大核(重载过载)
        inline float    power_throttle_critical_w = 9.5f; // ≥此功率深度压制(临界保命)
        inline int      power_throttle_big_pct  = 80;     // 高功耗大核上限%(本机 hwmax × pct/100)
        inline int      power_throttle_critical_pct = 60; // 临界大核上限%
        // 过载 GPU 上限(绝对 MHz, 0=不限; 对齐 powerd GPU_CAP_HEAT/CRIT, 只压不抬):
        //   高功耗档 → power_throttle_gpu_mhz; 临界档 → power_throttle_critical_gpu_mhz
        //   临界档同时关大核(pwrParkBig)。与 FSM 每状态 GPU 上限取更严者。
        inline int      power_throttle_gpu_mhz          = 680;  // 高功耗 GPU 上限(MHz)
        inline int      power_throttle_critical_gpu_mhz = 460;  // 临界 GPU 上限(MHz)

        // ── 相机保持(移植 ASB V60 camera_hold) ──
        // 相机流式传输时 ISP+硬件编码器吃负载, 但 GPU 占用和 loadavg 都不高 → FSM 误判 MODERATE,
        // 导致 4K60 录制丢帧(相机 HAL 坐在 foreground cpuset, 既非 top-app 也未被 FSM 抬频)。
        // 开启后: 检测到相机流(相机 App 前台)时, 保持 interactive caps + 还原 top-app/foreground
        // cpuset 到全核 + 抬 camera HAL 所在的 uclamp.max 上限 + 软钳 scaling_max 到够用值。
        // 每个值读取前先记录, 退出时精确还原。热保护不动(junction hard-limit 和 thermal cap 仍节流)。
        //   camera_hold_enable: 总开关
        //   camera_hold_max_pct: 相机时 scaling_max 软钳到 本机 hwmax × pct/100(默认 80, 够 4K60 又不冲满)
        //   camera_hold_pkgs: 相机 App 包名列表(逗号分隔, 前台命中即认); 空则用内置默认表
        inline bool     camera_hold_enable   = true;
        inline int      camera_hold_max_pct  = 80;
        inline string_t camera_hold_pkgs;   // 空=用内置默认相机包名表

        // ── 夜间安静(移植 ASB V61 Quiet Night) ──
        // 夜间(inNight 窗口)+ 息屏 + 未充电时, 不仅强制 alpha≥900(原有), 还【停止 tick polling】,
        // 让设备进入更深睡(原有 30s/60s tick 仍会周期唤醒 CPU 阻止深睡)。
        //   quiet_night_enable: 总开关(与 night_quiet_enable 独立; 后者管 alpha, 本项管 polling)
        //   quiet_night_tick_ms: 夜间息屏 tick 间隔(毫秒, 默认 300000=5min; 远大于息屏 30s, 几乎停 poll)
        //   亮屏/充电/出夜间窗口 → 立即恢复正常 tick(由 inNight + screenOn + charging 三条件门控)
        inline bool     quiet_night_enable   = true;
        inline int      quiet_night_tick_ms  = 300000;   // 5min: 深睡优先, 偶尔醒一次确认状态

        // ── 风驰游戏态 oiface 保活(ColorOS/OxygenOS 系统组件) ──
        // oiface 是 OPPO/OnePlus(风驰内核)游戏调度链的关键系统组件: 它负责游戏场景识别与
        // 性能调度协同, 缺失则风驰游戏态的调度优化失效(频率能拉满但调度协同断链)。
        // 风驰游戏态(game_heavy + GAMING)【进入沿】时, 若 oiface 未存活则强拉:
        //   1) setprop persist.sys.oiface.enable 2  (开启并设为游戏态触发)
        //   2) start oiface                          (init 服务拉起)
        // 【退出风驰时不还原】—— 用户明确要求 oiface 保持存活(退出游戏也不断链,
        //    下次进游戏即时可用, 无需重新拉起的延迟)。仅"退 auto 模式"时下次进入会重拉(幂等)。
        //   oiface_keepalive_enable: 总开关; 非 OPPO/OnePlus 机型无此服务可关(避免无效 start 噪音)
        inline bool     oiface_keepalive_enable = true;

        inline bool     fas_enable     = true;   // 游戏时启用帧感知调频
        inline int      fas_target_fps = 0;      // 0=会话内自学习目标帧率(snap 30/60/90/120/144/165); >0=固定
        // 实验性: 帧时辅助进入 GAMING —— 未白名单的【限帧游戏(GPU<90 永不触 GPU 门)】也能进 GAMING 吃 FAS。
        // 仅当【检测到游戏渲染层(SurfaceView/BLAST) + GPU≥地板 持续 + 亮屏】才认; 面板 measured_fps 高刷滚动也到
        // ~120, 单看它会把滚动误判成游戏, 故【绝不】用 measured_fps 作进入信号。默认关(需用户显式开)。
        inline bool     fas_auto_game  = false;  // 默认 OFF: 帧时辅助自动进 GAMING(无白名单); 开则按渲染层+GPU 地板判
        // ── 游戏识别的兜底清单(见 GameList.hpp) ──
        // 大量手游不声明 android:appCategory, 用户也不手动绑画像; 无障碍/类别失效时靠包名清单兜底。
        // 优先级: 用户显式画像 > 系统 category > 本清单。误判可置 false 关掉。
        inline bool     gamelist_enable = true;
        // ── 游戏态自动免打扰(见 DndControl.hpp): 进 GAMING 切「仅优先」, 退出还原 ──
        // 默认 OFF: 会改变用户可见的系统行为, 需用户显式开启。
        inline bool     auto_dnd = false;

        // ── 游戏态调速器切换(风驰内核 hm(全名 hmbird)/scx 等) ──
        // 仅当前台命中 game_heavy(风驰游戏)画像时才切换。进入沿先不动调速器,
        // 只启动 oiface; 3s 后检查 CPU 调速器是否已是原生风驰内核(hm/hmbird/scx):
        //   是 → 日志"原生风驰启动成功"(ROM/内核已自动切, 不覆盖);
        //   否 → 日志失败, 强制切换。目标 = 偏好列表中【本机存在的第一个】(逗号分隔, 逐个对照
        // scaling_available_governors); 列表全不可用时回退 walt(高通)/sugov_ext(天玑等)。
        // 离开(换前台/退出智能模式/守护退出)时还原原调速器。
        // 空 = 功能关。还原是条件写: 若期间外部又改过 governor 则不覆盖(避免和切档/画像打架)。
        inline string_t gaming_gov = "hm,hmbird,scx";

        // ── auto 模式按 App 类别(ApplicationInfo.category)的策略层 ──
        // 在 FSM 之上叠加【只压不抬】的封顶钳制: 轻类别(视频/社交/音频)压低大核与 GPU 上限省电;
        // 游戏(GAME)/未知(UNDEFINED)不钳制, 交给 FSM+FAS 跑满。FSM 的负载自适应仍在天花板内工作。
        //   maxPctL/maxPctB = 该簇 scaling_max 上限 = 本机 hw 最高频 × pct/100(100=不钳制)
        //   gpuPct          = GPU 上限% 的额外封顶(0=用 FSM 按状态值; >0 时取 min, 只压不抬)
        // governor / 核心停泊属【CPU7 敏感】项, 本版暂不接管(默认全核在线), 待确认后再加。
        inline bool     app_policy_enable = true;   // 总开关: 关则完全按现状(纯 FSM), 不做类别钳制
        // 核心停泊(关核心)总开关, 默认 OFF —— 频率/GPU 钳制默认开, 但关核心必须用户显式 opt-in。
        // 历史: 用户曾因 auto 误关 CPU7 强烈不满, 故此项默认 false; 关则下方 cores[] 一律忽略=全核在线。
        inline bool     app_policy_core_enable = false;
        // cores[8]: 该类别每颗物理 CPU 期望在线态, 下标=cpu0..7, 1=在线 0=停泊。默认全 1=不关核。
        // 仅当 app_policy_core_enable=true 且命中【非 game/非 undefined】类别时生效; 且受硬护栏约束
        // (永不关 cpu0、永不关整簇), 见 scheduler.hpp desiredOnlineForCat()。
        struct CatPolicy { int maxPctL; int maxPctB; int gpuPct; int cores[kCoreCount]; };
        inline constexpr int kCatPolicyCount = 9;
        // 内置默认; 可被 config.json 的 AutoMode.app_policy 逐类别覆盖(见 JsonConfig::parseFunction)。
        // 下标 = ApplicationInfo.category 0..7; 下标 8 = UNDEFINED(-1)。catPolicyName[] 为 config 键名。
        // % 相对本机 cpuinfo_max_freq, 自适配每颗 SoC。以 8E5(6+2: 小核/perf 簇 3628、大核/prime 簇 4396)
        // 为基准对齐: 例 VIDEO 小核 44%≈1596MHz。轻类别真正压频省电, 游戏/未知/地图放高。
        // cores 默认全 {1,1,1,1,1,1,1,1} = 不关核(default 全在线)。用户在 config.json 的
        // app_policy.<类别>.cores 写 8 字符掩码(如 "11111100")才会停泊, 且需 app_policy_core_enable=true。
        inline CatPolicy catPolicyTable[kCatPolicyCount] = {
            {100, 100,   0, {1,1,1,1,1,1,1,1}},  // 0 GAME: 不钳制(FSM+FAS 全权), 永远全核
            { 40,  35,  35, {1,1,1,1,1,1,1,1}},  // 1 AUDIO: 音乐/播客, 大核与 GPU 压到很低(8E5 小核≈1451)
            { 44,  40,   0, {1,1,1,1,1,1,1,1}},  // 2 VIDEO: 解码轻 CPU(8E5 小核≈1596 大核≈1758); GPU 留给解码/显示
            { 70,  60,   0, {1,1,1,1,1,1,1,1}},  // 3 IMAGE: 看图/相册
            { 60,  55,   0, {1,1,1,1,1,1,1,1}},  // 4 SOCIAL: 滚动要点频率但远不必满(8E5 小核≈2177)
            { 55,  50,   0, {1,1,1,1,1,1,1,1}},  // 5 NEWS/阅读: 小说/资讯, 静态为主
            { 90,  85,   0, {1,1,1,1,1,1,1,1}},  // 6 MAPS: 可能 3D 导航, 少钳制
            { 70,  60,   0, {1,1,1,1,1,1,1,1}},  // 7 PRODUCTIVITY: 办公/文档
            {100, 100,   0, {1,1,1,1,1,1,1,1}},  // 8 UNDEFINED(-1): 不钳制, 永远全核
        };
        // 内置默认快照(不可变): 热重载时把整表复位到此, 否则用户删某键后旧 maxL/maxB/gpu 钳值会残留。
        inline const CatPolicy catPolicyDefault[kCatPolicyCount] = {
            {100, 100,   0, {1,1,1,1,1,1,1,1}},
            { 40,  35,  35, {1,1,1,1,1,1,1,1}},
            { 44,  40,   0, {1,1,1,1,1,1,1,1}},
            { 70,  60,   0, {1,1,1,1,1,1,1,1}},
            { 60,  55,   0, {1,1,1,1,1,1,1,1}},
            { 55,  50,   0, {1,1,1,1,1,1,1,1}},
            { 90,  85,   0, {1,1,1,1,1,1,1,1}},
            { 70,  60,   0, {1,1,1,1,1,1,1,1}},
            {100, 100,   0, {1,1,1,1,1,1,1,1}},
        };
        inline const char* const catPolicyName[kCatPolicyCount] = {
            "game", "audio", "video", "image", "social", "news", "maps", "productivity", "undefined",
        };
        inline CatPolicy catPolicy(int category) {
            return catPolicyTable[(category >= 0 && category <= 7) ? category : 8];
        }

        // ── auto 模式按 FSM 状态的频率/GPU 上限覆盖层(config.json 的 AutoMode.fsm_policy) ──
        // 让用户逐状态覆盖内置 FSM 频率曲线的【上限】(下限仍由内置包络给, 只被压到不超新上限):
        //   maxL/maxB = 该状态小核/大核 scaling_max 上限% = 本机 cpuinfo_max_freq × pct/100(合法域 10..100);
        //               -1 = 未设置 → 保持内置曲线(computeCaps 插值结果)。
        //   gpu       = 该状态 GPU 上限%(合法域 0..100); 0 = 未设置 → 用内置 AutoFsm::gpuPctForState。
        // 无总开关: fsm_policy 节点缺失/为空 = 全部未设置 = 行为与旧版完全一致。
        struct FsmPolicy { int maxL = -1; int maxB = -1; int gpu = 0; };
        // 下标 = AutoFsm::State 枚举序: 0=deep_idle 1=light_idle 2=moderate 3=heavy 4=sustained 5=gaming。
        // 内置默认全部未设置; 热重载时先整表复位(见 JsonConfig::resetState), 删键后不残留旧覆盖。
        inline FsmPolicy fsmPolicyTable[6] = {};
        // config 键名(小写), 顺序与上表下标一一对应。
        inline const char* const fsmPolicyName[6] = {
            "deep_idle", "light_idle", "moderate", "heavy", "sustained", "gaming",
        };
        inline FsmPolicy fsmPolicy(int state) {
            return (state >= 0 && state < 6) ? fsmPolicyTable[state] : FsmPolicy{};
        }

        // 12 桶(6 时段 × 工作日/周末)。这里发布【置信门控+覆盖后】的有效 alpha(0..1000, 越大越省电),
        // 供状态文件/前端展示; 学习原值与置信另由 AutoMode 内部持久化。
        inline std::atomic<int> alpha[12] = {};
        inline std::atomic<int> last_bucket{-1};
        inline std::atomic<int> last_temp{-1};
        inline std::atomic<int> last_batt{-1};
        inline std::atomic<int> last_conf{0};   // 当前桶置信度 x1000(0..1000)
    }

    namespace Scheduler {
        inline bool     enable = false;
        inline bool     Sched_energy_aware = false;
        inline bool     Sched_schedstats   = false;
        inline string_t Sched_latency_ns;
        inline string_t Sched_migration_cost_ns;
        inline string_t Sched_min_granularity_ns;
        inline string_t Sched_wakeup_granularity_ns;
        inline string_t Sched_nr_migrate;
        inline string_t Sched_util_clamp_min;
        inline string_t Sched_util_clamp_max;
    }

    namespace GpuFreq {
        inline bool     enable   = true;
        inline string_t min_freq;
        inline string_t max_freq;
        // 原子快照：配置加载完成时发布，gpuFreqGuard 每秒读它而非上面的 string（消除跨线程竞争）
        inline std::atomic<int> min_mhz{0};
        inline std::atomic<int> max_mhz{0};
        inline std::atomic<int> enabled{1};
    }

    namespace Performances {
        inline int      Online[kCoreCount]    = { -1, -1, -1, -1, -1, -1, -1, -1 };  // 默认 -1 跳过，不误关核心
        inline string_t MinFreq[kClusterCount];
        inline string_t MaxFreq[kClusterCount];
        inline string_t CpuGovernor[kClusterCount];
    }

    // 包名匹配支持 * 任意序列 / ? 单字符；优先级：AppProfile > Performances
    struct AppProfileModel {
        static constexpr int MAX_PACKAGES = 32;
        string_t modelName;
        string_t modeName;
        bool     isGame = false;
        // keepAlive=true 使非游戏画像也能锁定前台不掉档（小窗/弹窗/焦点切换均保持）
        bool     keepAlive = false;
        string_t packages[MAX_PACKAGES];
        int      packageCount = 0;

        string_t MinFreq[kClusterCount];
        string_t MaxFreq[kClusterCount];
        string_t Governor[kClusterCount];
        string_t GpuMinFreq;
        string_t GpuMaxFreq;
        int      Online[kCoreCount] = { -1, -1, -1, -1, -1, -1, -1, -1 };

        string_t SchedParamName [kClusterCount][kMaxSchedParams];
        string_t SchedParamValue[kClusterCount][kMaxSchedParams];
        int      SchedParamCount[kClusterCount] = { 0, 0, 0, 0 };
    };

    namespace AppProfile {
        static constexpr int MAX_MODELS    = 32;
        static constexpr int MAX_BLACKLIST = 64;
        inline bool     enable = false;

        inline std::recursive_mutex profileMtx;           // 保护 blacklist/Models 并发读写

        inline string_t        packageBlacklist[MAX_BLACKLIST];
        inline int             blacklistCount = 0;

        inline AppProfileModel Models[MAX_MODELS];
        inline int             modelCount   = 0;

        // perapp_powermode.txt 覆盖层：优先级高于 config.json packages
        static constexpr int MAX_PERAPP = 256;
        inline string_t perAppPkg[MAX_PERAPP];
        inline string_t perAppModel[MAX_PERAPP];
        inline int      perAppFps[MAX_PERAPP] = {0};   // 该 App 的 FAS 目标帧率(0=自动学习); 格式第三列可选
        inline int      perAppCount = 0;
        inline string_t perAppGlobal;              // * 行全局默认，可空

        // 多线程读、单线程写，必须 atomic
        inline std::atomic<int> currentMatch{-1};

        // 迭代式通配符匹配，避免递归爆栈
        inline bool matchPackage(const char* pattern, const char* pkg) {
            const char* star = nullptr;   // 上次 '*' 的位置
            const char* mark = nullptr;   // pkg 在那次 '*' 后的起点
            while (*pkg) {
                if (*pattern == '?') {
                    pattern++; pkg++;
                } else if (*pattern == '*') {
                    star = pattern++;
                    mark = pkg;
                    if (!*pattern) return true; // 末尾 '*'
                } else if (*pattern == *pkg) {
                    pattern++; pkg++;
                } else if (star) {
                    pattern = star + 1;
                    pkg     = ++mark;
                } else {
                    return false;
                }
            }
            while (*pattern == '*') pattern++;
            return *pattern == '\0';
        }

        inline bool isBlacklisted(const char* pkg) {
            std::lock_guard<std::recursive_mutex> lk(profileMtx);
            for (int i = 0; i < blacklistCount; i++) {
                if (matchPackage(packageBlacklist[i].c_str(), pkg)) return true;
            }
            return false;
        }

        inline int findModelByName(const char* name) {
            for (int i = 0; i < modelCount; i++)
                if (Models[i].modelName == name) return i;
            return -1;
        }

        // 该前台包的 FAS 目标帧率覆盖(perapp_powermode.txt 第三列); 无=0=自动学习。
        // 供 FAS 进入游戏时优先于全局 fas_target_fps 使用(fas-rs 每游戏目标帧率同款)。
        inline int fasTargetForPkg(const char* pkg) {
            for (int i = 0; i < perAppCount; i++)
                if (perAppFps[i] > 0 && matchPackage(perAppPkg[i].c_str(), pkg))
                    return perAppFps[i];
            return 0;
        }

        inline int findMatchingModel(const char* pkg, bool useGlobalFallback = true) {
            std::lock_guard<std::recursive_mutex> lk(profileMtx);  // 保护 blacklist/perApp/Models 并发读
            if (isBlacklisted(pkg)) return -1;
            // perapp 优先：config.json 与 perapp 重复时以 perapp 为准
            for (int i = 0; i < perAppCount; i++) {
                if (matchPackage(perAppPkg[i].c_str(), pkg)) {
                    int idx = findModelByName(perAppModel[i].c_str());
                    if (idx >= 0) return idx;
                }
            }
            for (int i = 0; i < modelCount; i++) {
                if (Models[i].packageCount == 0) continue;
                for (int j = 0; j < Models[i].packageCount; j++) {
                    if (matchPackage(Models[i].packages[j].c_str(), pkg)) return i;
                }
            }
            // 无匹配时回退到 perapp 全局默认行（* <模式>），防止停在上个 App 的档位。
            // useGlobalFallback=false: 类别识别(resolveCategory)专用 —— auto 下不应让 * 默认档把任意
            // App 都算成某类别, 否则未声明 appCategory 的游戏会被错判, 应落到系统 appCategory 兜底。
            if (useGlobalFallback && !perAppGlobal.empty()) {
                int idx = findModelByName(perAppGlobal.c_str());
                if (idx >= 0) return idx;
            }
            return -1;
        }
    }

    namespace AppProfile {
        struct ForegroundDecision {
            int matched = -1;
            int selected = -1;
            bool release = false;
            bool applyProfile = false;
        };

        inline bool shouldApplyProfile(int selected) {
            return enable && selected >= 0;
        }

        inline ForegroundDecision decideForeground(const char* pkg, bool autoActive) {
            std::lock_guard<std::recursive_mutex> lk(profileMtx);
            if (autoActive || !pkg || !*pkg) return {};
            if (isBlacklisted(pkg)) return {-1, -1, true, false};
            const int matched = findMatchingModel(pkg);
            int selected = matched;
            if (selected < 0 && !perAppGlobal.empty())
                selected = findModelByName(perAppGlobal.c_str());
            return {matched, selected, false, shouldApplyProfile(selected)};
        }
    }

    // 基础模式的调速器参数表，0-indexed，容量与 AppProfileModel::SchedParam* 一致
    struct SchedParam {
        string_t Name [kMaxSchedParams];
        string_t Value[kMaxSchedParams];
    };
};
