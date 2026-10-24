#pragma once
#include <sys/eventfd.h>
#include "LibUtils.hpp"
#include "Function.hpp"
#include "AutoMode.hpp"
#include "AutoFsm.hpp"
#include "AutoFas.hpp"
#include "SysTweaks.hpp"
#include "PolicyResolver.hpp"
#include "RestorePolicy.hpp"
#include "OwnedNode.hpp"
#include "PresenceCache.hpp"
#include "ProbeCadence.hpp"
#include "LoadGovernor.hpp"
#include "ThermalDecision.hpp"
#include "IdleUclamp.hpp"
#include "PowerBoostNode.hpp"
#include "PackageName.hpp"
#include "DeviceValidation.hpp"
#include "GameList.hpp"
#include "DndControl.hpp"
#include <sys/inotify.h>
#include <sys/resource.h>
#include <poll.h>
#include <sched.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <map>

class Schedule {
private:
    static constexpr const char* configPath = "/sdcard/Android/Kin/mode.txt";
    static constexpr const char* storageConfigPath = "/storage/emulated/0/Android/Kin/mode.txt";
    static constexpr const char* jsonPath = "/sdcard/Android/Kin/config.json";
    // App 开关覆盖层(JsonConfig::settingsPath 同值; 此处用于变更监听/联合哈希)
    static constexpr const char* settingsJsonPath = "/sdcard/Android/Kin/settings.json";
    static constexpr const char* cpusetEventPath = "/dev/cpuset/top-app";
    static constexpr const char* onlinePath = "/sys/devices/system/cpu/cpu%d/online";
    static constexpr const char* SchedParamPath = "/sys/devices/system/cpu/cpufreq/policy%d/%s/%s";
    static constexpr const char* GovernorPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor";
    static constexpr const char* MinFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq";
    static constexpr const char* MaxFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq";

    std::vector<thread> threads;
    std::mutex mtx;
    // 串行化画像应用，防并发交错产生混合态；可重入避免同线程嵌套死锁。
    std::recursive_mutex applyMtx;

    // 优雅退出：析构置位 + 写 eventfd 唤醒阻塞的 poll/select，cv 打断定时等待
    std::atomic<bool> shouldExit_{false};
    int stopEventFd_ = -1;
    std::mutex stopMtx_;
    std::condition_variable stopCv_;
    // FSM 专用唤醒: 前台变化(App 推送)时立即叫醒 FSM tick 重采样, 不等 2s 定时到点。
    // 独立 CV 避免误唤醒其它 waitStop 线程; 析构时也一并 notify 保证能退出。
    std::mutex fsmMtx_;
    std::condition_variable fsmCv_;
    std::atomic<bool> fsmWake_{false};

    // pkg → App 显示名 映射(App 端写 /sdcard/Android/Kin/AI/pkg_label.txt), 仅供日志把包名翻成 App 名字。
    // inotify 线程写、前台评估线程读, 用 appLabelsMtx_ 保护; 读不到就退回包名。
    // 用 std::map(红黑树)而非 unordered_map: 遵本文件 -flto/--gc-sections 下不用 unordered_map 的约定(见下方分类缓存注释)。
    std::map<std::string, std::string> appLabels_;
    std::mutex appLabelsMtx_;

    Function function;
    JsonConfig conf;
    Logger logger;
    Utils utils;

    // 自动模式(mode=auto): autoActive_ 为应用当前模式时发布的门闸, autoModeTask 据此接管/休眠。
    // autoMode_ 须在 logger 之后声明(成员初始化序), 它持 logger 引用。
    std::atomic<bool> autoActive_{false};
    std::atomic<unsigned long long> configGeneration_{0};
    // 关核心去重: auto 下上次施加到 cpuN/online 的在线集合(1/0)。初始全 1=全核在线(开机默认)。
    // 只在持 applyMtx 的 reconcileAutoCores()/onlineAll() 内读写, 无需 atomic。
    int lastAutoOnline_[kCoreCount] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    // 关核停泊迟滞(升快降慢): 同一类别墙钟稳定 ≥6s 才允许停核; applyMtx 内读写。
    static constexpr long long kParkHystMs = 6000;
    int       parkCat_ = INT_MIN;
    long long parkCatSince_ = 0;
    AutoModeCtl autoMode_{logger};
    AutoFsm     autoFsm_{logger};
    AutoFas     autoFas_{logger};
    // ── 负载闭环调频（P0-2 / P0-3）────────────────────────────────────────────
    // 算法在 include/LoadGovernor.hpp（零 IO 纯函数, 主机单测 kin_load_governor）。
    // 定位：**只能收紧**的一层 cap —— 叠在 FSM 预算之内（取 min）, 不参与预算制定,
    // 因此与 FAS / 类别钳制 / 相机 / 热保护 / PolicyResolver 天然兼容（都是取 min）。
    // 只覆盖 little(0)/big(1)：AutoFsm::Caps 只建模这两簇, prime 由 writeFsmCaps 按 maxB 等比缩放。
    kin::LoadGovParams loadGovParams_;
    kin::LoadGovState  loadGov_[2];
    bool               loadGovPrimed_[2]     = { false, false };
    long long          loadGovLastWriteMs_[2] = { 0, 0 };
    kin::DndControl dnd_;            // 游戏态自动免打扰(默认关, config auto_dnd)
    std::string lastModeLogged_;   // 仅在情景模式真正变化时打印, 避免配置重载刷屏
    SysTweaks   sysTweaks_{logger, utils};   // ASB 非 CPU 调优(VM/网络/WiFi/停服务/BG_TRIM)
    DeviceValidation validation_{logger};
    std::string lastAutoTuneMode_;   // G1: auto 档已下发的 SysTune 基础档(powersave/balance/performance), 边沿去重(持 applyMtx)

    std::string lastTopApp;          // 上一次前台应用包名（用于画像切换去重）
    long long   lastGameCpusetFixMs = 0;  // 游戏档 cpuset 矫正节流: 上次矫正时刻(ms), 防 ping-pong
    std::string heldPkg;             // 正在保活的游戏/保活画像包名(进程存活则维持), 空=未保活
    // 保活解除去抖: 连续 kHeldMissMax 轮都查不到进程才真解除。单次 50ms 复查兜不住应用切换期的
    // /proc 扫描瞬断, 而误解除代价是"掉档+还原调速器+重建"一整套, 宁可晚解除一两拍。
    int         heldMissStreak_ = 0;        // 连续未查到保活进程的轮数(查到即清零)
    static constexpr int kHeldMissMax = 3;  // 达到此轮数才判定游戏真退出
    bool        blacklistReleased = false;  // [真黑名单·方案A] 当前是否已对黑名单前台"释放接管"(交还系统)

    // App 无障碍服务(AppSwitchService)推送的前台包名 + 接收时刻; 新鲜则优先于 dumpsys, 省 popen。
    // 服务未启用/被杀 → 推送变陈旧 → 自动回退 dumpsys, 不影响"不开 App 也能用"。
    std::mutex  pushMtx_;
    std::string pushedTopApp_;
    std::atomic<long long> lastPushMs_{0};
    static constexpr long long kPushTtlMs = 30000;  // 推送有效期(ms): 3×心跳(10s), 心跳偶发跳拍不误废
    // auto/FSM 快路: getTopAppFast 返回的顶层可能是黑名单(输入法/launcher/systemui 与真 App 同在 top-app)。
    // 黑名单时回退到上一个"非黑名单"前台, 避免拿键盘当前台误报 类=-1。
    // autoModeTask 会在 auto 快路读写它 → 用 fgMtx_ 守护, 防 std::string 数据竞争。
    std::mutex  fgMtx_;
    std::string lastGoodFgPkg_;
    std::atomic<long long> lastFocusProbeMs_{0};  // mFocusedApp 兜底探测限频游标(≥5s, fas-rs 同款兜底)
    kin::PresenceCache<> lastGoodFgPresence_;
    static constexpr const char* topAppPath = "/sdcard/Android/Kin/topapp.txt";

    // ── App 类别缓存(ApplicationInfo.category): 每包名仅 dumpsys 一次, 之后命中缓存 ──
    // 取值: UNDEFINED=-1 GAME=0 AUDIO=1 VIDEO=2 IMAGE=3 SOCIAL=4 NEWS=5 MAPS=6 PRODUCTIVITY=7。
    // 类别随包名稳定(装一次不变), 故无需 TTL/失效; 仅按容量上限封顶防无界增长。
    // autoModeTask 线程读 / onTopAppChanged 可能他线程触发 → 复用既有 mutex 模式, 单独 catMtx_ 守护。
    std::mutex catMtx_;
    // 分类缓存(包名→category): 定长数组 + 线性查找, 不用 std::unordered_map ——
    // 本 daemon 是 -static -flto -Wl,--gc-sections 构建, 全局/静态 std::unordered_map 的非平凡构造
    // (设 max_load_factor=1.0)可能不生效(初始化被 gc)→ 首次插入按 size/0 算桶数触发
    // __next_prime overflow 崩溃。栈成员当前虽安全, 仍统一改定长数组彻底消除该风险类(全仓无 unordered_map)。
    static constexpr int kCatCacheCap = 64;   // 封顶: 满则清空重建(单设备前台 App 数远小于此)
    std::string catKey_[kCatCacheCap];
    int         catVal_[kCatCacheCap] = {};
    int         catCount_ = 0;

    // 取前台包名的 App 类别(ApplicationInfo.category)。首次见某包 → dumpsys package 一次并缓存;
    // 之后直接命中缓存, 绝不每 tick popen。无效/取不到 → 返回 -1(UNDEFINED)。
    int fgCategory(const std::string& pkg) {
        if (pkg == "null" || !kin::validPackageName(pkg)) return -1;
        {
            std::lock_guard<std::mutex> lk(catMtx_);
            for (int i = 0; i < catCount_; i++)
                if (catKey_[i] == pkg) return catVal_[i];   // 命中: 0 次 popen
        }
        // 仅取含 category 的少数行(grep 收窄缓冲, 省内存/解析); 同时捞数值版与文本版。
        char buf[256] = {0};
        kin::readPackageCategory(pkg, [&](const char* cmd) {
            utils.popenRead(cmd, buf, sizeof(buf));
        });
        int cat = -1;
        // ① 优先数值: AOSP "categoryHint=N"(N 即 ApplicationInfo.category)。
        const char* h = strstr(buf, "categoryHint=");
        if (h) {
            h += 13;   // strlen("categoryHint=")
            if (*h == '-' || (*h >= '0' && *h <= '9')) cat = atoi(h);
        }
        // ② 回退文本: 部分 ROM/版本印 "category=game"/"category=video"/... → 映射回枚举。
        if (cat < 0) {
            const char* c = strstr(buf, "category=");
            if (c) {
                c += 9;   // strlen("category=")
                if      (strncmp(c, "game", 4) == 0)         cat = 0;
                else if (strncmp(c, "audio", 5) == 0)        cat = 1;
                else if (strncmp(c, "video", 5) == 0)        cat = 2;
                else if (strncmp(c, "image", 5) == 0)        cat = 3;
                else if (strncmp(c, "social", 6) == 0)       cat = 4;
                else if (strncmp(c, "news", 4) == 0)         cat = 5;
                else if (strncmp(c, "maps", 4) == 0)         cat = 6;
                else if (strncmp(c, "productivity", 12) == 0) cat = 7;
            }
        }
        {
            std::lock_guard<std::mutex> lk(catMtx_);
            if (catCount_ >= kCatCacheCap) catCount_ = 0;   // 满则整清(逻辑清空, 下次重建)
            catKey_[catCount_] = pkg;
            catVal_[catCount_] = cat;
            catCount_++;
        }
        return cat;
    }

    // App 切换 → 该包类别已稳定缓存, 无需主动失效; 仅在缓存被显式清理时重建。预留清缓存入口。
    void invalidateCategoryCache() {
        std::lock_guard<std::mutex> lk(catMtx_);
        catCount_ = 0;
    }

    // 解析前台包的类别(ApplicationInfo.category 枚举: GAME0/AUDIO1/VIDEO2/IMAGE3/SOCIAL4/NEWS5/MAPS6/PROD7, -1=未知)。
    // ★优先用既有 App 画像(perapp_powermode.txt + config Models, 用户已整理 150+ 包名→video/social/music/...
    //   /game_heavy/game_lite)→ 画像名映射到类别。国产 App(抖音/B站/微信…)多不声明 android:appCategory,
    //   靠 dumpsys 读不到, 但画像表里已有 → 这样才能正确切换。画像未覆盖再退回系统 appCategory(fgCategory)。
    int resolveCategory(const std::string& pkg) {
        if (pkg.empty() || pkg == "null") return -1;
        {
            std::lock_guard<std::recursive_mutex> lk(applyMtx);
            // 画像字符串可能由配置热重载重写，查找与读取必须在同一把锁内完成。
            int gm = Config::AppProfile::findMatchingModel(pkg.c_str(), false);
            if (gm >= 0) {
                if (Config::AppProfile::Models[gm].isGame) return 0;
                const char* mn = Config::AppProfile::Models[gm].modelName.c_str();
                if (strstr(mn, "video"))   return 2;
                if (strstr(mn, "music"))   return 1;
                if (strstr(mn, "social"))  return 4;
                if (strstr(mn, "reading")) return 5;
                if (strstr(mn, "game"))    return 0;
            }
        }
        return fgCategory(pkg);
    }

    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // 缓存上一次写入的 (min,max,gov)，相同则跳过 sysfs 写，避免抖动场景重复刷盘
    string_t lastMinFreq[Config::kClusterCount];
    string_t lastMaxFreq[Config::kClusterCount];
    string_t lastGovernor[Config::kClusterCount];
    // 各簇 cpuinfo_max_freq(kHz) 缓存(硬件固定, 读一次): 供 FSM 给超大核簇按硬件等比缩放上限
    int      clusterHwMaxKHz_[Config::kClusterCount] = { 0, 0, 0, 0 };
    // 各簇 cpuinfo_min_freq(kHz) 缓存(硬件固定, 读一次): 供风驰游戏态写硬件地板极限
    int      clusterHwMinKHz_[Config::kClusterCount] = { 0, 0, 0, 0 };
    // ── 游戏态调速器切换(AutoMode.gaming_gov, 风驰内核 hm/scx 等): 进 GAMING 换、离开还原 ──
    bool     gamingGovActive_ = false;                     // true=ROM/OIface 接管窗口，Kin 不写 governor
    bool     gamingGovEngaged_ = false;                    // 供 FAS 互斥：放权窗口内视为风驰在位
    bool     fengchiGovResetPending_ = false;              // 热重载后通知 FSM 重走进入沿

    // ── 相机保持(ASB V60 camera_hold)状态 ──
    bool     cameraHoldActive_ = false;                    // 当前是否处于相机保持态
    string_t cameraSavedTopAppCpus_;                       // 进入前 top-app cpuset 原值(空=没动)
    string_t cameraSavedFgCpus_;                           // 进入前 foreground cpuset 原值(空=没动)
    string_t cameraSavedUclampMaxFg_;                      // 进入前 foreground uclamp.max 原值(空=没动)
public:
    Schedule& operator=(Schedule&&) = delete;

    Schedule() {
        pinSelfToLittleCores();   // 必须在创建工作线程前设主线程亲和性，新线程继承之
        Init();
        Config::GameList::loadUserList();   // 用户追加的游戏包名(启动一次)
        autoMode_.writeCapabilities();
        stopEventFd_ = eventfd(0, EFD_CLOEXEC);
        // 单 inotify 线程监控三个配置文件，替代原 config/json/perapp 三线程
        threads.emplace_back(thread(&Schedule::fileWatchTask, this));

        if (Config::AppProfile::enable) {
            threads.emplace_back(thread(&Schedule::appProfileTask, this));
        }
        // 开机后多次重应用, 覆盖 ROM 晚启动的开机提频(否则要手动切模式才正常)
        threads.emplace_back(thread(&Schedule::bootResettleTask, this));
        // 实时监测 CPU 状态, 被 ROM/perf 改写则写回配置
        threads.emplace_back(thread(&Schedule::cpuEnforceTask, this));
        // 自动模式决策线程(仅 mode=auto 时接管, 否则低频休眠)
        threads.emplace_back(thread(&Schedule::autoModeTask, this));
        // BG_TRIM 后台内存回收线程(默认关, Function.BgTrim.enable 开启)
        threads.emplace_back(thread(&Schedule::bgTrimTask, this));
        // 功耗记录线程(始终运行, 与情景模式无关): 供 App 首页"当前记录"卡片 + 曲线
        threads.emplace_back(thread(&Schedule::powerRecorderTask, this));
        threads.emplace_back(thread(&Schedule::validationTask, this));
    }

    // 每 30s 记一条电池采样到 power_log.txt(原始量, App 自算功率/会话/曲线)
    void powerRecorderTask() {
        if (waitStop(8000)) return;   // 等系统起来
        while (!shouldExit_.load()) {
            autoMode_.appendPowerSample();
            if (waitStop(30000)) break;
        }
    }

    void validationTask() {
        if (waitStop(12000)) return;
        validation_.run();
        while (!shouldExit_.load()) {
            if (access(DeviceValidation::requestPath(), F_OK) == 0) {
                unlink(DeviceValidation::requestPath());
                validation_.run();
            }
            if (waitStop(2000)) break;
        }
    }

    // 触控加速: 配置内核自带 input boost(高通 cpu_boost / 通用 cpu_input_boost)。
    // enable=true → 各簇按 level_pct% cpuinfo_max 设触摸提频 + 时长并打开; false → 关闭(置 0)。
    // 由内核负责检测触摸事件, 守护只在 config 应用/热重载时写一次。 (按需求改用 ASB/内核同款 input boost)
    void applyTouchBoost() {
        const bool on = Config::TouchBoost::enable;
        int dur = Config::TouchBoost::duration_ms; if (dur < 0) dur = 0;
        int pct = Config::TouchBoost::level_pct; if (pct < 10) pct = 10; else if (pct > 100) pct = 100;
        char buf[16];
        if (on) {
            // 高通 cpu_boost: input_boost_freq 为 "首核:频率" 列表(各簇)
            std::string freqList;
            char part[40], p[128];
            for (int i = 0; i < kClusterCount; i++) {
                int pol = Config::Policy::CpuPolicy[i];
                if (pol < 0) continue;
                FastSnprintf(p, sizeof(p),
                             "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq", pol);
                int hwmax = utils.readInt(p);
                int f = (hwmax > 0) ? (int)((long)hwmax * pct / 100) : 0;
                FastSnprintf(part, sizeof(part), "%s%d:%d", freqList.empty() ? "" : " ", pol, f);
                freqList += part;
            }
            FastSnprintf(buf, sizeof(buf), "%d", dur);
            utils.FileWriteBlocking("/sys/module/cpu_boost/parameters/input_boost_freq", string_t(freqList.c_str()));
            utils.FileWriteBlocking("/sys/module/cpu_boost/parameters/input_boost_ms", string_t(buf));
            utils.FileWriteBlocking("/sys/module/cpu_boost/parameters/sched_boost_on_input", string_t("1"));
            utils.FileWriteBlocking("/sys/module/cpu_boost/parameters/input_boost_enabled", string_t("1"));
            // 通用 cpu_input_boost 变体(部分内核用这套)
            utils.FileWriteBlocking("/sys/module/cpu_input_boost/parameters/input_boost_duration_ms", string_t(buf));
            utils.FileWriteBlocking("/sys/module/cpu_input_boost/parameters/enabled", string_t("1"));
            utils.FileWriteBlocking("/sys/module/cpu_input_boost/parameters/input_boost_enabled", string_t("1"));
            logger.Info("触控加速: 已开启 input boost [%s] %dms", freqList.c_str(), dur);
        } else {
            utils.FileWriteBlocking("/sys/module/cpu_boost/parameters/input_boost_enabled", string_t("0"));
            utils.FileWriteBlocking("/sys/module/cpu_input_boost/parameters/enabled", string_t("0"));
            utils.FileWriteBlocking("/sys/module/cpu_input_boost/parameters/input_boost_enabled", string_t("0"));
        }
    }

    // BG_TRIM: 周期回收后台内存(软回收不杀进程)。仅 Function.BgTrim.enable 时真正动作。
    void bgTrimTask() {
        if (waitStop(20000)) return;   // 等系统起来
        while (!shouldExit_.load()) {
            int iv = Config::BgTrim::interval_s;
            if (iv < 30) iv = 30;
            if (Config::BgTrim::enable) {
                std::string top;
                { std::lock_guard<std::recursive_mutex> lk(applyMtx); top = lastTopApp; }
                // 息屏判定改用 autoMode_ 的缓存 fd 读背光(取代每轮 dumpsys power 派生进程)。
                // screenState(): 1=亮 0=灭 -1=未知; 仅 0(确定息屏)才传 true, -1 按亮屏处理(同旧默认)。
                sysTweaks_.bgTrimTick(top, autoMode_.screenState() == 0);
            }
            if (waitStop(iv * 1000)) break;
        }
    }

    // 找包主进程 pid: 扫 /proc/<pid>/cmdline 与包名全等(同 Utils::isPackageRunning 的判法,
    // 排除 pkg:sub 子进程)。不走 popen/pidof → 无 shell、无命令注入面。找不到返回 -1。
    int findGamePid(const std::string& pkg) {
        if (pkg.empty()) return -1;
        // 进入/换游戏偶尔会在同一前台事件风暴中重复触发；成功 PID 在 1s 内
        // 足够稳定，避免重复全表扫描 /proc。失败不缓存，确保新启动进程及时可见。
        static std::mutex pidCacheMtx;
        static std::string cachedPkg;
        static int cachedPid = -1;
        static long cachedAtMs = 0;
        const long now = nowMs();
        {
            std::lock_guard<std::mutex> lk(pidCacheMtx);
            if (cachedPid > 0 && cachedPkg == pkg && now - cachedAtMs < 1000)
                return cachedPid;
        }
        DIR* d = opendir("/proc");
        if (!d) return -1;
        int found = -1;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            bool num = true;
            for (const char* p = e->d_name; *p; ++p)
                if (*p < '0' || *p > '9') { num = false; break; }
            if (!num) continue;
            char cmdPath[64];
            FastSnprintf(cmdPath, sizeof(cmdPath), "/proc/%s/cmdline", e->d_name);
            int cfd = open(cmdPath, O_RDONLY | O_CLOEXEC);
            if (cfd < 0) continue;
            char cmd[256] = { 0 };
            ssize_t cn = read(cfd, cmd, sizeof(cmd) - 1);
            close(cfd);
            if (cn <= 0) continue;
            cmd[cn] = '\0';
            if (strchr(cmd, ':')) continue;   // pkg:push 之类子进程 → 只要主进程
            if (pkg == cmd) { found = atoi(e->d_name); break; }
        }
        closedir(d);
        if (found > 0) {
            std::lock_guard<std::mutex> lk(pidCacheMtx);
            cachedPkg = pkg;
            cachedPid = found;
            cachedAtMs = now;
        }
        return found;
    }

    // 同 Function::stopGuards 风格：置位 → 唤醒(cv + eventfd) → join，成员销毁前线程已全部退出
    ~Schedule() {
        {
            std::lock_guard<std::mutex> lk(stopMtx_);
            shouldExit_.store(true);
        }
        stopCv_.notify_all();
        fsmCv_.notify_all();   // 一并唤醒 FSM 等待, 保证 autoModeTask 能退出
        if (stopEventFd_ >= 0) {
            eventfd_write(stopEventFd_, 1);
        }
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
        sysTweaks_.restoreAll();
        dnd_.release();   // 守护退出/重载: 把免打扰还回用户原值, 别把用户留在静音状态
        if (stopEventFd_ >= 0) {
            close(stopEventFd_);
            stopEventFd_ = -1;
        }
    }

    // 可被析构即时打断的睡眠；返回 true 表示收到停止请求
    bool waitStop(int ms) {
        std::unique_lock<std::mutex> lk(stopMtx_);
        return stopCv_.wait_for(lk, std::chrono::milliseconds(ms),
                                [this] { return shouldExit_.load(); });
    }

    // FSM 专用睡眠: 到点 / 收到前台变化唤醒 / 退出 任一发生即返回。返回 true = 应退出。
    // 被前台变化唤醒时消费标记并返回 false, 调用方据此立即开始下一 tick 重采样。
    bool waitFsmTick(int ms) {
        std::unique_lock<std::mutex> lk(fsmMtx_);
        fsmCv_.wait_for(lk, std::chrono::milliseconds(ms),
                        [this] { return shouldExit_.load() || fsmWake_.load(); });
        if (shouldExit_.load()) return true;
        fsmWake_.store(false);
        return false;
    }
    // 唤醒 FSM 立即重采样(前台变化时调用)。先在锁内置位再 notify, 防丢唤醒。
    void wakeFsm() {
        { std::lock_guard<std::mutex> lk(fsmMtx_); fsmWake_.store(true); }
        fsmCv_.notify_all();
    }


    // 开机重应用：ROM 的 perf HAL / 开机提频晚于本守护启动, 会把我们应用的频率/调速器顶高,
    // 而去重缓存挡住自愈 → 卡高频, 要手动切模式才好。开机后分几次强制重应用(applyAllConfig
    // 内含 invalidateFreqCache), 等 ROM 稳定。可被析构打断。
    void bootResettleTask() {
        const int delaysMs[] = { 8000, 20000, 45000 };
        for (int d : delaysMs) {
            if (waitStop(d)) return;
            logger.Debug("开机重应用: 覆盖 ROM 开机提频, 强制重应用当前配置");
            applyAllConfig();
        }
    }

    // 必须 size 感知：clear() 只置 _size=0 不动缓冲区，仅比 c_str() 会把"已清空"误判为
    // 等于旧值，导致 invalidateFreqCache()/漂移纠正后同值重写被去重跳过（"降不回/不生效"根因）。
    static inline bool str_eq(const string_t& a, const string_t& b) {
        return a.size() == b.size() && strcmp(a.c_str(), b.c_str()) == 0;
    }

    // 传引用避免每次调用 3 个 string_t 堆拷贝
    void FreqWriter(const int Policy, const string_t& MinFreq, const string_t& MaxFreq, const string_t& Governor) {
        std::lock_guard<std::mutex> lock(mtx);
        int cluster = -1;
        for (int i = 0; i < kClusterCount; i++) {
            if (Config::Policy::CpuPolicy[i] == Policy) { cluster = i; break; }
        }

        char path[256];
        // affected_cpus 为空说明整簇已 offline，写 governor 会报错，整簇跳过。
        // 【必须在去重之前判定】: 否则离线簇被以"与缓存相同的值"重写时会命中下方去重 return 而不清缓存,
        // 等簇重新上线后同值仍被去重跳过 → governor/频率永不重应用(簇失管, 跑 ROM 默认调速器)。
        char affPath[256];
        FastSnprintf(affPath, sizeof(affPath),
                     "/sys/devices/system/cpu/cpufreq/policy%d/affected_cpus", Policy);
        if (!utils.FileStartsWithDigit(affPath)) {
            logger.Debug("CPU簇: %d 无在线核,跳过 governor/频率", Policy);
            // 离线跳过时清缓存，确保核心重新上线后必写（否则被去重永久跳过）
            if (cluster >= 0) {
                lastMinFreq[cluster].clear();
                lastMaxFreq[cluster].clear();
                lastGovernor[cluster].clear();
            }
            return;
        }

        // 不以 last* 内存缓存短路：ROM/其他模块可能在两拍之间改写节点，
        // 必须每次通过统一的读后比较核对实际 sysfs 值。last* 仅记录最近一次成功值，
        // 供外部切换/离线重上线时同步状态，不是节点所有权证明。

        // 先写 governor 再写 min/max：切调速器时内核可能重置频率。
        bool govOk = true;
        if (!Governor.empty()) {
            FastSnprintf(path, sizeof(path), GovernorPath, Policy);
            bool ok = utils.FileWriteIfChangedBlocking(path, Governor);
            if (!ok) {
                // 配置的调速器不被当前 SoC 支持时，回退到默认（高通 walt / 其他 sugov_ext）
                string_t fb = function.checkQcom() ? "walt" : "sugov_ext";
                if (fb != Governor) {
                    if (utils.FileWriteIfChangedBlocking(path, fb)) {
                        logger.Warn("CPU簇: %d 调速器 %s 不可用, 已回退 %s",
                                    Policy, Governor.c_str(), fb.c_str());
                        ok = true;
                    }
                }
            }
            if (ok) logger.Debug("CPU簇: %d 调速器: %s", Policy, Governor.c_str());
            else    logger.Warn("CPU簇: %d 调速器写入失败: %s (节点不可写或调速器不存在)", Policy, Governor.c_str());
            govOk = ok;
        }

        // 写失败时不更新去重缓存，否则同值重试会被永久跳过
        bool freqOk = true;
        if (!MinFreq.empty()) {
            FastSnprintf(path, sizeof(path), MinFreqPath, Policy);
            if (!utils.FileWriteIfChangedBlocking(path, MinFreq)) freqOk = false;
            logger.Debug("CPU簇: %d 最小频率: %s", Policy, MinFreq.c_str());
        }

        if (!MaxFreq.empty()) {
            FastSnprintf(path, sizeof(path), MaxFreqPath, Policy);
            if (!utils.FileWriteIfChangedBlocking(path, MaxFreq)) freqOk = false;
            logger.Debug("CPU簇: %d 最大频率: %s", Policy, MaxFreq.c_str());
        }

        if (cluster >= 0 && govOk && freqOk) {
            lastMinFreq[cluster]  = MinFreq;
            lastMaxFreq[cluster]  = MaxFreq;
            lastGovernor[cluster] = Governor;
        }
    }

    // 非游戏档 governor 兜底：画像留空 → 基础模式 → SoC 默认(高通 conservative / 其他 sugov_ext)。
    // 防外部(如 Scene)把 config 里的 governor 清空后, 该簇被 daemon 放手、随 ROM 漂移到 walt。
    // 游戏档不走此兜底：留空表示交给 ROM(风驰/hmbird)。
    string_t resolveGovernor(int clusterIdx, const string_t& want) {
        if (!want.empty()) return want;
        if (!Performances::CpuGovernor[clusterIdx].empty())
            return Performances::CpuGovernor[clusterIdx];
        return function.checkQcom() ? "conservative" : "sugov_ext";
    }

    // ── 游戏态调速器切换(Config::AutoMode::gaming_gov): 前台命中 game_heavy(风驰游戏)画像即触发。
    // 进入沿不立即切 —— 调用方(tick 循环)先启动 oiface, 3s 后检查是否已是原生风驰, 不是才调本函数强切。
    // 目标 = 偏好列表(逗号分隔, 如 "hm,scx")中本机 scaling_available_governors 里存在的第一个;
    // 离开 game_heavy/退出智能模式/守护退出时还原。还原是条件写: 若期间外部改过 governor
    // (切档/画像/第三方)则不覆盖。调用方须持 applyMtx。──

    // 读小文件(sysfs 单行)到 string_t; 失败返回空。
    static string_t readSmallFile(const char* path) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return string_t();
        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return string_t();
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) n--;
        buf[n] = '\0';
        return string_t(buf);
    }

#if 0
    // 旧版 Kin 强切 governor 实现，仅保留历史参考；ROM 放权方案禁止编译和调用。
    // avail(空格分词)里是否含整词 word(长度 wl)
    static bool governorListed(const string_t& avail, const char* word, size_t wl) {
        const char* a = avail.c_str();
        size_t as = 0, alen = avail.size();
        while (as < alen) {
            size_t ae = as;
            while (ae < alen && a[ae] != ' ') ae++;
            if (ae - as == wl && strncmp(a + as, word, wl) == 0) return true;
            as = ae + 1;
        }
        return false;
    }

    // 从逗号分隔偏好列表挑第一个该 policy 可用的调速器(对照 scaling_available_governors 整词匹配);
    // 列表全不可用时回退: 高通 walt / 其它(天玑等) sugov_ext(回退项也须本机存在); 仍无则空。
    // fromPref 非空时输出"选中的是否偏好列表项"(true=风驰 hm/hmbird/scx 之类, false=walt/sugov_ext 回退)
    // —— 供互斥判定区分"风驰真在位"与"仅回退"(回退机型 game_heavy 仍应享受 FAS)。
    string_t pickAvailGovernor(int policy, const string_t& prefCsv, bool* fromPref = nullptr) {
        if (fromPref) *fromPref = false;
        char path[192];
        FastSnprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpufreq/policy%d/scaling_available_governors", policy);
        string_t avail = readSmallFile(path);
        if (avail.empty()) return string_t();
        const char* csv = prefCsv.c_str();
        size_t start = 0, len = strlen(csv);
        while (start <= len) {
            size_t comma = start;
            while (comma < len && csv[comma] != ',') comma++;
            size_t b = start, e = comma;
            while (b < e && (csv[b] == ' ' || csv[b] == '\t')) b++;
            while (e > b && (csv[e-1] == ' ' || csv[e-1] == '\t')) e--;
            if (e > b && e - b < 64 && governorListed(avail, csv + b, e - b)) {
                char wbuf[64];
                memcpy(wbuf, csv + b, e - b);
                wbuf[e - b] = '\0';
                if (fromPref) *fromPref = true;
                return string_t(wbuf);
            }
            if (comma >= len) break;
            start = comma + 1;
        }
        // 回退: scx/hm(hmbird) 都没有 → 高通 walt, 天玑等其它平台 sugov_ext
        const char* fb = function.checkQcom() ? "walt" : "sugov_ext";
        if (governorListed(avail, fb, strlen(fb))) return string_t(fb);
        return string_t();
    }

    // 单个调速器名是否在偏好列表(gaming_gov, 逗号分隔, trim 空白)中。
    // string_t 是 qlib::string::value<char>, 没有 std::string 的 find/substr —— 手动扫描,
    // 与 nativeFengchiGovRunning 同 idiom。供风驰漂移检查判定"当前簇是否还是风驰调速器"。
    bool govInPrefList(const string_t& cur) {
        if (cur.empty()) return false;
        const string_t& pref = Config::AutoMode::gaming_gov;
        if (pref.empty()) return false;
        const char* csv = pref.c_str();
        size_t len = strlen(csv);
        size_t start = 0;
        while (start <= len) {
            size_t comma = start;
            while (comma < len && csv[comma] != ',') comma++;
            size_t b = start, e = comma;
            while (b < e && (csv[b] == ' ' || csv[b] == '\t')) b++;
            while (e > b && (csv[e-1] == ' ' || csv[e-1] == '\t')) e--;
            if (e > b && cur.size() == (e - b) &&
                strncmp(cur.c_str(), csv + b, e - b) == 0)
                return true;
            if (comma >= len) break;
            start = comma + 1;
        }
        return false;
    }

    // 检查各簇当前调速器是否已有任一为偏好列表(hm/hmbird/scx 等)中的项 —— 即"原生风驰"在位。
    // 供进入风驰游戏态 3s 后检查: ROM/内核已自动切成风驰则无需再切(只记日志"原生风驰启动成功")。
    // 注意: 只比对偏好列表项, 回退项(walt/sugov_ext)不算原生风驰。
    bool nativeFengchiGovRunning() {
        const string_t& pref = Config::AutoMode::gaming_gov;
        if (pref.empty()) return false;
        const char* csv = pref.c_str();
        size_t len = strlen(csv);
        char path[192];
        for (int i = 0; i < kClusterCount; i++) {
            int pol = Config::Policy::CpuPolicy[i];
            if (pol < 0) continue;
            FastSnprintf(path, sizeof(path), GovernorPath, pol);
            string_t cur = readSmallFile(path);
            if (cur.empty()) continue;
            // 逐项比对 pref 列表(逗号分隔, trim 空白)
            size_t start = 0;
            while (start <= len) {
                size_t comma = start;
                while (comma < len && csv[comma] != ',') comma++;
                size_t b = start, e = comma;
                while (b < e && (csv[b] == ' ' || csv[b] == '\t')) b++;
                while (e > b && (csv[e-1] == ' ' || csv[e-1] == '\t')) e--;
                if (e > b && cur.size() == (e - b) &&
                    strncmp(cur.c_str(), csv + b, e - b) == 0)
                    return true;
                if (comma >= len) break;
                start = comma + 1;
            }
        }
        return false;
    }

    // wantGaming 边沿驱动; 幂等, 每 tick 调也只在状态翻转时动 sysfs。
    // pending 补切: 进入沿时簇离线(节点读不到, 常见于关核停泊后直进游戏)→ 记 pending,
    // 后续 tick 簇上线后补写(仅对 pending 簇, 不重扫全部)。「本机无此调速器」不进 pending(无望目标不重试)。
    void applyGamingGovLegacy(bool wantGaming) {
        const string_t& pref = Config::AutoMode::gaming_gov;
        if (pref.empty()) wantGaming = false;             // 功能关: 已切换则还原
        bool pendAny = false;
        for (int i = 0; i < kClusterCount; i++) pendAny = pendAny || gamingGovPend_[i];
        if (wantGaming == gamingGovActive_ && !(wantGaming && pendAny)) return;
        char path[192];
        if (wantGaming) {
            const bool entering = !gamingGovActive_;
            bool any = false;
            bool present = false;   // 风驰真在位: 任一簇的【偏好项】(hm/hmbird/scx)已切成或本就在跑
            for (int i = 0; i < kClusterCount; i++) {
                if (!entering && !gamingGovPend_[i]) continue;   // 补切只碰 pending 簇
                if (entering) { gamingGovSaved_[i].clear(); gamingGovSet_[i].clear(); gamingGovPend_[i] = false; }
                int pol = Config::Policy::CpuPolicy[i];
                if (pol < 0) continue;
                FastSnprintf(path, sizeof(path), GovernorPath, pol);
                string_t cur = readSmallFile(path);
                if (cur.empty()) {                        // 簇离线/节点缺失 → 待上线补切
                    gamingGovPend_[i] = true;
                    continue;
                }
                gamingGovPend_[i] = false;
                bool fromPref = false;
                string_t pick = pickAvailGovernor(pol, pref, &fromPref);
                if (pick.empty()) continue;               // 本机没有列表里的调速器: 该簇不动
                if (str_eq(cur, pick)) {                  // 已是目标: 不切不存, 但偏好项在跑也算在位
                    if (fromPref) present = true;
                    continue;
                }
                if (utils.FileWriteIfChangedBlocking(path, pick)) {
                    gamingGovSaved_[i] = cur;
                    gamingGovSet_[i]   = pick;
                    // 同步 FreqWriter 去重缓存(受其 mtx 保护), 防错误短路;
                    // 顺带清频率去重 —— governor 切换会重置内核 min/max, 下一拍 caps 必须真重写。
                    { std::lock_guard<std::mutex> g(mtx);
                      lastGovernor[i] = pick;
                      lastMinFreq[i].clear(); lastMaxFreq[i].clear(); }
                    any = true;
                    if (fromPref) present = true;
                    logger.Info("游戏态调速器: 簇%d %s → %s%s", pol, cur.c_str(), pick.c_str(),
                                entering ? "" : " 补切");
                }
            }
            gamingGovActive_ = true;                      // 即使一个没切成也置位, 避免每 tick 重试刷日志
            if (entering) gamingGovEngaged_ = present;    // 供 FAS 互斥判定: 仅回退(walt/sugov_ext)不算风驰在位
            else if (present) gamingGovEngaged_ = true;   // 补切成功 → 在位升级(FAS 互斥随之生效)
            // 进入游戏态: 矫正 cpuset 到全核 (天玑/骁龙通用, 对抗 OEM 收窄大核)
            if (entering) correctGameTopAppCpus();
            bool pendNow = false;
            for (int i = 0; i < kClusterCount; i++) pendNow = pendNow || gamingGovPend_[i];
            if (entering && !any && !pendNow)
                logger.Debug("游戏态调速器: 偏好[%s] 本机均不可用, 未切换", pref.c_str());
        } else {
            bool restorePending = false;
            for (int i = 0; i < kClusterCount; i++) {
                gamingGovPend_[i] = false;                // 退出沿: 清 pending(未成簇无 saved, 不留悬空)
                if (gamingGovSaved_[i].empty()) continue;
                int pol = Config::Policy::CpuPolicy[i];
                if (pol < 0) { gamingGovSaved_[i].clear(); gamingGovSet_[i].clear(); continue; }
                FastSnprintf(path, sizeof(path), GovernorPath, pol);
                string_t cur = readSmallFile(path);
                // 条件还原: 仍是我们写的值才还原; 外部已改则不覆盖别人的设置。
                // 节点暂时不可读或写入失败时保留 saved/set，下一次恢复沿重试，
                // 避免一次瞬时 sysfs 故障让 Kin 永久丢失原 governor。
                bool released = false;
                if (!cur.empty() && str_eq(cur, gamingGovSet_[i])) {
                    if (utils.FileWriteIfChangedBlocking(path, gamingGovSaved_[i])) {
                        { std::lock_guard<std::mutex> g(mtx);
                          lastGovernor[i] = gamingGovSaved_[i];
                          lastMinFreq[i].clear(); lastMaxFreq[i].clear(); }
                        logger.Info("游戏态调速器: 簇%d 还原 %s", pol, gamingGovSaved_[i].c_str());
                        released = true;
                    }
                } else if (!cur.empty()) {
                    // 外部已修改节点，放弃所有权但不覆盖外部设置。
                    released = true;
                }
                if (released) {
                    gamingGovSaved_[i].clear();
                    gamingGovSet_[i].clear();
                } else if (!gamingGovSaved_[i].empty()) {
                    restorePending = true;
                }
            }
            // Keep the transition logically active while a conditional restore is
            // pending, so the next call retries instead of returning early.
            gamingGovActive_ = restorePending;
            gamingGovEngaged_ = false;
        }
    }
#endif

    // 游戏态 governor 放权：只记录 ROM 接管窗口，不写入或还原 scaling_governor。
    // 与动态限频游戏态一致，OIface/ROM/风驰可自行选择 hm/hmbird/scx 等调速器。
    void applyGamingGov(bool wantGaming, const char* = "") {
        const bool enabled = !Config::AutoMode::gaming_gov.empty();
        gamingGovActive_ = wantGaming && enabled;
        gamingGovEngaged_ = gamingGovActive_;
    }

    // 返回 core 所属的 policy（起始核 <= core 的最大 CpuPolicy），找不到返回 -1
    int policyForCore(int core) {
        int best = -1;
        for (int i = 0; i < kClusterCount; i++) {
            int p = Policy::CpuPolicy[i];
            if (p != -1 && p <= core && p > best) best = p;
        }
        return best;
    }

    // ── P2-⑥ top-app uclamp.min 下限(0-100, 0=还原默认) ──
    // cgroup cpu 控制器的 uclamp 接口: 给 top-app 组设性能下限, 同时抬调频(schedutil 直接响应)
    // 与选核(EAS 把高需求任务放大核)。仅前台受益、后台不跟着抬频。边沿驱动(仅变化才写)。
    // 节点候选(cgroup v1 /dev/cpuctl 与 v2 /sys/fs/cgroup); 首次全探不到则永久禁用(uclampAvail_=0)。
    int uclampAvail_ = -1;                 // -1=未探测 0=不可用 1=可用
    int uclampMinState_ = -1;              // 当前已写的数值(-1=从未由我们写过)
    bool uclampApplied_ = false;           // 我们是否正持有一个非默认下限(决定是否需要还原)
    string_t uclampOrig_;                  // 首次介入前的节点原值(还原写回它, 不写死 0)
    const char* uclampNode_ = nullptr;
    // pct>0: 给 top-app 设性能下限; pct==0: 还原到【进入前的原值】(而非硬写 0, 防清掉 OEM 前台基线)。
    // 从未写过非默认值时 pct==0 直接 no-op, 绝不主动改写平台默认。对齐 applyGamingGov 的保存-还原语义。
    void applyTopAppUclampMin(int pct) {
        if (uclampAvail_ == 0) return;
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        if (uclampAvail_ < 0) {
            static const char* cands[] = {
                "/dev/cpuctl/top-app/cpu.uclamp.min",
                "/sys/fs/cgroup/cpu/top-app/cpu.uclamp.min",
                nullptr,
            };
            for (const char** c = cands; *c; ++c) {
                struct stat st;
                if (stat(*c, &st) == 0) { uclampNode_ = *c; break; }
            }
            uclampAvail_ = uclampNode_ ? 1 : 0;
            if (!uclampAvail_) return;
        }
        // 接管前置条件: **读不到原值就不接管** —— 否则退出时既无法还原, 又会把我们的值永久留在
        // 节点上(相机保持同款规则: "读不到原值则跳过接管")。原值为空时本拍不动, 下拍重新快照。
        if (uclampOrig_.empty()) {
            uclampOrig_ = readSmallFile(uclampNode_);   // 快照原值, 还原时写回
            if (uclampOrig_.empty()) return;
        }
        if (pct <= 0) {
            if (!uclampApplied_) return;                // 从未介入 → 不碰平台默认
            // 还原: 仅当节点仍是我们写的值(外部/游戏引擎改过则不覆盖); 写回原值。
            // 原值已在接管前确认可读, 这里不再需要 empty 例外。
            string_t cur = readSmallFile(uclampNode_);
            char mine[16]; snprintf(mine, sizeof(mine), "%d", uclampMinState_);
            const bool stillOwned = cur.empty() || str_eq(cur, string_t(mine));
            const bool restored = !stillOwned ||
                utils.FileWriteIfChangedBlocking(uclampNode_, uclampOrig_);
            uclampApplied_ = kin::releaseOwned(uclampApplied_, stillOwned, restored);
            if (!uclampApplied_) uclampMinState_ = -1;
            return;
        }
        if (uclampApplied_ && pct == uclampMinState_) return;   // 边沿: 同值不重写
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%d", pct);   // 接口接受百分比整数(0..100)
        if (n > 0 && utils.FileWriteIfChangedBlocking(uclampNode_, string_t(buf))) {
            uclampMinState_ = pct; uclampApplied_ = true;
        }
    }

    // ── 息屏 DEEP_IDLE 背景 uclamp 上限(移植 ASB V64) ──
    // 只压不抬, 用 OwnedNode 记原值; 离开 DEEP_IDLE / 关开关时条件还原(被外部改过就不覆盖)。
    // 内核无 background uclamp.max 接口则整段跳过 —— 不写不存在的节点。
    const char* bgUclampNode_[2] = { nullptr, nullptr };
    bool        bgUclampProbed_  = false;
    kin::OwnedNode bgUclampOwned_[2];
    void applyDeepIdleBgUclamp(bool deepIdle) {
        if (!bgUclampProbed_) {
            bgUclampProbed_ = true;
            static const char* const cands[2][3] = {
                { "/dev/cpuctl/background/cpu.uclamp.max",
                  "/sys/fs/cgroup/background/cpu.uclamp.max", nullptr },
                { "/dev/cpuctl/system-background/cpu.uclamp.max",
                  "/sys/fs/cgroup/system-background/cpu.uclamp.max", nullptr },
            };
            for (int g = 0; g < 2; ++g)
                for (const char* const* c = cands[g]; *c; ++c) {
                    struct stat st;
                    if (stat(*c, &st) == 0) { bgUclampNode_[g] = *c; break; }
                }
            if (!bgUclampNode_[0] && !bgUclampNode_[1])
                logger.Info("息屏背景 uclamp: 内核无 background uclamp.max 接口, 跳过(不写入不存在的节点)");
        }
        const bool wantCap = deepIdle && Config::AutoMode::bg_uclamp_enable;
        for (int g = 0; g < 2; ++g) {
            const char* node = bgUclampNode_[g];
            if (!node) continue;
            auto rd = [node] { return std::string(readSmallFile(node).c_str()); };
            auto wr = [this, node](const std::string& v) {
                return utils.FileWriteIfChangedBlocking(node, string_t(v.c_str()));
            };
            if (!wantCap) {
                if (!bgUclampOwned_[g].held()) continue;
                if (!bgUclampOwned_[g].restore(rd, wr))
                    logger.Debug("息屏背景 uclamp: %s 还原未完成, 下拍重试", node);
                continue;
            }
            const std::string cur = rd();
            const int curPct = cur.empty() ? -1 : atoi(cur.c_str());
            const int target = kin::deepIdleBgUclampTarget(curPct, true, Config::AutoMode::bg_uclamp_cap);
            if (target < 0) continue;   // 已够低 / 读不到 / 关闭 → 不动
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", target);
            if (!bgUclampOwned_[g].apply(std::string(buf), rd, wr))
                logger.Debug("息屏背景 uclamp: %s 写入失败", node);
        }
    }

    // ── 功率驱动 boost/压制(移植 powerd: UCLAMP 三级回退控制 boost, 功率作压制边界) ──
    // UCLAMP 接口优先级(powerd 式, 逐级回退)。语义详见 docs/kernel-boost-research.md 第一节:
    //   1) cgroup v2 cpu.uclamp.min(/sys/fs/cgroup, 全局下限, 最精细)。单位【百分比 0-100】非 0-1024。
    //   2) /proc/sys/kernel/sched_util_clamp_min: "系统允许的 UCLAMP_MIN 上限"(默认 1024), 非 boost 源;
    //      写小值反而钳死全系统下限, 故 sysctl 模式不碰它, boost 交给 sched_boost。
    //   3) 回退 /proc/sys/kernel/sched_boost(老接口, 只能开/关 boost)。
    // 注: sched_util_clamp_max 语义是"允许的 UCLAMP_MAX 上限"(非"允许下限"), 且 max 聚合下挡不住频率, 故 cap 一律走 cpufreq(见 FSM 压制段 + 关核)。
    //   卡顿 → 抬高(开 boost, 内核给任务频率底线补响应); 高功耗 → 压 0(关 boost)。
    // 边沿驱动(仅档位变化才写)。auto 启用接管此节点, 退出还原各节点原值。
    // 内核全无接口则 boost 段跳过(首次探测失败永久禁用); 压制段走 caps/关核/GPU 不受影响。
    // 返回功率档位: 0=卡顿(开boost补救) 1=中性(省电) 2=高(关boost+压大核+限GPU) 3=临界(深压+关核+限GPU); -1=旁路。
    int  pwrBoostMode_  = 0;             // 0=未探测/无 1=cgroup 2=sched_boost
    int  pwrBoostState_ = -2;            // 当前已写的档位(-2=从未写过, -1..3=档位)
    kin::OwnedNode pwrBoostNode_;       // Only the selected boost node is owned.
    // ── boost 时间门控(防抖 + 防长期钉高) ──
    // 原实现只有"档位变化才写"的边沿去重, 没有任何时间约束: PSI some avg10 在阈值附近
    // 抖动时会 L0/L1 反复切, 每次都写 uclamp(+旧版还写 sched_boost), 抬频抖动且开销不小。
    // 三条约束: ① 进 boost 后至少驻留 min_ms(防抖) ② 单次最长 max_ms(防 PSI 长期高把频率
    // 永久钉住) ③ 退出后冷却 cool_ms 才允许再进(防紧接着又抬起来)。
    static constexpr long long kPwrBoostMinMs  = 3000;    // 最短驻留
    static constexpr long long kPwrBoostMaxMs  = 15000;   // 单次最长
    static constexpr long long kPwrBoostCoolMs = 10000;   // 退出后冷却
    long long pwrBoostEnterMs_ = 0;      // 本次进 boost 的时刻(0=当前不在 boost)
    long long pwrBoostLeaveMs_ = 0;      // 上次离开 boost 的时刻(0=从未)
    // 记录离开 boost 的时刻(起冷却)。
    void pwrBoostNoteLeave() {
        if (pwrBoostEnterMs_ != 0) { pwrBoostLeaveMs_ = nowMs(); pwrBoostEnterMs_ = 0; }
    }
    // 给 computePwrLevel 的结果套时间门控, 返回最终档位。
    //   level>=2(压制): 立即放行并退出 boost —— 功耗安全优先于防抖。
    //   level<0(旁路):  清进入标记, 由 pwrBoostRestore 还原节点。
    //   level==0(卡顿): 新进须过冷却; 已在 boost 中超 max_ms 则强制退出起冷却。
    //   level==1(中性): 若在 boost 中且未满 min_ms, 继续 boost(防 PSI 在阈值附近抖动导致反复切)。
    int pwrBoostGate(int level) {
        const long long now = nowMs();
        if (level >= 2) { pwrBoostNoteLeave(); return level; }
        if (level < 0)  { pwrBoostEnterMs_ = 0; return level; }
        if (level == 0) {
            if (pwrBoostEnterMs_ == 0) {
                if (pwrBoostLeaveMs_ != 0 && (now - pwrBoostLeaveMs_) < kPwrBoostCoolMs)
                    return 1;                                  // 冷却未满: 不给 boost
                pwrBoostEnterMs_ = now;
                return 0;
            }
            if ((now - pwrBoostEnterMs_) >= kPwrBoostMaxMs) {   // 单次超时: 强退 + 起冷却
                pwrBoostNoteLeave();
                return 1;
            }
            return 0;
        }
        // level == 1(中性)
        if (pwrBoostEnterMs_ != 0 && (now - pwrBoostEnterMs_) < kPwrBoostMinMs) return 0;
        pwrBoostNoteLeave();
        return 1;
    }
    static constexpr const char* kPwrCgUclampMin  = "/sys/fs/cgroup/cpu.uclamp.min";
    // sched_util_clamp_min 只是 UCLAMP_MIN 的"允许上限"(0..1024), 不是 boost 源 ——
    // 保留常量仅作说明: 选型绝不能拿它的存在当依据(见 include/PowerBoostNode.hpp)。
    static constexpr const char* kPwrUclampSysctl = "/proc/sys/kernel/sched_util_clamp_min";
    static constexpr const char* kPwrSchedBoost   = "/proc/sys/kernel/sched_boost";

    // 探测 UCLAMP 接口(逐级回退, 仅探测一次)。返回 1=cgroup 2=sched_boost 0=全无。
    // 必须以【实际写入节点】是否存在为准: 原实现用 sched_util_clamp_min 的存在选中 sched_boost 模式,
    // 在只有前者没后者的内核上会每拍重试一次写不存在的节点。
    int pwrBoostProbe() {
        if (pwrBoostMode_ != 0) return pwrBoostMode_;
        struct stat st;
        const bool cg    = (stat(kPwrCgUclampMin, &st) == 0);
        const bool boost = (stat(kPwrSchedBoost,  &st) == 0);
        pwrBoostMode_ = kin::selectPowerBoostMode(cg, boost);
        return pwrBoostMode_;
    }
    const char* pwrBoostMainNode() {
        switch (pwrBoostMode_) {
            case 1: return kPwrCgUclampMin;
            case 2: return kPwrSchedBoost;
        }
        return nullptr;
    }
    // 还原各节点原值(auto 退出/关功率功能/旁路时调)。仅当我们持有过才还原。
    void pwrBoostRestore() {
        pwrBoostNoteLeave();
        const char* node = pwrBoostMainNode();
        if (!node) return;
        if (pwrBoostNode_.restore(
                [&] { return std::string(readSmallFile(node).c_str()); },
                [&](const std::string& value) {
                    return utils.FileWriteIfChangedBlocking(node, string_t(value.c_str()));
                })) {
            pwrBoostState_ = -2;
        }
    }
    // 由本 tick 采样算功率档位(采样融入 auto 决策)。
    // 返回 -1=旁路(关/充电/相机保持/读不到) 0=卡顿(开 boost 补救) 1=中性(省电) 2=高(压大核+限GPU) 3=临界(关核+限GPU)。
    // boost 只在卡顿且功耗<throttle_w 时开(补救响应性); 不卡顿则中性省电; 高/临界功耗优先压制(关 boost)。
    // 卡顿判据(任一): PSI cpu some ≥ jank_psi; 或无 PSI 时 cpuBusyW ≥ jank_busy 且 load1 ≥ jank_load。
    // 同步刷新 lastPwrW_(日志用)。gating: power_enable + 非充电 + powerW 可读 + 非相机保持态。
    int computePwrLevel(const AutoModeCtl::Metrics& mx) {
        if (!Config::AutoMode::power_enable || cameraHoldActive_ ||
            mx.charging || mx.powerW < 0) return -1;
        lastPwrW_ = mx.powerW;
        const float pw = (float)mx.powerW;
        if (pw >= Config::AutoMode::power_throttle_critical_w) return 3;   // 临界: 深压+关核
        if (pw >= Config::AutoMode::power_throttle_w)        return 2;   // 高功耗: 压制(关 boost)
        // ── boost 前置门控: 没人看屏幕 / FSM 判定空闲 → 永不 boost ──
        // 压制档(2/3)在上面已 return, 不受本门控影响: 息屏后台重载仍要压。
        // 息屏时 PSI 极易被几个后台线程(推送/同步/音乐解码)在小核排队顶高, 而此时没有
        // 任何"响应性"需要补救 —— 抬频纯属浪费。DEEP_IDLE/LIGHT_IDLE 同理(静态阅读/挂机)。
        if (mx.screenOn == 0) return 1;
        {
            const int st = autoFsm_.state();   // state() 返回 int, 枚举值隐式转换比较
            if (st == AutoFsm::DEEP_IDLE || st == AutoFsm::LIGHT_IDLE) return 1;
        }
        // 功耗未到压制阈值: 卡顿才开 boost, 否则中性省电
        if (Config::AutoMode::power_boost_jank_enable && detectJank(mx)) return 0;
        return 1;   // 中性: 不 boost, 省电
    }
    // 卡顿检测: PSI 优先(最准, 直接反映任务等 CPU); 无 PSI 回退到 busy+load 双确认。
    // 绝对需求确认(PSI 分支专用): PSI 衡量的是【相对争用】, 不等于【真需要更多算力】——
    // 低频/轻载下几个后台线程在小核排队就能把 PSI some avg10 顶到两位数。故 PSI 过阈后
    // 还须叠加一个绝对需求下限: 能效加权负载 ≥ busy_floor(读不到则回退 load1), 否则不算
    // "需要抬频的卡顿"。busy_floor=0 表示关掉本确认(退回旧的纯 PSI 行为)。
    bool jankDemandOk(const AutoModeCtl::Metrics& mx) {
        const int floorPct = Config::AutoMode::power_boost_jank_busy_floor;
        if (floorPct <= 0) return true;                          // 0=关闭需求确认
        if (mx.cpuBusyW >= 0) return mx.cpuBusyW >= floorPct;    // 有加权负载: 用它判
        return mx.load1 >= Config::AutoMode::power_boost_jank_load;   // 读不到: 回退 load1
    }
    bool detectJank(const AutoModeCtl::Metrics& mx) {
        // PSI 优先(最准), 但必须过绝对需求下限, 避免空载高 PSI 把频率抬起来。
        if (mx.cpuPsi >= Config::AutoMode::power_boost_jank_psi) return jankDemandOk(mx);
        if (mx.cpuPsi < 0) {   // 无 PSI 节点: 回退到负载双确认(busy 高 且 load 高 才算卡顿, 避免单信号误判)
            if (mx.cpuBusyW >= Config::AutoMode::power_boost_jank_busy &&
                mx.load1    >= Config::AutoMode::power_boost_jank_load) return true;
        }
        return false;
    }
    // 应用功率 boost 档位。level: -1=旁路(还原) 0=卡顿(开boost) 1=中性(省电) 2=高 3=临界。
    // 仅 level 变化才写。cgroup 模式: 卡顿档 power_boost_value 换算百分比写 cpu.uclamp.min, 其余写 0;
    // sysctl 模式: 不碰 sched_util_clamp_min(非 boost 源), boost 仅靠 sched_boost;
    // Modes 2/3 write only sched_boost; mode 1 writes only cgroup uclamp.
    void applyPowerBoost(int level) {
        if (level < 0) { pwrBoostRestore(); return; }             // 旁路: 还原
        if (pwrBoostProbe() == 0) return;                         // 无任何接口: boost 段跳过
        if (level == pwrBoostState_) return;                      // 边沿: 同档不重写
        const bool wantOn = (level == 0);                         // 仅卡顿档开 boost(补救响应性)
        const char* mainNode = pwrBoostMainNode();
        if (!mainNode) return;
        char buf[16];
        if (pwrBoostMode_ == 1) {
            // cgroup v2 cpu.uclamp.min 单位是【百分比 0-100】(非 0-1024): 直接写 128 会被内核
            // -ERANGE 拒绝, boost 静默失效 → 换算成百分比。详见 docs/kernel-boost-research.md 第二节。
            int v = wantOn ? (Config::AutoMode::power_boost_value * 100 / 1024) : 0;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            snprintf(buf, sizeof(buf), "%d", v);
        } else {
            snprintf(buf, sizeof(buf), "%d", wantOn ? 1 : 0);
        }
        const bool ok = pwrBoostNode_.apply(buf,
            [&] { return std::string(readSmallFile(mainNode).c_str()); },
            [&](const std::string& value) {
                return utils.FileWriteIfChangedBlocking(mainNode, string_t(value.c_str()));
            });
        if (ok) {
            pwrBoostState_ = level;
            if (!wantOn) pwrBoostNoteLeave();     // 离开 boost: 起冷却计时
            if (wantOn) logger.Info("功率 boost: 开 (卡顿补救, %dW, min=%d, uclamp=%s)", lastPwrW_,
                        Config::AutoMode::power_boost_value,
                        pwrBoostMode_ == 1 ? "cgroup" : "sched_boost");
            else if (level >= 2) logger.Info("功率 boost: 关+压制 (%dW ≥ %.1fW)", lastPwrW_,
                        (double)(level == 3 ? Config::AutoMode::power_throttle_critical_w : Config::AutoMode::power_throttle_w));
        }
    }
    int lastPwrW_ = -1;   // 上次功率 W(日志用)
    bool pwrParkBig_ = false;   // 临界功率(≥critical_w): 强制关大核(保命), reconcileAutoCores 读取

    // ── P3-⑪ 天玑 FPSGO(联发科官方帧感知引擎) ──
    // force_onoff: 0=跟随系统 1=强制开 2=强制关。game_heavy 游戏强开(1), 离开还原(0)。
    // 节点仅 MTK 存在; 非 MTK 首探失败则永久禁用。边沿驱动。
    int  fpsgoAvail_ = -1;                  // -1=未探测 0=不可用 1=可用
    int  fpsgoState_ = -1;                  // 已写值(-1=未知)
    const char* fpsgoNode_ = "/sys/kernel/fpsgo/common/force_onoff";
    bool fpsgoUsable() {                     // 懒探测(只判节点存在, 不写); MTK 才有
        if (fpsgoAvail_ < 0) {
            struct stat st;
            fpsgoAvail_ = (stat(fpsgoNode_, &st) == 0) ? 1 : 0;
        }
        return fpsgoAvail_ == 1;
    }
    void applyFpsgo(bool on) {
        if (!fpsgoUsable()) return;
        int want = on ? 1 : 0;
        if (want == fpsgoState_) return;    // 边沿
        if (utils.FileWriteIfChangedBlocking(fpsgoNode_, string_t(want ? "1" : "0"))) {
            fpsgoState_ = want;
            logger.Info("FPSGO: %s", want ? "强制开(天玑帧感知)" : "还原");
        }
    }

    // ── 相机保持(ASB V60 camera_hold) ──
    // 内置默认相机包名表(用户可在 config.json 的 camera_hold_pkgs 覆盖/扩充, 逗号分隔)。
    // 覆盖常见 AOSP/OPPO/OnePlus/小米/华为/三星/第三方相机包名; 命中其一即认。
    static constexpr const char* const kDefaultCameraPkgs[] = {
        "com.android.camera",           // AOSP / OnePlus 原生
        "com.android.camera2",
        "com.oneplus.camera",           // OnePlus
        "com.oplus.camera",
        "com.coloros.camera",
        "com.oppo.camera",
        "com.android.sec.camera",       // Samsung
        "com.sec.android.app.camera",
        "com.huawei.camera",            // Huawei
        "com.miui.camera",              // Xiaomi
        "com.mediatek.camera",          // MTK
        "com.footej.camera",            // 第三方
        "com.magicadvanced.rootless2",  // 第三方
        "com.geeksoft.camera360",       // 第三方
    };
    static constexpr int kDefaultCameraPkgCount =
        sizeof(kDefaultCameraPkgs) / sizeof(kDefaultCameraPkgs[0]);

    // 前台包名是否是相机 App: 优先用用户配置的 camera_hold_pkgs(逗号分隔), 空则用内置默认表。
    // 同时接受 IMAGE category 兜底(部分相机 App category=IMAGE=3), 但包名命中优先(更精确)。
    bool isCameraApp(const std::string& fgPkg, int fgCat) {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        if (fgPkg.empty() || fgPkg == "null") return false;
        // ① 用户配置表(非空时优先, 可覆盖默认表)
        const string_t& userPkgs = Config::AutoMode::camera_hold_pkgs;
        if (!userPkgs.empty()) {
            // 逗号分隔逐项比对(支持通配符 *, 与黑名单同款 matchPackage 前缀匹配)
            const char* s = userPkgs.c_str();
            const char* e = s + userPkgs.size();
            while (s < e) {
                const char* comma = strchr(s, ',');
                size_t len = comma ? (size_t)(comma - s) : (size_t)(e - s);
                if (len > 0) {
                    // string_t 无 (const char*, size_t) 构造, 用临时 std::string 截取
                    std::string pat(s, len);
                    if (Config::AppProfile::matchPackage(pat.c_str(), fgPkg.c_str())) return true;
                }
                if (!comma) break;
                s = comma + 1;
            }
        } else {
            // ② 内置默认表
            for (int i = 0; i < kDefaultCameraPkgCount; i++) {
                if (Config::AppProfile::matchPackage(kDefaultCameraPkgs[i], fgPkg.c_str())) return true;
            }
        }
        // ③ IMAGE category 兜底(看图/相册类也是 IMAGE, 故仅当未命中包名表时才用, 可能含假阳性)
        //    注: 为降低假阳性(相册≠相机), IMAGE 兜底单独不启用 —— 相机识别只靠包名表。
        //    用户若用冷门相机 App, 可在 camera_hold_pkgs 显式添加。
        (void)fgCat;
        return false;
    }

    // 应用/还原相机保持。边沿驱动: 仅在进入沿(false→true)做记录+接管, 退出沿(true→false)做还原。
    // 接管动作: ① cpuset top-app/foreground 还原全核 ② foreground uclamp.max 抬到 100(不限)
    // 退出动作: 条件还原(仍是我们写的值才还原, 外部改过不覆盖)。
    // scaling_max 软钳由调用方在 FSM caps 计算时叠加(不在此函数, 因 caps 计算在 writeFsmCaps 前)。
    void applyCameraHold(bool wantHold) {
        if (!Config::AutoMode::camera_hold_enable) wantHold = false;
        if (wantHold == cameraHoldActive_) return;   // 边沿: 同态不重写
        if (wantHold) {
            // ── 进入沿: 记录原值 + 接管 ──
            cameraSavedTopAppCpus_  = readSmallFile("/dev/cpuset/top-app/cpus");
            cameraSavedFgCpus_      = readSmallFile("/dev/cpuset/foreground/cpus");
            cameraSavedUclampMaxFg_ = readSmallFile("/dev/cpuctl/foreground/cpu.uclamp.max");
            // cpuset 还原全核: 先放开 root, 再写 top-app/foreground(子集⊆父集)
            char online[64] = {0};
            if (readNodeTrim("/sys/devices/system/cpu/online", online, sizeof(online)) && online[0]) {
                utils.FileWriteIfChangedBlocking("/dev/cpuset/cpus", online);
                utils.FileWriteIfChangedBlocking("/dev/cpuset/top-app/cpus", online);
                utils.FileWriteIfChangedBlocking("/dev/cpuset/foreground/cpus", online);
                logger.Info("相机保持: cpuset top-app/foreground → %s", online);
            }
            // foreground uclamp.max 抬到 100(不限): camera HAL 坐在 foreground, 默认 max 可能被压低
            if (!cameraSavedUclampMaxFg_.empty()) {
                utils.FileWriteIfChangedBlocking("/dev/cpuctl/foreground/cpu.uclamp.max", "100");
            }
            cameraHoldActive_ = true;
        } else {
            // ── 退出沿: 条件还原 ──
            // cpuset: 仅当仍是我们写的值(全核 online)才还原原值
            char online[64] = {0};
            readNodeTrim("/sys/devices/system/cpu/online", online, sizeof(online));
            if (online[0] && !cameraSavedTopAppCpus_.empty()) {
                string_t cur = readSmallFile("/dev/cpuset/top-app/cpus");
                if (str_eq(cur, string_t(online))) {
                    utils.FileWriteIfChangedBlocking("/dev/cpuset/top-app/cpus", cameraSavedTopAppCpus_);
                }
            }
            if (online[0] && !cameraSavedFgCpus_.empty()) {
                string_t cur = readSmallFile("/dev/cpuset/foreground/cpus");
                if (str_eq(cur, string_t(online))) {
                    utils.FileWriteIfChangedBlocking("/dev/cpuset/foreground/cpus", cameraSavedFgCpus_);
                }
            }
            // uclamp.max: 仅当仍是我们写的值(100)才还原
            if (!cameraSavedUclampMaxFg_.empty()) {
                string_t cur = readSmallFile("/dev/cpuctl/foreground/cpu.uclamp.max");
                if (str_eq(cur, string_t("100"))) {
                    utils.FileWriteIfChangedBlocking("/dev/cpuctl/foreground/cpu.uclamp.max", cameraSavedUclampMaxFg_);
                }
            }
            cameraSavedTopAppCpus_.clear();
            cameraSavedFgCpus_.clear();
            cameraSavedUclampMaxFg_.clear();
            cameraHoldActive_ = false;
            logger.Info("相机保持: 退出, 还原 cpuset/uclamp");
        }
    }

    // 风驰游戏态 oiface 保活: 进入风驰沿调用一次(边沿驱动, 非 tick 内重复)。
    // oiface 是 ColorOS/OxygenOS(风驰内核)游戏调度链的关键系统组件, 缺失则风驰调度协同断链。
    // 强拉方式对齐用户提供的脚本: setprop 开启 + init start 拉起。幂等——已存活时 start 无副作用。
    // 【退出风驰不还原】: 用户要求 oiface 保持存活, 退出游戏也不断链(下次进游戏即时可用)。
    void ensureOifaceAlive() {
        if (!Config::AutoMode::oiface_keepalive_enable) return;
        logger.Info("正在恢复oiface服务");
        char buf[64];
        utils.popenRead("setprop persist.sys.oiface.enable 2", buf, sizeof(buf));
        utils.popenRead("start oiface >/dev/null 2>&1", buf, sizeof(buf));
    }

    // 相机保持态下, 把 FSM 算出的 caps 软钳到 camera_hold_max_pct × hwmax/100(够 4K60 又不冲满)。
    // 在 writeFsmCaps 之前调用, 修改 capsToWrite。
    void clampCapsForCamera(AutoFsm::Caps& c) {
        if (!cameraHoldActive_) return;
        const int pct = Config::AutoMode::camera_hold_max_pct;
        if (pct >= 100 || pct < 40) return;   // 100=不钳; 越界不钳(防误)
        int hwL = clusterHwMaxKHz(0);
        int hwB = clusterHwMaxKHz(1);
        if (hwL > 0) { int cap = (int)((long)hwL * pct / 100); if (c.maxL > cap) c.maxL = cap; }
        if (hwB > 0) { int cap = (int)((long)hwB * pct / 100); if (c.maxB > cap) c.maxB = cap; }
    }

    // CPU 上线是异步 hotplug，写 online=1 后 affected_cpus 可能未更新，轮询等待再写 governor
    void waitClusterReady(int policy, int timeoutMs = 200) {
        if (policy < 0) return;
        char p[256];
        FastSnprintf(p, sizeof(p),
                     "/sys/devices/system/cpu/cpufreq/policy%d/affected_cpus", policy);
        int waited = 0;
        while (waited < timeoutMs) {
            if (utils.FileStartsWithDigit(p)) return;
            utils.sleep_ms(8);
            waited += 8;
        }
    }

    // 优先级：AppProfile > Performances；空字段回退到 Performances；GPU/核心仅在画像中定义时才覆盖
    void applyAppProfile(int modelIdx) {
        if (modelIdx < 0 || modelIdx >= Config::AppProfile::modelCount) {
            // 无匹配画像，回到基础 mode
            applyBaseMode();
            return;
        }

        const auto& model = Config::AppProfile::Models[modelIdx];
        const bool romManagedGovernor =
            strcmp(model.modelName.c_str(), "game_heavy") == 0 &&
            !Config::AutoMode::gaming_gov.empty();

        // 先开核心再写 governor/频率，否则离线核收不到 governor 写入
        {
            bool hasCustomOnline = false;
            for (int i = 0; i < kCoreCount; i++) {
                if (model.Online[i] != -1) { hasCustomOnline = true; break; }
            }
            if (hasCustomOnline) {
                char path[256];
                bool broughtOnline = false;
                for (int i = 0; i < kCoreCount; i++) {
                    if (model.Online[i] == -1) continue;
                    FastSnprintf(path, sizeof(path), onlinePath, i);
                    utils.WriteInt(path, model.Online[i]);
                    if (model.Online[i] == 1) broughtOnline = true;
                    logger.Debug("画像核心: %d %s", i, model.Online[i] ? "开启" : "关闭");
                }
                // 等新上线的簇 affected_cpus 稳定后再写频率/调速器，并重落 cpuset
                if (broughtOnline) {
                    for (int i = 0; i < kCoreCount; i++)
                        if (model.Online[i] == 1) waitClusterReady(policyForCore(i));
                    function.cpusetFunction();
                }
            } else {
                // 画像未定义核心开关 → 跟随基础模式，防止省电档的离线核未被拉回
                restoreBaseCoresIfNeeded();
            }
        }

        for (int i = 0; i < kClusterCount; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;

            // 回退：AppProfile -> Performances
            const string_t& minF = !model.MinFreq[i].empty() ? model.MinFreq[i]
                                                              : Performances::MinFreq[i];
            const string_t& maxF = !model.MaxFreq[i].empty() ? model.MaxFreq[i]
                                                              : Performances::MaxFreq[i];
            // governor: 游戏档留空=交给 ROM(风驰); 非游戏档留空则兜底基础模式/默认,
            // 避免被外部(如 Scene)清空后该簇随 ROM 漂移到 walt(用户反馈"一个co一个walt")
            const string_t gov = romManagedGovernor ? string_t()
                               : model.isGame ? model.Governor[i]
                                              : resolveGovernor(i, model.Governor[i]);
            FreqWriter(Policy::CpuPolicy[i], minF, maxF, gov);
        }

        if (!model.GpuMinFreq.empty() || !model.GpuMaxFreq.empty()) {
            const string_t& gpuMin = !model.GpuMinFreq.empty() ? model.GpuMinFreq : GpuFreq::min_freq;
            const string_t& gpuMax = !model.GpuMaxFreq.empty() ? model.GpuMaxFreq : GpuFreq::max_freq;
            function.gpuFreqControlCustom(gpuMin, gpuMax);
            // 守护快照同步为画像目标: 否则 gpuFreqGuard 仍按基础值判"漂移", ~1s 内把
            // 画像 GPU 频率改回基础值(画像 GPU 设置形同虚设); 同步后守护反而保护画像值。
            GpuFreq::min_mhz.store(Fastatoi(gpuMin.c_str()));
            GpuFreq::max_mhz.store(Fastatoi(gpuMax.c_str()));
        } else {
            // 画像未定义 GPU → 跟随基础档, 防止上一画像的 GPU 频率/快照残留
            applyBaseGpu();
        }

        char spPath[256];
        for (int i = 0; i < kClusterCount; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (model.SchedParamCount[i] == 0) continue;
            // governor 留空则无目标路径，跳过 SchedParam
            const string_t& gov = model.Governor[i];
            if (gov.empty()) continue;
            for (int j = 0; j < model.SchedParamCount[i]; j++) {
                if (model.SchedParamName[i][j].empty()) continue;
                FastSnprintf(spPath, sizeof(spPath), SchedParamPath,
                             Policy::CpuPolicy[i], gov.c_str(),
                             model.SchedParamName[i][j].c_str());
                // 阻塞写：governor 类节点用 O_NONBLOCK 会 EAGAIN 静默失败(同 scaling_governor)
                utils.FileWriteIfChangedBlocking(spPath, model.SchedParamValue[i][j]);
                logger.Debug("画像调速器参数: c%d/%s = %s",
                             i, model.SchedParamName[i][j].c_str(),
                             model.SchedParamValue[i][j].c_str());
            }
        }

        logger.Debug("场景画像已应用: %s (isGame=%d)",
                     model.modelName.c_str(),
                     model.isGame ? 1 : 0);
    }

    void applyBaseMode() {
        // 先上线核心再写 governor/频率，否则离线核收不到 governor 写入
        restoreBaseCoresIfNeeded();

        for (int i = 0; i < kClusterCount; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            // 基础模式(powersave/balance/...)均非游戏: governor 留空也兜底, 不交给 ROM
            const string_t gov = resolveGovernor(i, Performances::CpuGovernor[i]);
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i],
                    Performances::MaxFreq[i], gov);
        }
        // applyBaseMode 必须重写基础 SchedParam，否则 governor tunables 保留游戏画像值导致不降频
        SchedParam();
        applyBaseGpu();
        function.FeasFunc(false);
    }

    // 退出画像时主动还原基础 GPU 频率，否则要等 gpuFreqGuard 最长 15s 才生效
    void applyBaseGpu() {
        if (!Config::GpuFreq::enable) return;
        JsonConfig::publishGpuSnapshot();
        if (Config::GpuFreq::min_freq.empty() && Config::GpuFreq::max_freq.empty()) {
            // 基础档未配 GPU: 不能直接放手 —— 否则离开 auto 时 GPU 会卡在 FSM 设的低百分比(C2)。
            // 还原到硬件最高频(=不限制), 解除可能残留的 FSM 限频。读不到硬件最高频才放手。
            int hw = function.gpuHwMaxMhz();
            if (hw > 0) {
                char mx[16]; FastSnprintf(mx, sizeof(mx), "%d", hw);
                Config::GpuFreq::max_mhz.store(hw, std::memory_order_relaxed);
                Config::GpuFreq::min_mhz.store(0, std::memory_order_relaxed);
                function.gpuFreqControlCustom(string_t(), string_t(mx));
            }
            return;
        }
        function.gpuFreqControlCustom(Config::GpuFreq::min_freq, Config::GpuFreq::max_freq);
    }

    void Reset() {
        invalidateFreqCache();
        for (int i = 0; i < kClusterCount; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647",
                    function.checkQcom() ? "walt" : "sugov_ext");
        }
        function.FeasFunc(true);
    }

    // [真黑名单·方案A] 彻底松手:全核在线 + 解锁频率(0~INT_MAX→硬件上下限) + 还原默认调速器/EAS,
    // 不施加任何档位,把 CPU/GPU 交还系统接管。用于黑名单 App 前台时。
    void releaseControl() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        char path[256];
        for (int i = 0; i < kCoreCount; i++) {
            FastSnprintf(path, sizeof(path), onlinePath, i);
            utils.WriteInt(path, 1);
        }
        for (int i = 0; i < kClusterCount; i++)
            if (Config::Policy::CpuPolicy[i] != -1) waitClusterReady(Config::Policy::CpuPolicy[i]);
        Reset();          // 解锁频率 + 默认调速器(walt/sugov_ext) + FeasFunc
        applyBaseGpu();   // GPU 交还基础,避免上一画像(如游戏)的 GPU 频率残留
    }

    void invalidateFreqCache() {
        for (int i = 0; i < kClusterCount; i++) {
            lastMinFreq[i].clear();
            lastMaxFreq[i].clear();
            lastGovernor[i].clear();
        }
    }

    void applyCurrentMode() {
        if (conf.mode.empty()) {
            logger.Warn("情景模式为空，跳过应用配置");
            return;
        }
        // 仅在模式真正变化时打 INFO; 配置重载(applyAllConfig)重复进来不刷屏。
        const bool logMode = (conf.mode != lastModeLogged_);
        lastModeLogged_ = conf.mode;
        if (logMode) logger.Info("情景模式: %s 已启用", conf.mode.c_str());

        // 自动模式: auto 自成一档。FSM 实时调频, 不加载任何基础档 —— 只放开下限+governor,
        // 上限交 FSM 持续写。
        if (conf.mode == "auto") {
            if (Config::AutoMode::enable) {
                autoActive_.store(true);
                if (logMode) logger.Info("智能档已接管 (FSM 实时调频, 不挂基础档)");
                primeAutoFsm();
            } else {
                autoActive_.store(false);
                logger.Warn("mode=auto 但 AutoMode.enable=false → 退回基础档 balance");
                applyWithProfile();
            }
            return;
        }

        autoActive_.store(false);
        if (conf.mode == "fast" && OfficialMode::enable) {
            Reset();
            return;
        }
        applyWithProfile();
    }

    // auto 的 FSM 预置: auto 自成一档, 不加载任何 config 基础档(省电/均衡/性能)。
    // 只做: 全核在线 + 各簇 governor 兜底 + 放开 scaling_min 到硬件最低(写"0",内核钳到 cpuinfo_min)。
    // 下限放开是关键 —— 触摸时内核 input boost 抬下限不会被 FSM 覆盖(FSM 只写上限),闲时频率也能自然落到最低。
    void primeAutoFsm() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        // 配置热重载可在 GAMING 期间重进这里: 下方 FreqWriter 会重写基础 governor,
        // 先把游戏态调速器条件还原并复位状态, 下一 FSM tick(仍在 GAMING)会自动重新切换。
        applyGamingGov(false, "primeAutoFsm");
        // 还原后 gamingGovActive_=false 但 fengchiActive(循环局部)仍 true → 状态机死区(进入沿要
        // !fengchiActive, 补切要 gamingGovActive_, 还原要 !fengchiActive, 全不满足 → 永远停在 walt)。
        // 置标志通知 FSM 循环重置 fengchiActive, 让进入沿重新接管(重走 3s 延时 → 重新切风驰)。
        fengchiGovResetPending_ = true;
        onlineAll();   // auto 强制全核在线, 不沿用上一基础档的 Online[](均衡档会关 CPU7)
        for (int i = 0; i < kClusterCount; i++) {
            int pol = Config::Policy::CpuPolicy[i];
            if (pol < 0) continue;
            const string_t gov = resolveGovernor(i, Config::Performances::CpuGovernor[i]);
            FreqWriter(pol, string_t("0"), string_t(), gov);   // 下限=hw-min, 上限留给 FSM, governor 兜底
        }
        applyBaseGpu();          // GPU 维持开机载入范围, 由 GPU 守护在内自调
        function.FeasFunc(false);
    }

    // ── G1: auto 档让 SysTune(每档 VM/网络/WiFi) 跟随自学习基础档, 边沿触发(仅变化才下发, 不进逐 tick 写循环)──
    // 移植 ASB V57 smart 跟档语义(alpha 高/低 → 切省电/低延迟网络集)。base = powersave/balance/performance。
    void followAutoSysTune(const std::string& base) {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        if (!Config::SysTune::enable) {
            sysTweaks_.restoreAll();
            lastAutoTuneMode_.clear();
            return;
        }
        if (base == "balance") sysTweaks_.restoreWifi();
        if (base == lastAutoTuneMode_) return;                // 边沿: 基础档未变则不重下发
        lastAutoTuneMode_ = base;
        sysTweaks_.applyProfileTune(base);
    }

    // 自动模式决策线程: ~2s 一 tick, 按负载/GPU/温度推进六状态, 用 Smart alpha 混合
    // battery↔balanced 包络直接写各簇频率上下限。仅当无 App 画像接管时驱动; 有画像则让位。
    void autoModeTask() {
        if (waitStop(7000)) return;   // 等系统稳定
        std::string lastResolved;
        int lastFsmState = -1;
        std::string lastLoggedFgPkg_ = "\x01";  // 上次已打印的前台/类别(哨兵值保证首次必打)
        int lastLoggedFgCat_ = -2;
        long long lastLogMs_ = 0;     // 上次打印 Info 行的时刻(心跳节拍用)
        long long lastFasLogMs_ = 0;  // FAS Info 行 ~1Hz 节流游标
        int lastGpuMax = -1;          // 每状态 GPU 上限去重
        bool fengchiActive = false;   // 风驰游戏态是否在位(边沿: 进出沿清 GPU min 去重, 让 FSM 重新接管)
        bool fsmPrimed = false;       // 是否已为 FSM 预置基础档(governor/schedparam/gpu)
        int  lastScreenOn = -1;       // 上一拍亮灭屏状态(供息屏→亮屏沿复位负载闭环)
        long long lastFsmMs = 0;      // 上次推进 FSM 状态机的时刻(见下方 fsmDue)
        bool fasWasGaming = false;    // FAS 是否已进入 GAMING 会话(用于进/出沿)
        std::string fasPkg;           // FAS 会话绑定的游戏包(换包沿=重开会话); 只记非空包
        int  frameGpuStreak = 0;      // 帧时辅助进入: GPU≥地板 连续 tick 数(防滚动突发误入)
        // 游戏保活粘滞(对齐基础模式的保活语义): 游戏【进程不死】就一直是游戏状态。
        // 存活判定 = isPackageRunning(/proc 扫描, 标准接口, 不依赖任何机型专属节点), ≥2s 节流。
        std::string stickyGame;       // 在位的粘滞游戏包(空=无)
        bool stickyHeavy = false;     // 该游戏是否 game_heavy(风驰档)
        long long stickyChkMs = 0;    // 存活复查节流游标
        bool stickyAliveCache = true; // 节流窗口内复用的存活结果
        while (!shouldExit_.load()) {
            if (!autoActive_.load()) {
                lastResolved.clear(); lastFsmState = -1; lastGpuMax = -1; fsmPrimed = false;
                fengchiActive = false;   // 退 auto: 风驰态归零(下次进 auto 不残留)
                lastLoggedFgPkg_ = "\x01"; lastLoggedFgCat_ = -2; lastLogMs_ = 0;
                stickyGame.clear();   // 退 auto: 粘滞归零(对称 fasWasGaming)
                fengchiGovResetPending_ = false;   // 退 auto: 清热重载标志(对称 fengchiActive)
                if (fasWasGaming) { autoFas_.onExitGaming(); fasWasGaming = false; }
                resetLoadGovernor();   // 退出 auto: 闭环状态归零, 下次进 auto 从当时的 FSM 预算重新起步
                // 退出 auto: 退出路径(onModeChanged→primeAutoFsm/online)已按非 auto 档处理核心,
                // 这里只把去重缓存复位为全 1, 下次进 auto 不因陈旧缓存漏掉一次上线。
                // 必须持 applyMtx: fileWatch 线程经 onModeChanged→onlineAll 在锁内写同一数组(避免 data race)。
                { std::lock_guard<std::recursive_mutex> lk(applyMtx);
                  applyGamingGov(false, "退出auto");   // 退出 auto: 游戏态调速器若在切换态, 条件还原(外部改过则不覆盖)
                  gamingGovEngaged_ = false;   // 退出 auto: 清原生风驰标记 (active_=false 时 applyGamingGov(false) 为 no-op, 需显式清)
                  applyTopAppUclampMin(0);   // 退出 auto: 还原 top-app uclamp 下限
                  applyPowerBoost(-1);        // 退出 auto: 还原 UCLAMP 各节点到原值
                  pwrParkBig_ = false;        // 退出 auto: 解除功率关核(大核随 onlineAll 重新上线)
                  applyFpsgo(false);         // 退出 auto: 还原 FPSGO
                  applyCameraHold(false);    // 退出 auto: 相机保持若在位, 条件还原 cpuset/uclamp
                  applyDeepIdleBgUclamp(false);   // 退出 auto: 息屏背景 uclamp 若被我们压过, 条件还原
                  for (int i = 0; i < kCoreCount; i++) lastAutoOnline_[i] = 1;
                  lastAutoTuneMode_.clear(); }   // 退出 auto → 清 SysTune 基础档去重, 重进时按当时基础档重下发
                if (waitStop(3000)) break;
                continue;
            }
            const unsigned long long tickGeneration = configGeneration_.load(std::memory_order_acquire);
            AutoModeCtl::Metrics mx = autoMode_.sample();

            // ── FSM 节奏与负载闭环解耦 ──────────────────────────────────────────────
            // LoadGovernor 需要快拍(600ms), 但 AutoFsm 的确认窗口是按 **tick 数** 标定的:
            //   UP_WINDOW=2 (注释口径 ×2s = 4s)、DOWN_WINDOW=5 (×2s = 10s)、
            //   kPairConfirmWindow=3、最小驻留则是墙钟秒(不受影响)。
            // 若 tick 缩短到 600ms 后仍每拍都推进状态机, 等价于把升档压到 1.2s、降档压到 3s,
            // 状态机会明显 churn(上游现场日志已证: MODERATE↔HEAVY 来回本身就是主要抖动源)。
            // 故状态机与账号记账仍按 kFsmTickMs(2s) 抽帧, 只有采样 + 闭环 + 写频跑快拍。
            const long long tickNowMs = (long long)nowMs();
            const bool fsmDue = (lastFsmMs == 0) || (tickNowMs - lastFsmMs >= kFsmTickMs);
            if (fsmDue) {
                autoMode_.accountTick(mx);   // 会话学习(两种子模式都做) —— 按 2s 节奏, 保持原语义
                lastFsmMs = tickNowMs;
            }

            // 配置热重载在 applyMtx 内重写字符串和策略表；本 tick 复制标量快照，
            // 避免锁外读取配置对象，同时不把慢速前台与 sysfs 探测放进锁内。
            struct AutoTickCfg {
                bool fasAutoGame;
                bool fasEnable;
                bool fsmWeightedLoad;
                bool gameFpsgo;
                bool cameraHoldEnable;
                bool appPolicyEnable;
                bool batteryEnable;
                bool fsmJointDvfs;
                bool quietNightEnable;
                int quietNightTickMs;
                int fsmSustainedEnter;
                int gameUclampMin;
                bool loadGovEnable;
                int  loadGovTickMs;
            } tickCfg;
            {
                std::lock_guard<std::recursive_mutex> lk(applyMtx);
                tickCfg.fasAutoGame = Config::AutoMode::fas_auto_game;
                tickCfg.fasEnable = Config::AutoMode::fas_enable;
                tickCfg.fsmWeightedLoad = Config::AutoMode::fsm_weighted_load;
                tickCfg.gameFpsgo = Config::AutoMode::game_fpsgo;
                tickCfg.cameraHoldEnable = Config::AutoMode::camera_hold_enable;
                tickCfg.appPolicyEnable = Config::AutoMode::app_policy_enable;
                tickCfg.batteryEnable = Config::AutoMode::auto_battery_enable;
                tickCfg.fsmJointDvfs = Config::AutoMode::fsm_joint_dvfs;
                tickCfg.quietNightEnable = Config::AutoMode::quiet_night_enable;
                tickCfg.quietNightTickMs = Config::AutoMode::quiet_night_tick_ms;
                tickCfg.fsmSustainedEnter = Config::AutoMode::fsm_sustained_enter;
                tickCfg.gameUclampMin = Config::AutoMode::game_uclamp_min;
                tickCfg.loadGovEnable = Config::AutoMode::loadgov_enable;
                tickCfg.loadGovTickMs = Config::AutoMode::loadgov_tick_ms;
            }

            // 息屏→亮屏沿: 复位负载闭环状态。
            // 息屏期间 FSM 在 DEEP_IDLE(LEVEL 0.0), 预算被压到最低; 若带着那时的 currentPerf 醒来,
            // 闭环要爬好几个 tick 才回到可用上限(升频确认 3 tick + 平滑), 亮屏首秒会明显不跟手。
            // 复位后从"当时的预算"直接起步。
            if (mx.screenOn != 0 && lastScreenOn == 0) resetLoadGovernor();
            lastScreenOn = mx.screenOn;

            // 注入前台包名(供 App 发热学习 / 凉爽游戏 / gameHint 复用)。仅亮屏取 —— 息屏省去
            // dumpsys 开销, 且避免把 launcher/null 误学成热 App(息屏温度本就在降)。
            const char* foregroundSource = "screen_off";
            std::string fgPkg = (mx.screenOn != 0) ? getForegroundPkg(false, &foregroundSource) : std::string();
            const std::string observedForeground = fgPkg;
            autoMode_.setForegroundSource(foregroundSource);
            autoMode_.setForegroundApp(fgPkg.c_str());   // 发热学习记【真实焦点】, 粘滞不改写它

            // 设计约定: auto 模式下「FSM 独占」—— 不再让位给 App 画像或基础全局档。
            // (前台处理器在 auto 下也已把 matched 强制为 -1, 不会应用任何画像。)
            // 游戏等高负载场景由 FSM 的 GAMING/HEAVY 状态自行拉满频率, 无需静态画像接管。
            {
                // 前台是已知游戏 App → 直接判 GAMING(不只看 GPU; 有些游戏 GPU 不到阈值)。
                // 判据 = perapp_powermode.txt(App「应用策略」写入, findMatchingModel 内 perapp 优先)
                // 或 config 游戏库: 包名策略为 game_heavy/game_lite(is_game 画像)即进游戏状态。
                // useGlobalFallback=false: 只认显式包名策略, "*" 全局默认行不算 —— 否则全局默认
                // 被设成游戏档时所有 App 都会被判成游戏。
                bool gameHint = false;
                bool gameByProfile = false;   // 游戏判定来自画像(perapp/游戏库), 非系统类别/帧时推断
                bool fgHeavy = false;   // 前台命中 game_heavy(风驰游戏)画像: 驱动风驰调速器 + 与 FAS 互斥 + uclamp
                bool fgBlacklisted = false;
                if (!fgPkg.empty() && fgPkg != "null") {
                    std::lock_guard<std::recursive_mutex> lk(applyMtx);
                    fgBlacklisted = Config::AppProfile::isBlacklisted(fgPkg.c_str());
                }
                if (!fgPkg.empty() && fgPkg != "null" && !fgBlacklisted) {
                    // 画像匹配 + modelName 字符串读取收进 applyMtx: 配置热重载(onJsonChanged 持锁)会
                    // 整表重写 Models[].modelName(std::string 重赋值), 锁外读 c_str() 有撕裂/UAF 风险。
                    // fgCategory 的 popen 慢 I/O 留锁外。
                    {
                        std::lock_guard<std::recursive_mutex> lk(applyMtx);
                        int gm = Config::AppProfile::findMatchingModel(fgPkg.c_str(), false);
                        if (gm >= 0 && Config::AppProfile::Models[gm].isGame) {
                            gameHint = true;
                            gameByProfile = true;
                            fgHeavy = (strcmp(Config::AppProfile::Models[gm].modelName.c_str(),
                                              "game_heavy") == 0);
                        }
                    }
                    // App 类别自动识别: 系统标记 category=GAME(0) → 直接判游戏, 无需手动白名单。
                    // 与白名单取【或】, 独立于实验开关 fas_auto_game; 视频(category=2)等不命中, 不会误入。
                    // fgCategory 仅首次见该包 popen 一次, 之后命中缓存 → 不增每 tick 开销。
                    if (!gameHint && fgCategory(fgPkg) == 0) gameHint = true;
                    // 内置游戏清单兜底(见 GameList.hpp): 系统未声明 appCategory、用户也没绑画像时,
                    // 靠包名清单识别。不依赖无障碍推送/系统类别, 是这条链路失效时的最后一道保险。
                    if (!gameHint && Config::AutoMode::gamelist_enable &&
                        Config::GameList::contains(fgPkg.c_str())) {
                        gameHint = true;
                        logger.Debug("游戏清单命中: %s", fgPkg.c_str());
                    }
                }
                // ── 多窗口游戏扫描: 小窗/分屏时前台包名只有顶层一个, 但游戏可能在后台可见窗口里。
                // 遍历所有 visible=true 的 standard Task, 只要有一个命中游戏画像就保持游戏态。
                // 仅在尚未从 fgPkg 判出游戏时才触发(避免重复 getForegroundCached 开销)。
                // fgHeavy: 多窗口扫出的 heavy 直接驱动风驰调速器, 与粘滞语义一致。
                // TTL 按活动度分级(上游 V64 思路, 见 include/ProbeCadence.hpp): 安静时 120s 一次,
                // 重载/游戏态 8s —— 原来固定 2s, 亮屏期间常驻 0.5 次/秒的 dumpsys。
                if (!gameHint && mx.screenOn != 0) {
                    const int mwTtl = kin::multiWindowScanTtlMs(mx.gpuPct, mx.cpuBusy,
                                                               autoFsm_.state() >= AutoFsm::HEAVY);
                    Utils::ForegroundSnapshot snap = utils.getForegroundCached(mwTtl);
                    if (snap.valid) {
                        std::lock_guard<std::recursive_mutex> lk(applyMtx);
                        for (const auto& visiblePkg : snap.standardVisible) {
                            if (visiblePkg.empty() || visiblePkg == "null") continue;
                            if (Config::AppProfile::isBlacklisted(visiblePkg.c_str())) continue;
                            if (visiblePkg == fgPkg) continue;   // fgPkg 已检测过
                            int gm = Config::AppProfile::findMatchingModel(visiblePkg.c_str(), false);
                            // 清单命中且用户没绑画像时, 用轻量档(game_lite)画像名参与后续判定:
                            // 只借"是游戏"这一结论, 不改用户配置(fgHeavy 仍为 false)。
                            if (gm < 0 && Config::AutoMode::gamelist_enable &&
                                Config::GameList::contains(visiblePkg.c_str())) {
                                gameHint = true;
                                if (fgPkg.empty() || fgPkg == "null") fgPkg = visiblePkg;
                            }
                            if (gm >= 0 && Config::AppProfile::Models[gm].isGame) {
                                gameHint = true;
                                gameByProfile = true;
                                bool h = (strcmp(Config::AppProfile::Models[gm].modelName.c_str(),
                                                 "game_heavy") == 0);
                                if (h) fgHeavy = true;
                                // fgPkg 更新为游戏包, 供粘滞/FAS/日志使用
                                if (!h || fgPkg.empty() || fgPkg == "null")
                                    fgPkg = visiblePkg;
                                break;
                            }
                        }
                    }
                }
                // 实验性帧时辅助进入(默认关): 限帧游戏 GPU<90 永不触 GPU 门, 这里给一条【非白名单】入口。
                // 必须同时满足: 开关开 + 亮屏 + GPU≥地板 持续 FRAME_ENTRY_DWELL tick + 检测到游戏渲染层。
                // 【绝不】用 measured_fps —— 高刷滚动 measured_fps 也到 ~120。视频用 SurfaceView 但 GPU 低, 被地板挡掉。
                static constexpr int kFrameEntryGpu   = 50;   // GPU 持续地板(视频硬解 GPU 远低于此 → 排除)
                static constexpr int kFrameEntryDwell = 3;    // ×tick: GPU 地板需连续达到才认(滚动 GPU 是突发, 攒不满)
                static constexpr int kFrameEntryCpu   = 22;   // CPU 地板(整机 8 核活跃%): 高 GPU+高 CPU=游戏, 高 GPU+低 CPU=视频
                if (!gameHint && tickCfg.fasAutoGame && mx.screenOn != 0 &&
                    !fgPkg.empty() && fgPkg != "null" &&
                    !fgBlacklisted &&
                    autoFas_.available()) {
                    // 视频/音频类显式排除: 它们也用 SurfaceView, 渲染层+GPU 地板可能误判, 按系统类别直接挡掉。
                    const int fcat = fgCategory(fgPkg);   // 命中类别缓存, 不增每 tick popen
                    if (fcat == 2 || fcat == 1) {         // VIDEO=2 / AUDIO=1 → 绝不自动进 GAMING
                        frameGpuStreak = 0;
                    } else {
                        if (mx.gpuPct >= kFrameEntryGpu) { if (frameGpuStreak < 100000) frameGpuStreak++; }
                        else                             { frameGpuStreak = 0; }
                        // ASB V54 相关性兜底: 类别 UNDEFINED 时, 高 GPU 还须高 CPU 才认游戏(排除硬解视频)。
                        // cpuBusy 未知 -1 则退回原行为(仅渲染层+GPU 地板)。已知类别(非视频)不受 CPU 地板约束。
                        const bool cpuOk = (fcat != -1) || (mx.cpuBusy < 0) || (mx.cpuBusy >= kFrameEntryCpu);
                        if (frameGpuStreak >= kFrameEntryDwell && cpuOk &&
                            autoFas_.hasGameLayer(utils, fgPkg.c_str(), (long)nowMs()))
                            gameHint = true;   // 复用 gameHint→GAMING 即时确认路径(FSM desired)
                    }
                } else {
                    frameGpuStreak = 0;
                }
                // ── 游戏保活粘滞(对齐基础模式): 游戏【进程不死】就一直是游戏状态 ──
                // 场景: 游戏内开小窗/分屏/回桌面/焦点横跳 —— 只要游戏进程还活着, 本 tick 就按
                // 「前台=该游戏」处理: 风驰/关核/FAS/类别钳制全部不抖。判定只用两样标准接口:
                // App 无障碍推送(前台来源) + isPackageRunning(/proc 进程存活), 无任何机型专属节点。
                // 解除 = 进程退出(手动杀/系统杀/游戏自退) / 画像失去游戏身份或被拉黑 / 退 auto。
                bool stickyHolding = false;
                if (mx.screenOn != 0) {
                    bool wantEnter = gameHint && !fgPkg.empty();
                    // 抢占防御: 系统类别误标的悬浮工具(如游戏助手)不能顶掉在位游戏;
                    // 画像确认的游戏(perapp/游戏库)可正常移交。
                    if (wantEnter && !stickyGame.empty() && stickyGame != fgPkg && !gameByProfile)
                        wantEnter = false;
                    if (wantEnter) {
                        if (stickyGame != fgPkg) {
                            if (stickyGame.empty())
                                logger.Debug("粘滞: 记入 %s heavy=%d", fgPkg.c_str(), fgHeavy ? 1 : 0);
                            else
                                logger.Debug("粘滞: 移交 %s → %s", stickyGame.c_str(), fgPkg.c_str());
                            stickyGame = fgPkg;
                            stickyChkMs = 0; stickyAliveCache = true;
                        }
                        stickyHeavy = fgHeavy;
                        stickyHolding = true;
                    } else if (!stickyGame.empty()) {
                        // 画像热更新复查(锁内纯查表): 被拉黑/画像明确失去 isGame → 立即解除;
                        // game_heavy↔lite 翻转跟随; 无画像(类别/帧时进入)维持原判。
                        bool revoked = false;
                        {
                            std::lock_guard<std::recursive_mutex> lk(applyMtx);
                            if (Config::AppProfile::isBlacklisted(stickyGame.c_str())) {
                                revoked = true;
                            } else {
                                int gm = Config::AppProfile::findMatchingModel(stickyGame.c_str(), false);
                                if (gm >= 0) {
                                    if (!Config::AppProfile::Models[gm].isGame) revoked = true;
                                    else stickyHeavy = (strcmp(
                                        Config::AppProfile::Models[gm].modelName.c_str(), "game_heavy") == 0);
                                }
                            }
                        }
                        if (revoked) {
                            logger.Info("粘滞: 解除 %s 原因=画像/黑名单变更", labelForLog(stickyGame).c_str());
                            stickyGame.clear();
                        } else {
                            const long long snow = nowMs();
                            if (snow - stickyChkMs >= 2000) {   // 存活复查 ≥2s 节流(/proc 扫描)
                                stickyAliveCache = utils.isPackageRunning(stickyGame.c_str());
                                stickyChkMs = snow;
                            }
                            if (stickyAliveCache) {
                                fgPkg = stickyGame;             // 本 tick 视作游戏在前台
                                gameHint = true; fgHeavy = stickyHeavy;
                                stickyHolding = true;
                            } else {
                                logger.Info("粘滞: 解除 %s 原因=进程已退出", labelForLog(stickyGame).c_str());
                                stickyGame.clear();
                            }
                        }
                    }
                }
                // 首次进入 FSM: 预置一次(放开下限+governor, 不挂基础档), 之后 FSM 只写上限 scaling_max。
                // 注意: prime 不再额外调 autoFsm_.update() —— 否则同一份 mx 被喂两次, 把一次真实 tick
                // 当成两拍升档/gaming 连胜确认, 破坏 UP_WINDOW 迟滞。下方统一只 update 一次。
                if (!fsmPrimed) {
                    primeAutoFsm();
                    fsmPrimed = true;
                }
                const int alpha = autoMode_.alphaFor(mx);
                // 低电是最终 CPU 安全 cap 来源之一；它必须在 FAS/App/boost 候选之后仍然生效。
                const bool lowBatterySafe = autoMode_.lowBatteryActive(tickCfg.batteryEnable, mx.charging);
                // FSM 温度入参走皮肤锚定决策温(SUSTAINED 热降频天花板据此; 无皮肤=结温=现状)。
                // 日志行仍打真实结温 mx.tempC。
                // 负载信号: 开启加权且可读时用能效加权整机负载(大小核按能力×频率加权, 更准),
                // 否则回退平摊平均 cpuBusy。阈值(80/60)含义在加权下更贴近"芯片真到能力上限"。
                int fsmCpu = (tickCfg.fsmWeightedLoad && mx.cpuBusyW >= 0)
                                 ? mx.cpuBusyW : mx.cpuBusy;
                // PSI 争用修正(GearDVFS): 利用率未到顶但任务在排队等 CPU(突发/多任务)→ 真实需求被平均
                // 利用率漏掉, 用 PSI 兜底抬高(取大, 只在真有等待时生效)。仅加权路径启用, 避免扰动旧调校。
                if (tickCfg.fsmWeightedLoad && mx.cpuPsi > fsmCpu && fsmCpu >= 0)
                    fsmCpu = mx.cpuPsi;
                // 前台包类别提前取: GAMING 包名校验与下方类别钳制共用同一份结果(命中缓存, 不增 popen)。
                const int fgCat = (mx.screenOn != 0 && !fgPkg.empty()) ? resolveCategory(fgPkg) : -1;
                // 上游 V64: 前台已被明确分类为非游戏 → 不让 GPU 单指标把状态推到 GAMING。
                // gameHint 已成立(画像/系统类别/帧源)或类别未知(-1)时保持旧行为, 未分类的游戏不受影响。
                const bool fgNotGame = (!gameHint && fgCat >= 0);
                // 上游 V64: 两个热源打架时负载驱动的迁移多要一拍(见 ThermalDecision.hpp 的争议定义)。
                // 热升档不受影响 —— 对传感器的不确定只该买到延迟, 不该买到安全。
                const bool thermalDisputed = kin::thermalSourcesDisputed(
                    mx.tempC, mx.skinC, Config::AutoMode::thermal_skin_gate,
                    Config::AutoMode::veto_temp, Config::AutoMode::skin_temp,
                    Config::AutoMode::junction_hard_temp);
                // 仅在 fsmDue 拍推进状态机(见上方说明); 非 fsmDue 拍保持现有状态, 只让闭环调频继续跑。
                bool changed = false;
                if (fsmDue) changed = autoFsm_.update(mx.gpuPct, mx.load1, autoMode_.thermalDecisionC(mx), mx.screenOn, fsmCpu, gameHint,
                                               mx.currentMa, mx.charging, fgNotGame, thermalDisputed);
                AutoFsm::Caps caps = autoFsm_.computeCaps(alpha);

                // ── 用户 fsm_policy 覆盖内置曲线(config.json Function.AutoMode.fsm_policy) ──
                // 按当前状态把小核/大核上限换成「本机 cpuinfo_max_freq × 用户%」(-1=未配置, 保持内置包络);
                // 下限只压不抬(min 不超过新上限)。顺序: 用户覆盖放在最前, FAS(游戏帧率闭环)与应用类别
                // 钳制仍在其后生效 —— 帧控可继续下调、轻类别封顶仍只压不抬, 原有语义不变。
                // 持 applyMtx 复制(配置热重载会整表重写 fsmPolicyTable, 防读到撕裂值), 与下方 catPolicy 同款。
                Config::AutoMode::FsmPolicy fpol;
                bool gamingGovOn;      // 风驰调速器功能是否开(gaming_gov 非空); 锁内快照防热重载撕裂
                bool fengchiInPlace;   // 风驰已确认在位, 或尚未尝试切换(首拍先按意图挡 FAS, 防一拍进出抖动)
                { std::lock_guard<std::recursive_mutex> lk(applyMtx);
                  fpol = Config::AutoMode::fsmPolicy(caps.state);
                  gamingGovOn = !Config::AutoMode::gaming_gov.empty();
                  fengchiInPlace = gamingGovActive_ ? gamingGovEngaged_ : true; }
                // 百分比换算后钳到 [cpuinfo_min, hwmax](与内置曲线 scale() 的包络一致):
                // 小核最低频常占最高频 15% 左右, 用户拖到 10% 时若不钳地板, 写出的
                // scaling_max 低于硬件最低频, 内核可能拒写(旧上限残留)或自行收紧, 行为不可预期。
                if (fpol.maxL >= 10 && fpol.maxL <= 100 && autoFsm_.hwMaxL() > 0) {
                    int v = (int)((long)autoFsm_.hwMaxL() * fpol.maxL / 100);
                    caps.maxL = v < autoFsm_.hwMinL() ? autoFsm_.hwMinL() : v;
                    if (caps.minL > caps.maxL) caps.minL = caps.maxL;
                }
                if (fpol.maxB >= 10 && fpol.maxB <= 100 && autoFsm_.hwMaxB() > 0) {
                    int v = (int)((long)autoFsm_.hwMaxB() * fpol.maxB / 100);
                    caps.maxB = v < autoFsm_.hwMinB() ? autoFsm_.hwMinB() : v;
                    if (caps.minB > caps.maxB) caps.minB = caps.maxB;
                }

                // ── 负载闭环调频（P0-2 / P0-3）：在预算内按本簇负载收紧上限 ──
                // 必须放在用户 fsm_policy 覆盖之后：fsm_policy 是用户配的**预算**，闭环只能在预算内收紧。
                // 后面 FAS / 类别钳制 / 相机 / 热 / PolicyResolver 仍会继续取 min，顺序不受影响。
                applyLoadGovernor(caps, mx.clusterBusy, tickNowMs);
                // 闭环可观测性: 只在 FSM 拍打一行(与状态机同节奏, 不随快拍刷屏)。
                // 判读口径: perf 是该簇闭环算出的上限占硬件最高频的比例, busy 是簇内最忙核占用。
                // 若 perf 长期贴 1.0 而 busy 不高 → 说明 headroom/门槛没起作用; 若 perf 恒定不动
                // → 说明撞上了死区或 FSM 预算(闭环只能在预算内收紧)。
                if (fsmDue && Config::AutoMode::loadgov_enable) {
                    logger.Debug("负载闭环: 小核 busy=%d%% perf=%.3f 上限=%dMHz | 大核 busy=%d%% perf=%.3f 上限=%dMHz",
                                 mx.clusterBusy[0], loadGovPrimed_[0] ? loadGov_[0].currentPerf : -1.0f,
                                 caps.maxL / 1000,
                                 mx.clusterBusy[1], loadGovPrimed_[1] ? loadGov_[1].currentPerf : -1.0f,
                                 caps.maxB / 1000);
                }

                // ── FAS(帧感知): 仅【命中游戏画像(inGaming=gameHint) + 亮屏 + 开关 + 帧源可用】时闭环覆盖 scaling_max ──
                // capsToWrite 默认 = 静态 FSM 上限; FAS 只在 fo.engaged(拿到可信帧率)时下调, 否则保持静态 —— 绝不让游戏变差。
                AutoFsm::Caps capsToWrite = caps;
                bool fasEngaged = false;
                // 每 tick 新建合并器；显式安全 cap 独立保存，不把 FSM 基础值当硬限制。
                // 下方功率 boost 可调整普通上限，但不能解除已登记的热限制。
                kin::PolicyResolver policyResolver({capsToWrite.minL, capsToWrite.maxL,
                                                     capsToWrite.minB, capsToWrite.maxB});
                // auto 游戏状态 = 命中任何游戏画像(gameHint: isGame 画像/系统游戏类别/帧时辅助),
                // 不依赖 FSM 状态机(避免进 GAMING 一拍延迟、退出 25s 最小驻留造成的"画像已变但状态没跟"窗口)。
                const bool inGaming = gameHint;
                // FAS 现对【任何方式命中游戏画像】生效(含未白名单经 GPU≥90 帧时辅助进入)——
                // FAS 仅在帧达标时下调 scaling_max(util 上限省电)、掉帧即放开, 且永不超过静态上限,
                // 故对"已确认是游戏"的 GAMING 态绝不会比现有静态档更差, 反而回收限帧游戏的浪费功耗。
                // dumpsys 兜底所需的 fgPkg 下方已 setForegroundPkg 注入(GPU 进入的游戏也有前台包)。
                // 与风驰调速器互斥: 风驰游戏(game_heavy 画像)切到 hmbird/scx 后由内核调度器自主调频,
                // FAS 再压 scaling_max 会跟它打架 → 风驰【真在位】时 game_heavy 不走 FAS;
                // 本机没有 hm/hmbird/scx(仅 walt/sugov_ext 回退)时 game_heavy 仍享受 FAS。
                // 非风驰游戏(game_lite/GPU 进入)照常 FAS; 风驰功能关(gaming_gov 空)时 FAS 对所有游戏生效。
                // 天玑 FPSGO 互斥: 开 FPSGO 的 game_heavy 交给厂商帧引擎, 同样不走 Kin FAS(防两个帧控打架)。
                const bool fpsgoOn = tickCfg.gameFpsgo && fgHeavy && inGaming && fpsgoUsable();
                // 风驰接管(前台命中 game_heavy 风驰游戏画像即触发, 不再要求 GAMING 状态): 跳过 FAS —— 硬件极限频率下帧控会与满频矛盾。
                const bool fengchiTakeover = fgHeavy && gamingGovOn;
                const bool fasBlocked = (fgHeavy && gamingGovOn && fengchiInPlace) || fpsgoOn || fengchiTakeover;
                if (tickCfg.fasEnable && inGaming && !fasBlocked &&
                    mx.screenOn != 0 && autoFas_.available()) {
                    // perapp 第三列的每游戏目标帧率(持 applyMtx 查表, 防热重载撕裂; 0=自动学习)
                    int fpsOverride = 0;
                    if (!fgPkg.empty()) {
                        std::lock_guard<std::recursive_mutex> lk(applyMtx);
                        fpsOverride = Config::AppProfile::fasTargetForPkg(fgPkg.c_str());
                    }
                    if (!fasWasGaming) {
                        autoFas_.onEnterGaming(caps, (long)nowMs(), fpsOverride);
                        autoFas_.setFgPid(findGamePid(fgPkg));   // fas-rs v4.4 util_max: 供单线程 CPU 比例采样(进场/换包时设一次)
                        fasWasGaming = true;
                        fasPkg = fgPkg;
                    } else if (!fgPkg.empty() && fasPkg != fgPkg) {
                        // 会话内换游戏(粘滞移交/驻留期换前台): 重开 FAS 会话 —— 帧史/目标/
                        // 预热窗(engageMs_)/钳位起点(caps)全按新游戏重来, 不沿用别家状态。
                        // 经"空包名过渡"(前台瞬时取不到)也能正确触发: fasPkg 只记非空包。
                        autoFas_.onExitGaming();
                        autoFas_.onEnterGaming(caps, (long)nowMs(), fpsOverride);
                        autoFas_.setFgPid(findGamePid(fgPkg));   // fas-rs v4.4 util_max: 供单线程 CPU 比例采样(进场/换包时设一次)
                        fasPkg = fgPkg;
                    }
                    autoFas_.setForegroundPkg(fgPkg.c_str());
                    AutoFas::Out fo = autoFas_.tick(autoMode_, utils, caps,
                                                    caps.maxL, caps.maxB, mx.gpuPct, mx.cpuBusy, (long)nowMs(), mx.cpuBusyW);
                    if (fo.engaged) {
                        capsToWrite.maxL = fo.maxL; capsToWrite.maxB = fo.maxB;
                        fasEngaged = true;
                        policyResolver.cap(fo.maxL, fo.maxB, "fas", "frame_target");
                    }
                } else if (fasWasGaming) {
                    autoFas_.onExitGaming(); fasWasGaming = false;
                }

                // ── 按 App 类别封顶钳制(只压不抬) ──
                // 类别已在 FSM 更新前取好(fgCat), 这里只把结果注入 AutoMode 供状态文件/相机判定。
                // 轻类别(视频/社交/音频)压低大核/小核 scaling_max 与 GPU 上限省电; 游戏/未知不钳制。
                // FAS 接管(游戏帧率闭环)时不叠加, 交给帧控。结果命中缓存, 不增每 tick popen。
                autoMode_.setForegroundCategory(fgCat);
                // ── 相机保持检测(ASB V60 camera_hold): 前台是相机 App → 保持性能 ──
                // 相机流式传输时 ISP+编码器吃负载但 GPU/load 不高, FSM 误判 MODERATE → 4K60 丢帧。
                // 命中相机包名表 → cameraHoldWant=true, 后续在写频率锁内接管 cpuset/uclamp + 软钳 caps。
                const bool cameraHoldWant = (mx.screenOn != 0) &&
                                            tickCfg.cameraHoldEnable &&
                                            isCameraApp(fgPkg, fgCat);
                // 持 applyMtx 复制类别策略: 配置热重载(applyAllConfig 持锁)会整表重写 catPolicyTable,
                // 否则本 tick 可能读到撕裂的结构。复制后即解锁, 钳制用副本。
                Config::AutoMode::CatPolicy cpol;
                { std::lock_guard<std::recursive_mutex> lk(applyMtx); cpol = Config::AutoMode::catPolicy(fgCat); }
                if (tickCfg.appPolicyEnable && !fasEngaged && fgCat != -1) {
                    const int hwL = autoFsm_.hwMaxL(), hwB = autoFsm_.hwMaxB();
                    if (cpol.maxPctL > 0 && cpol.maxPctL < 100 && hwL > 0) {
                        int capL = hwL * cpol.maxPctL / 100;
                        policyResolver.cap(capL, 0, "app", "category");
                        if (capsToWrite.maxL > capL) capsToWrite.maxL = capL;
                        if (capsToWrite.minL > capsToWrite.maxL) capsToWrite.minL = capsToWrite.maxL;
                    }
                    if (cpol.maxPctB > 0 && cpol.maxPctB < 100 && hwB > 0) {
                        int capB = hwB * cpol.maxPctB / 100;
                        policyResolver.cap(0, capB, "app", "category");
                        if (capsToWrite.maxB > capB) capsToWrite.maxB = capB;
                        if (capsToWrite.minB > capsToWrite.maxB) capsToWrite.minB = capsToWrite.maxB;
                    }
                }

                // 写频率/GPU 必须在 applyMtx 内, 并【重新确认仍在 auto+FSM】 —— 否则本 tick 计算期间
                // 用户切到手动档(onModeChanged 持锁恢复了基础档), 这里会把刚恢复的 CPU/GPU 覆盖掉,
                // GPU 还会被守护持续顶住卡在 FSM 值。(修复竞态 C1)
                {
                    std::lock_guard<std::recursive_mutex> lk(applyMtx);
                    if (!autoActive_.load() ||
                        configGeneration_.load(std::memory_order_acquire) != tickGeneration) continue;
                    // 风驰游戏态只解除频率限制并启动 OIface，governor 全程由 ROM/风驰接管。
                    // Kin 不做 3 秒兜底强切，也不在退出时恢复 governor，避免与系统争抢。
                    if (fengchiGovResetPending_) {
                        fengchiGovResetPending_ = false;
                        if (fengchiActive) fengchiActive = false;
                    }
                    if (fengchiTakeover && !fengchiActive) {
                        fengchiActive = true;
                        ensureOifaceAlive();
                        applyGamingGov(true);
                        logger.Info("游戏态 governor 已放权，交给 ROM/风驰接管");
                    } else if (!fengchiTakeover && fengchiActive) {
                        fengchiActive = false;
                        applyGamingGov(false);
                        if (Config::GpuFreq::enable) {
                            Config::GpuFreq::min_mhz.store(0, std::memory_order_relaxed);
                            lastGpuMax = -1;
                        }
                    }
                    if (fengchiActive) {
                        // 风驰接管: 写硬件极限 CPU/GPU 频率，governor 保持空写。
                        applyFengchiHwCaps();
                        // FAS/类别钳制/zTT/每状态 GPU 上限全部跳过(硬件极限下帧控与钳制无意义)。
                        // 风驰游戏态下相机保持不接管(游戏优先, 且游戏时不会同时开相机流)。
                        applyCameraHold(false);
                        // 功率 boost 旁路: 风驰内核自主调频, 不介入 UCLAMP(还原各节点原值)。
                        applyPowerBoost(-1);
                        pwrParkBig_ = false;   // 风驰接管: 解除功率关核, 让大核重新可用(游戏需要)
                    } else {
                    // ── 相机保持 caps 软钳(ASB V60): 相机时把 FSM caps 压到够用值, 不冲满 ──
                    // 在 writeFsmCaps 前修改 capsToWrite, 与类别钳制叠加(取更严者)。
                    const AutoFsm::Caps beforeCamera = capsToWrite;
                    clampCapsForCamera(capsToWrite);
                    policyResolver.cap(capsToWrite.maxL < beforeCamera.maxL ? capsToWrite.maxL : 0,
                                       capsToWrite.maxB < beforeCamera.maxB ? capsToWrite.maxB : 0,
                                       "camera", "camera_hold");
                    // ── P2-⑦ CPU+GPU 联合决策(zTT) + P2-⑧ ADPF 预测热: 热受限 GAMING 且 GPU 明确
                    // 是瓶颈(≥90%)时, 帧率由 GPU 决定, CPU 再高频也无益 → 额外下压大核上限省电。
                    // 热受限判据 = 物理温度到阈值 或 ADPF 热余量预测即将节流(≥85%, 早于物理温度触发)。
                    // 只削【非瓶颈】方向, 不伤帧率; 不做反向(压 GPU 有伤帧风险)。FAS 接管时交给帧控不叠加。
                    const bool thermalTight =
                        (mx.tempC >= 0 && mx.tempC >= tickCfg.fsmSustainedEnter) ||
                        (mx.adpfHr >= 85);
                    if (tickCfg.fsmJointDvfs && !fasEngaged && inGaming &&
                        mx.gpuPct >= 90 && thermalTight) {
                        int trimmed = capsToWrite.maxB * 92 / 100;   // 大核上限 -8%
                        int floorB = autoFsm_.hwMinB();
                        if (trimmed < floorB) trimmed = floorB;
                        if (trimmed < capsToWrite.maxB) {
                            policyResolver.cap(std::numeric_limits<int>::max(), trimmed,
                                               "thermal", "joint_dvfs");
                            capsToWrite.maxB = trimmed;
                            if (capsToWrite.minB > capsToWrite.maxB) capsToWrite.minB = capsToWrite.maxB;
                        }
                    }
                    // ── 功率驱动压制/补救(powerd 式): 在 FSM caps 之上叠加 —— 卡顿抬上限+开 boost, 过载压上限+关核+限 GPU ──
                    // 采样融入 auto: 由 computePwrLevel 统一算档。
                    // 档位: 0=卡顿(开 boost + 抬大核上限补救) 1=中性(不动, 省电) 2=高功耗(压大核上限+限 GPU) 3=临界(深压+关大核+限 GPU)。
                    // boost 段同步切 UCLAMP(cgroup v2/sysctl/sched_boost 三级回退)。
                    // 套时间门控: 最短驻留/单次上限/冷却, 防 PSI 在阈值附近抖动导致 boost 反复开关
                    // (每次开关都要写 uclamp/sched_boost, 高通内核上 sched_boost 还会触发跨簇迁移)。
                    const int pwrLevel = pwrBoostGate(computePwrLevel(mx));
                    // 卡顿档(L0): 适度抬高 FSM 大核上限(给 boost 下限留空间, 否则下限被上限夹住还是卡)。
                    // 只抬不压: hwmax × cap_pct 与 FSM caps 取更宽者(不削弱 FSM 已有的省电 caps, 仅在卡顿时放宽)。
                    if (pwrLevel == 0) {
                        int pct = Config::AutoMode::power_boost_cap_pct;
                        int hwB = autoFsm_.hwMaxB();
                        if (hwB > 0 && pct > 0 && pct <= 100) {
                            int capB = hwB * pct / 100;
                            if (capB > capsToWrite.maxB) {
                                capsToWrite.maxB = capB;
                                if (capsToWrite.maxB > hwB) capsToWrite.maxB = hwB;   // 不超硬件极限
                            }
                        }
                    }
                    // 高/临界档: 大核上限压到 hwmax × pct/100(只压不抬, 取更严者)
                    if (pwrLevel >= 2) {
                        int pct = (pwrLevel == 3) ? Config::AutoMode::power_throttle_critical_pct
                                                   : Config::AutoMode::power_throttle_big_pct;
                        int hwB = autoFsm_.hwMaxB();
                        if (hwB > 0 && pct > 0 && pct < 100) {
                            int capB = hwB * pct / 100;
                            int floorB = autoFsm_.hwMinB();
                            if (capB < floorB) capB = floorB;
                            policyResolver.cap(0, capB, "power",
                                               pwrLevel == 3 ? "critical_power" : "high_power");
                            if (capB < capsToWrite.maxB) {
                                capsToWrite.maxB = capB;
                                if (capsToWrite.minB > capsToWrite.maxB) capsToWrite.minB = capsToWrite.maxB;
                            }
                        }
                    }
                    // 临界档(≥critical_w)强制关大核(保命): reconcileAutoCores 读取此标志, 旁路 6s 迟滞立即停泊。
                    // 旁路/非临界 → false, 大核自动上线(上线即时, 可逆)。
                    pwrParkBig_ = (pwrLevel == 3);
                    applyPowerBoost(pwrLevel);
                    if (lowBatterySafe) {
                        const int lowL = autoFsm_.hwMaxL() > 0
                            ? std::max(autoFsm_.hwMinL(), autoFsm_.hwMaxL() * 75 / 100) : capsToWrite.maxL;
                        const int lowB = autoFsm_.hwMaxB() > 0
                            ? std::max(autoFsm_.hwMinB(), autoFsm_.hwMaxB() * 65 / 100) : capsToWrite.maxB;
                        policyResolver.cap(lowL, lowB, "battery", "low_battery");
                    }
                    // 所有已登记 cap 最后统一收紧，power boost 不得重新放开 FAS/类别/相机/热限制。
                    // 仍由原有 writer 应用，风驰分支保持独立。
                    policyResolver.appPolicy({capsToWrite.minL, capsToWrite.maxL,
                                              capsToWrite.minB, capsToWrite.maxB},
                                             inGaming ? "game" : "fsm",
                                             pwrLevel == 0 ? "power_boost" : "state");
                    const kin::PolicyCaps& resolvedCaps = policyResolver.result().caps;
                    capsToWrite.minL = resolvedCaps.minL;
                    capsToWrite.maxL = resolvedCaps.maxL;
                    capsToWrite.minB = resolvedCaps.minB;
                    capsToWrite.maxB = resolvedCaps.maxB;
                    writeFsmCaps(capsToWrite);
                    autoMode_.setLastWriteStatus(utils.writeBatchStatus(),
                                                 utils.writeBatchNodeSummary(),
                                                 utils.writeBatchFailedNodes());
                    // 每状态 GPU 上限(对齐 ASB):空闲低、游戏满。仅在能读到合理 GPU 硬件最高频时启用,
                    // 否则跳过(沿用 config GPU)。改 GpuFreq::max_mhz 原子,由 GPU 守护线程每秒落实。
                    if (Config::GpuFreq::enable) {
                        int gpuHw = function.gpuHwMaxMhz();
                        // 用户 fsm_policy 的 gpu(>0)覆盖内置每状态 GPU 上限; 0=未配置用内置。
                        // 类别 GPU 封顶(只压不抬)仍在其后叠加, 语义不变。
                        const int userGpu = fpol.gpu;
                        int pct = (userGpu > 0) ? userGpu : AutoFsm::gpuPctForState(autoFsm_.state());
                        if (tickCfg.appPolicyEnable && !fasEngaged &&
                            cpol.gpuPct > 0 && cpol.gpuPct < pct) pct = cpol.gpuPct;   // 类别 GPU 封顶(只压不抬)
                        if (gpuHw > 0 && pct > 0) {
                            int gmax = gpuHw * pct / 100;
                            // 功率过载限 GPU(只压不抬, 对齐 powerd GPU_CAP_HEAT/CRIT): 高/临界档取绝对 MHz 上限,
                            // 与 FSM 每状态上限取更严者。0=不限(沿用 gmax)。
                            if (pwrLevel >= 2) {
                                int gpuCap = (pwrLevel == 3) ? Config::AutoMode::power_throttle_critical_gpu_mhz
                                                             : Config::AutoMode::power_throttle_gpu_mhz;
                                if (gpuCap > 0 && gpuCap < gmax) gmax = gpuCap;
                            }
                            if (gmax != lastGpuMax) {
                                Config::GpuFreq::max_mhz.store(gmax, std::memory_order_relaxed);
                                lastGpuMax = gmax;
                            }
                        }
                    }
                    }   // end if (fengchiActive) else
                    // 每拍同步放权窗口，热重载关闭 gaming_gov 后会立即恢复普通 governor 管理。
                    applyGamingGov(fengchiActive);
                    // ── P2-⑥ uclamp: game_heavy 风驰游戏给 top-app 设性能下限(同时抬调频+把高需求任务
                    // 放大核), 仅前台受益、后台不陪跑。边沿驱动, 离开还原 0。内核无接口则一次性跳过。
                    applyTopAppUclampMin(fgHeavy ? tickCfg.gameUclampMin : 0);
                    // ── P3-⑪ 天玑 FPSGO: game_heavy 且开关开时强开厂商帧引擎(已与 FAS 互斥), 离开还原。
                    applyFpsgo(fpsgoOn);
                    // ── 相机保持(ASB V60 camera_hold): 前台相机 → 接管 cpuset/uclamp(边沿驱动)。
                    // 风驰接管时已强制 false(上方); 此处处理非风驰态下的相机进出沿。
                    applyCameraHold(cameraHoldWant);
                    // ── 息屏 DEEP_IDLE 背景 uclamp 上限(ASB V64): 只压不抬, 离开 DEEP_IDLE 条件还原 ──
                    applyDeepIdleBgUclamp(autoFsm_.state() == AutoFsm::DEEP_IDLE);
                    // Vendor frequency limits are read-only telemetry, never overridden.
                    // ── 关核心(可逆)对齐: FAS 接管(游戏帧控)时绝不关核(传 0=game); 否则按前台类别。
                    // game/undefined/未知 或 关核心未开 → desiredOnlineForCat 返回全核 → 自动切回(可逆)。
                    // 关核心开时本函数为在线集合唯一权威; 关核心关时 want 恒全 1, 与 onlineAll 一致。去重避免重复写。
                    reconcileAutoCores(fasEngaged ? 0 : fgCat, inGaming || stickyHolding);
                }
                // ── G1: auto 档让 SysTune(每档 VM/网络/WiFi) 跟随自学习基础档(移植 ASB V57 smart 跟档)──
                // 复用本 tick 已算 alpha 解析基础档, 边沿触发: 仅基础档变化才重下发(纯 sysctl, 不进逐 tick 写循环)。
                followAutoSysTune(autoMode_.decide(mx, alpha));
                // 日志节拍: 除状态切换外, 前台包名或类别变化也打印(否则 App 切换不改状态时日志看似冻住);
                // 另加 60s 心跳, 让用户能看出还活着。INFO 保持原有单行简洁。
                {
                    const std::string curFgKey = fgPkg.empty() ? std::string("-") : fgPkg;
                    const bool fgChanged  = (curFgKey != lastLoggedFgPkg_) || (fgCat != lastLoggedFgCat_);
                    // 心跳仅亮屏时打: 息屏前台恒为空(不取前台), 60s 刷「前台: -」纯噪声且误导成"息屏还在检测前台"。
                    // 息屏进入时由 fgChanged 打一行即止, 之后静默到亮屏有真实前台。
                    const bool heartbeat  = (mx.screenOn != 0) && (lastLogMs_ != 0) && (nowMs() - lastLogMs_ >= 60000);
                    if (changed || autoFsm_.state() != lastFsmState || fgChanged || heartbeat) {
                        // INFO 统一格式(用户指定): 前台变化 / FSM 状态切换 / 60s 心跳, 一律打同一行
                        // 「前台: xxx 类型=N FSM=状态」, 不再有单独的 FSM= 行。详细诊断仍在 Debug。
                        // 诊断字段不放在括号里 —— Logger 的 stripParens 会把 (...) 当注释删掉。
                        const bool stateChanged = (autoFsm_.state() != lastFsmState);
                        if (fgChanged || heartbeat || stateChanged)
                            logger.Info("前台: %s 类型=%d FSM=%s", fgPkg.empty() ? "-" : labelForLog(fgPkg).c_str(),
                                        fgCat, autoFsm_.stateName());
                        logger.Debug("自动FSM: %s alpha=%d 小核≤%dMHz 大核≤%dMHz · CPU=%d%% 加权=%d%% PSI=%d GPU=%d 温=%d 热余量=%d load=%.2f 类=%d 前台=%s",
                                    autoFsm_.stateName(), alpha, capsToWrite.maxL / 1000, capsToWrite.maxB / 1000,
                                    mx.cpuBusy, mx.cpuBusyW, mx.cpuPsi, mx.gpuPct, mx.tempC, mx.adpfHr, mx.load1, fgCat,
                                    fgPkg.empty() ? "-" : fgPkg.c_str());
                        lastFsmState = autoFsm_.state();
                        lastLoggedFgPkg_ = curFgKey;
                        lastLoggedFgCat_ = fgCat;
                        lastLogMs_ = nowMs();
                    }
                }
                if (fasEngaged && nowMs() - lastFasLogMs_ >= 1000) {
                    // ~1Hz 节流: FAS 拍率已到 5Hz(掉帧期 ~8Hz), 每拍打 Info 一小时能刷几万行日志
                    lastFasLogMs_ = nowMs();
                    logger.Info("FAS: 目标=%dfps 实测=%.1ffps 小核≤%dMHz 大核≤%dMHz",
                                autoFas_.targetFps(), autoFas_.lastFpsX10() / 10.0,
                                capsToWrite.maxL / 1000, capsToWrite.maxB / 1000);
                }
                autoMode_.setForegroundApp(observedForeground.c_str());
                // 游戏态自动免打扰: 进 GAMING 切「仅优先」, 离开立刻还原(engage/release 幂等)。
                // 默认关; 用户中途关掉开关时也会走到 else 分支把之前的临时值还回去。
                if (Config::AutoMode::auto_dnd) {
                    if (autoFsm_.state() == AutoFsm::GAMING) dnd_.engage();
                    else                                     dnd_.release();
                } else {
                    dnd_.release();
                }
                if (fsmDue) {
                    // 附加运行态（闭环 + 帧级分析）—— 只上报可判定值，不上报内部计数器。
                    // loadgov_perf 用 **x1000 性能比**而不是 kHz：它表达的是"闭环意图"，
                    // 与 cpu_max_*（最终落盘值）不等时，说明被 FAS/类别/热/PolicyResolver 又收紧了。
                    AutoModeCtl::AuxRuntime aux;
                    aux.loadGovEnable   = Config::AutoMode::loadgov_enable;
                    aux.loadGovBusyL    = mx.clusterBusy[0];
                    aux.loadGovBusyB    = mx.clusterBusy[1];
                    aux.loadGovPerfLX1000 = loadGovPrimed_[0]
                        ? (int)(loadGov_[0].currentPerf * 1000.0f + 0.5f) : -1;
                    aux.loadGovPerfBX1000 = loadGovPrimed_[1]
                        ? (int)(loadGov_[1].currentPerf * 1000.0f + 0.5f) : -1;
                    aux.fasFrameAnalysis = autoFas_.frameAnalysisOn();
                    aux.fasLoading       = autoFas_.loadingActive();
                    aux.fasPostGuard     = autoFas_.postGuardFrames();
                    aux.fasGearDwell     = autoFas_.gearDwellFrames();
                    aux.fasPerFrame      = autoFas_.usingPerFrame();
                    autoMode_.writeFsmStatus(mx, alpha, autoFsm_.stateName(),
                                             capsToWrite.maxL / 1000, capsToWrite.maxB / 1000,
                                             policyResolver.result(), fasEngaged, fengchiActive, aux);
                }
                // 自适应 tick(省唤醒): 息屏 30s; FAS 接管/预热期按 FAS 轮询节奏(200ms 闭环,
                // measured_fps ~1s 窗口, 5Hz 足够; 预热若 8s 没接管则 warmingUp 放弃快节奏);
                // 亮屏其余情形: 负载闭环开启时用 loadgov_tick_ms(缺省 600ms), 否则回旧口径
                // (LIGHT_IDLE 4s / 中载及以上 2s)。前台一变仍由 wakeFsm 立即叫醒。
                //
                // 【为什么 LIGHT_IDLE 也跑快拍】轻载正是闭环最该把上限压下来的时段 —— 原来 4s
                // 一拍意味着静止时上限 4 秒才可能变一次, 对负载完全没有反馈。代价是唤醒次数上升,
                // 故负载闭环只读已在缓存 fd 上的 /proc/stat(见 AutoMode::readWeightedBusy), 无 popen。
                int ivMs;
                if (mx.screenOn == 0)                              ivMs = 30000;
                else if (fasEngaged)                              ivMs = autoFas_.pollIntervalMs();
                else if (fasWasGaming && autoFas_.warmingUp((long)nowMs()))
                                                                  ivMs = autoFas_.pollIntervalMs();
                else if (tickCfg.loadGovEnable)                    ivMs = tickCfg.loadGovTickMs;
                else if (autoFsm_.state() == AutoFsm::LIGHT_IDLE) ivMs = 4000;
                else                                              ivMs = kFsmTickMs;
                // ── Quiet Night(ASB V61): 夜间 + 息屏 + 未充电 → 大幅拉长 tick 让设备深睡 ──
                // 在息屏 30s 基础上, 命中夜间安静窗口则拉到 quiet_night_tick_ms(默认 5min)。
                // 亮屏/充电/出窗口 → 立即恢复正常 tick(下一拍自然由上方分支决定)。
                if (tickCfg.quietNightEnable && mx.screenOn == 0 && !mx.charging
                    && autoMode_.inQuietNightWindow()) {
                    ivMs = tickCfg.quietNightTickMs;
                }
                if (waitFsmTick(ivMs)) break;
                continue;
            }
        }   // autoModeTask 主循环结束
        // 线程退出(守护停止/重启): 游戏态调速器若在切换态, 条件还原, 不把 hm/scx 留给死守护
        { std::lock_guard<std::recursive_mutex> lk(applyMtx); applyGamingGov(false, "auto线程退出"); applyTopAppUclampMin(0); applyPowerBoost(-1); applyFpsgo(false); applyCameraHold(false); applyDeepIdleBgUclamp(false); pwrParkBig_ = false; }
    }

    static constexpr int kFsmTickMs = 2000;   // FSM 亮屏 tick 固定 2s(对齐 ASB)
    static int clampTick(int s) { return s < 2 ? 2 : (s > 120 ? 120 : s); }

    // 读本簇 cpuinfo_max_freq(kHz)并缓存; 失败/未配置返回 0。
    int clusterHwMaxKHz(int i) {
        if (i < 0 || i >= Config::kClusterCount) return 0;
        if (clusterHwMaxKHz_[i] > 0) return clusterHwMaxKHz_[i];
        int pol = Config::Policy::CpuPolicy[i];
        if (pol < 0) return 0;
        char p[256];
        FastSnprintf(p, sizeof(p),
                     "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq", pol);
        int v = utils.readInt(p);
        if (v > 0) clusterHwMaxKHz_[i] = v;
        return clusterHwMaxKHz_[i];
    }

    // 读本簇 cpuinfo_min_freq(kHz)并缓存; 失败回退 300000(300MHz, 与 AutoFsm::hwMinL/B 地板一致)。
    // 供风驰游戏态把 CPU 下限顶到硬件地板(与 clusterHwMaxKHz 对称, 一并写硬件极限)。
    int clusterHwMinKHz(int i) {
        if (i < 0 || i >= Config::kClusterCount) return 300000;
        if (clusterHwMinKHz_[i] > 0) return clusterHwMinKHz_[i];
        int pol = Config::Policy::CpuPolicy[i];
        if (pol < 0) return 300000;
        char p[256];
        FastSnprintf(p, sizeof(p),
                     "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_min_freq", pol);
        int v = utils.readInt(p);
        if (v > 0) clusterHwMinKHz_[i] = v;
        return clusterHwMinKHz_[i] > 0 ? clusterHwMinKHz_[i] : 300000;
    }

    // ── 负载闭环调频（P0-2 / P0-3）────────────────────────────────────────────
    // 把 config.json 的 loadgov_* 整数百分比同步进 LoadGovParams（配置热重载即时生效）。
    // 每次调用都重建一次（纯值类型，开销可忽略），避免缓存一份可能过期的参数。
    void syncLoadGovParams() {
        kin::LoadGovParams p;
        p.upThreshold       = (float)Config::AutoMode::loadgov_up_pct / 100.0f;
        p.downThreshold     = (float)Config::AutoMode::loadgov_down_pct / 100.0f;
        p.headroomFactor    = (float)Config::AutoMode::loadgov_headroom_pct / 100.0f;
        p.smoothingUp       = (float)Config::AutoMode::loadgov_smooth_up_pct / 100.0f;
        p.writeDeadzone     = (float)Config::AutoMode::loadgov_deadzone_pct / 100.0f;
        p.writeDwellMs      = Config::AutoMode::loadgov_dwell_ms;
        // T11 稳态下探
        p.steadyDecayEnabled     = Config::AutoMode::loadgov_steady_enable;
        p.steadyDecayTargetRatio = (float)Config::AutoMode::loadgov_steady_target_pct / 100.0f;
        p.steadyDecayStep        = (float)Config::AutoMode::loadgov_steady_step_x1000 / 1000.0f;
        p.steadyDecayMax         = (float)Config::AutoMode::loadgov_steady_max_pct / 100.0f;
        p.normalize();   // 越界/NaN 自愈（config 层已就地校验过一次，这里是第二道闸）
        loadGovParams_ = p;
    }

    // 复位闭环状态：下次 tick 从“当前 FSM 预算”起步。
    // 调用时机 = 进/退 auto、息屏→亮屏沿、配置重载 —— 这些时刻预算可能整体跳变，
    // 带着旧的 currentPerf 跑会把新预算当成“负载变化”去追（ChiRi 的同款教训：
    // reload_config 必须重放 perf_init，否则接管间隙 currentPerf 会掉到底再慢慢爬）。
    void resetLoadGovernor() {
        for (int i = 0; i < 2; ++i) {
            loadGovPrimed_[i]      = false;
            loadGovLastWriteMs_[i] = 0;
        }
    }

    // 在 FSM 预算内按本簇负载收紧 caps.maxL/maxB。
    // 调用位置：computeCaps + 用户 fsm_policy 覆盖之后、FAS/类别/热等后续 cap 之前 ——
    // 保证“预算 -> 闭环收紧 -> 各方硬限制”的顺序（后续每一层仍可继续收紧）。
    //
    // 语义：**只能收紧**。闭环输出与预算取 min，永不抬高；因此关闭本层即逐位回退到原行为。
    void applyLoadGovernor(AutoFsm::Caps& caps, const int clusterBusy[], long long nowMs) {
        if (!Config::AutoMode::loadgov_enable) return;
        syncLoadGovParams();

        const int hwMax[2] = { autoFsm_.hwMaxL(), autoFsm_.hwMaxB() };
        const int hwMin[2] = { autoFsm_.hwMinL(), autoFsm_.hwMinB() };
        int*      budget[2] = { &caps.maxL, &caps.maxB };

        for (int i = 0; i < 2; ++i) {
            if (hwMax[i] <= 0) continue;          // 未探到硬件频率 -> 本簇不接管
            const int busy = clusterBusy[i];
            if (busy < 0) continue;               // 本拍读不到逐核负载 -> 保持原预算，不猜

            // 驻留门（缺省 0 = 关）：未满驻留就整簇冻结，不调 step()。
            // 不能“step 完再丢写入” —— 那会让状态机与 sysfs 失步，下一拍以新状态去追旧值。
            if (!kin::dwellElapsed(nowMs, loadGovLastWriteMs_[i], loadGovParams_.writeDwellMs)) continue;

            // 首次：从当前预算起步（而不是从固定的 perfInit），否则第一拍会把预算当成跳变。
            if (!loadGovPrimed_[i]) {
                float initPerf = (float)*budget[i] / (float)hwMax[i];
                if (initPerf > 1.0f) initPerf = 1.0f;
                loadGov_[i].reset(initPerf);
                loadGovPrimed_[i] = true;
            }

            const float util = (float)busy / 100.0f;
            const kin::GovStep st = kin::step(loadGov_[i], loadGovParams_, util);

            // 写频计数：仅真实 Up/Down 才刷新驻留时钟（Hold / 等待确认不算一次写）。
            if (st.decision == kin::GovDecision::Up || st.decision == kin::GovDecision::Down)
                loadGovLastWriteMs_[i] = nowMs;

            int khz = (int)(st.effPerf * (float)hwMax[i]);
            if (khz < hwMin[i]) khz = hwMin[i];
            // 只能收紧：闭环永远不会把预算抬高（“抬频买响应”应调升频门槛，不是拆这一层）
            if (khz < *budget[i]) *budget[i] = khz;
        }
    }

    // FSM 直接写各簇频率上限(小核=cluster0, 大核=cluster1, 超大核=cluster2+)。governor 沿用已预置
    // 基础档的调速器(即只改频率界, 不动 governor → 簇内仍由 walt 自主调频, 契合 Kin 理念)。
    // FSM 只写【上限 scaling_max】+ governor,【不碰下限 scaling_min】(下限在 prime 时放开到 hw-min)。
    // 这样内核 input boost 抬起的下限不会被覆盖(触控加速才生效),闲时频率也能自然落到最低省电。
    void writeFsmCaps(const AutoFsm::Caps& c) {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        utils.beginWriteBatch();
        char bmx[16];
        const int bigHw = clusterHwMaxKHz(1);   // 大核簇硬件最高频, 供超大核等比缩放
        // 覆盖【所有】在线簇, 不只 cluster0/1: 三簇旗舰(8Gen2/3、天玑9300/9400 等)的超大核(prime,
        // index>=2)若不写 scaling_max, 在 primeAutoFsm 放开下限后会在负载下自由冲到硬件最高频,
        // 智能档省电形同虚设(用户感知: 智能档下 CPU7 仍满频)。AutoFsm::Caps 只建模小核/大核两档,
        // 故超大核按本簇硬件最高频等比缩放大核上限 —— 游戏态(maxB≈大核 hw-max)时 prime 也能到本簇满频。
        for (int i = 0; i < Config::kClusterCount; i++) {
            const int pol = Config::Policy::CpuPolicy[i];
            if (pol < 0) continue;
            int cap;
            if (i == 0) {
                cap = c.maxL;
            } else if (i == 1) {
                cap = c.maxB;
            } else {
                int primeHw = clusterHwMaxKHz(i);
                if (primeHw > 0 && bigHw > 0) {
                    long scaled = (long)c.maxB * primeHw / bigHw;
                    if (scaled > primeHw) scaled = primeHw;
                    cap = (int)scaled;
                } else {
                    cap = c.maxB;   // 读不到 hw-max 则退回大核上限(至少不再放飞)
                }
            }
            // ROM 接管风驰游戏态时 governor 必须留空，避免 FSM 每拍写回基础 governor。
            const string_t gov = gamingGovActive_
                ? string_t() : resolveGovernor(i, Config::Performances::CpuGovernor[i]);
            FastSnprintf(bmx, sizeof(bmx), "%d", cap);
            FreqWriter(pol, string_t(), string_t(bmx), gov);   // 空下限 = 不写 scaling_min
        }
        utils.endWriteBatch();
    }

    // ── 风驰游戏态: 主动写硬件极限频率(替代 FSM caps 接管) ──
    // 触发条件: auto 模式 + FSM + game_heavy(风驰游戏)画像命中(由 fengchiTakeover 驱动)。
    // 行为: 各簇 scaling_min=cpuinfo_min_freq, scaling_max=cpuinfo_max_freq(主动写硬件极限, 非放手);
    //       GPU min_mhz=硬件最低频, max_mhz=硬件最高频(主动写硬件极限)。
    // governor 由 ROM/OIface 接管，本函数不碰 governor（传空，与 writeFsmCaps 同款）。
    // 退出(离开 game_heavy): 调用方还原 governor + 让 FSM 重新接管 caps + GPU 交回 FSM 每状态上限。
    // 返回 true 表示本拍已接管(调用方跳过 writeFsmCaps/FAS/类别钳制/GPU 每状态上限); false 表示未接管。
    bool applyFengchiHwCaps() {
        // CPU: 逐簇写 cpuinfo_min ~ cpuinfo_max
        char bmin[16], bmax[16];
        for (int i = 0; i < Config::kClusterCount; i++) {
            int pol = Config::Policy::CpuPolicy[i];
            if (pol < 0) continue;
            int hmin = clusterHwMinKHz(i);
            int hmax = clusterHwMaxKHz(i);
            if (hmax <= 0) continue;   // 读不到硬件上限的簇跳过(避免误写 0)
            if (hmin <= 0 || hmin > hmax) hmin = hmax;   // 防倒挂
            // governor 传空: 风驰调速器由 applyGamingGov 管辖, 不在此重申(否则会盖回风驰)。
            FastSnprintf(bmin, sizeof(bmin), "%d", hmin);
            FastSnprintf(bmax, sizeof(bmax), "%d", hmax);
            FreqWriter(pol, string_t(bmin), string_t(bmax), string_t());
        }
        // GPU: 主动写硬件极限(min=硬件最低, max=硬件最高)。改 GpuFreq::min/max_mhz 原子,
        // 由 GPU 守护线程每秒落实并纠漂(与 FSM 每状态上限同一通道, 切回 FSM 时复用同一原子)。
        if (Config::GpuFreq::enable) {
            int gmin = function.gpuHwMinMhz();
            int gmax = function.gpuHwMaxMhz();
            if (gmax > 0) {
                if (gmin <= 0 || gmin > gmax) gmin = gmax;   // 防倒挂
                Config::GpuFreq::min_mhz.store(gmin, std::memory_order_relaxed);
                Config::GpuFreq::max_mhz.store(gmax, std::memory_order_relaxed);
            }
        }
        return true;
    }

    // 从 /sdcard/Android/Kin/AI/pkg_label.txt 加载 pkg→App名字 映射(App 写, 供日志显示 App 名字)。
    // 每行 "包名<空白>显示名"(显示名可含空格, 取包名后整段); # 注释; 读不到则清空为空表(退回包名)。
    void loadAppLabels() {
        static const char* paths[] = {
            "/sdcard/Android/Kin/AI/pkg_label.txt",
            "/storage/emulated/0/Android/Kin/AI/pkg_label.txt",
        };
        FILE* fp = nullptr;
        for (const char* p : paths) { fp = fopen(p, "r"); if (fp) break; }
        std::map<std::string, std::string> m;
        if (fp) {
            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                char* s = line;
                while (*s == ' ' || *s == '\t') s++;
                size_t len = strlen(s);
                while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' ' || s[len-1] == '\t'))
                    s[--len] = '\0';
                if (len == 0 || s[0] == '#') continue;
                char* sep = s;
                while (*sep && *sep != ' ' && *sep != '\t') sep++;
                if (!*sep) continue;   // 无分隔 = 无名字, 跳过
                *sep = '\0';
                char* label = sep + 1;
                while (*label == ' ' || *label == '\t') label++;
                if (!*label) continue;
                m.emplace(s, label);
            }
            fclose(fp);
        }
        { std::lock_guard<std::mutex> lk(appLabelsMtx_); appLabels_.swap(m); }
    }

    // 日志用: 有 App 名字就返回名字, 否则原样返回包名。返回拷贝, 避免持锁外引用失效。
    std::string labelForLog(const std::string& pkg) {
        std::lock_guard<std::mutex> lk(appLabelsMtx_);
        auto it = appLabels_.find(pkg);
        return (it != appLabels_.end()) ? it->second : pkg;
    }

    // 统一频率应用入口：有匹配画像则用画像，否则用基础 mode
    void applyWithProfile() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        int matchIdx = Config::AppProfile::currentMatch.load();
        if (Config::AppProfile::shouldApplyProfile(matchIdx)) {
            applyAppProfile(matchIdx);
        } else {
            applyBaseMode(); // 内部已先 restoreBaseCoresIfNeeded, 不再重复
        }
    }

    void restoreBaseCoresIfNeeded() {
        bool anyDefined = false;
        bool anyOnline  = false;
        for (int i = 0; i < kCoreCount; i++) {
            if (Performances::Online[i] != -1) anyDefined = true;
            if (Performances::Online[i] == 1)  anyOnline  = true;
        }
        if (anyDefined) {
            online();
            // 等新上线的簇稳定后再写 governor，hotplug 异步，内核不自动还原 cpuset
            for (int i = 0; i < kCoreCount; i++)
                if (Performances::Online[i] == 1) waitClusterReady(policyForCore(i));
            if (anyOnline) function.cpusetFunction();
        }
    }

    void online() {
        // auto(智能档)永不按基础档关核 —— 一律全核在线。这是核心控制的唯一闸口:
        // applyAllConfig()/init()/restoreBaseCoresIfNeeded() 都在 applyCurrentMode(→primeAutoFsm
        // 已 onlineAll)之后又调 online(), 若按 Performances::Online[](残留均衡/省电档=Core7=0)
        // 写就会把刚开的 CPU7 又关掉。故 auto 下统一改走 onlineAll()。
        if (autoActive_.load()) { onlineAll(); return; }
        char path[256];
        for (int i = 0; i < kCoreCount; i++) {
            if (Performances::Online[i] == -1) continue; // -1 = 跳过，不误关核心
            FastSnprintf(path, sizeof(path), onlinePath, i);
            utils.WriteInt(path, Performances::Online[i]);
            logger.Debug("核心: %d %s", i, Performances::Online[i] ? "开启" : "关闭");
        }
    }

    // auto(智能档)专用: 无条件全核在线, 不读基础档 Online[]。
    // auto 自成一档不挂均衡/省电基础档, 但 Performances::Online[] 仍残留上次加载的值
    // (如均衡档 Core7=0), restoreBaseCoresIfNeeded 会照搬而误关 CPU7。auto 一律全开。
    void onlineAll() {
        char path[256];
        for (int i = 0; i < kCoreCount; i++) {
            FastSnprintf(path, sizeof(path), onlinePath, i);
            utils.WriteInt(path, 1);
        }
        for (int i = 0; i < kClusterCount; i++) {
            int pol = Config::Policy::CpuPolicy[i];
            if (pol >= 0) waitClusterReady(pol);
        }
        function.cpusetFunction();   // hotplug 异步, 上线后重写 cpuset 归位
        // 全核已在线: 同步去重缓存, 让 reconcileAutoCores() 知道当前集合=全 1, 避免下一 tick 多余写。
        for (int i = 0; i < kCoreCount; i++) lastAutoOnline_[i] = 1;
    }

    // ── 关核心(可逆): 计算某类别在 auto 下的【期望在线集合】, 并施加硬护栏 ──
    // 规则(硬编码, 不可配): game(cat 0)/undefined(-1)/未知类别 → 全核在线;
    // 分类封顶总开关或关核开关任一关闭 → 全核在线; 其余按该类别 cores[] 掩码,
    // 但: 永不关 cpu0; 首簇(含 cpu0)至少保留 1 核在线(非首簇允许整簇停泊)。
    void desiredOnlineForCat(int fgCat, int out[kCoreCount]) {
        for (int i = 0; i < kCoreCount; i++) out[i] = 1;   // 默认全核(对齐 onlineAll)
        // 关核从属于「分类封顶」总开关: 总开关关掉 = 整个分类策略停用, 关核也一并停,
        // 否则 UI 上"分类封顶已关"却还在关核, 与开关文案矛盾。
        if (!Config::AutoMode::app_policy_enable ||
            !Config::AutoMode::app_policy_core_enable) return;
        if (fgCat < 0 || fgCat == 0) return;        // game(0)/undefined(-1)/未识别 → 全核, 绝不关
        const Config::AutoMode::CatPolicy p = Config::AutoMode::catPolicy(fgCat);
        int want[kCoreCount];
        for (int i = 0; i < kCoreCount; i++) want[i] = (p.cores[i] == 0) ? 0 : 1;
        want[0] = 1;                                 // 硬护栏: 永不关 cpu0
        // 硬护栏: 仅【首簇】(start==0, 即 cluster0/perf 含 cpu0)至少保留 1 核在线 —— 永不整芯下线。
        // 非首簇(prime/big, 如 8E5 cluster1=cpu6-7)允许【整簇停泊】, 满足 video/audio/reading 全关 prime。
        for (int c = 0; c < kClusterCount; c++) {
            int start = Config::Policy::CpuPolicy[c];
            if (start != 0) continue;                // 只护首簇(cluster0); 其余簇(及 -1 填充)可整簇离线
            int end = kCoreCount;                    // 下一有效簇起点 = 本簇末尾
            for (int d = 0; d < kClusterCount; d++) {
                int s2 = Config::Policy::CpuPolicy[d];
                if (s2 > start && s2 < end) end = s2;
            }
            bool anyOn = false;
            for (int i = start; i < end && i < kCoreCount; i++) if (want[i] == 1) anyOn = true;
            if (!anyOn && start < kCoreCount) want[start] = 1;   // 首簇被全关 → 强留簇首(=cpu0)
        }
        for (int i = 0; i < kCoreCount; i++) out[i] = want[i];
    }

    // 每 tick 把在线集合【对齐】到期望集合: 仅写有变化的核(去重), 先上线后下线 → 切回不关核的
    // 类别时之前停泊的核会被重新上线(可逆)。仅在 auto 下调用, 持 applyMtx。
    // 升快降慢: 上线即时; 【下线(停泊)带 6s 墙钟迟滞】——同一类别稳定 6s 才停核,
    // 游戏/粘滞期间不预攒计时, 类别毛刺(小窗横跳/瞬时 -1)每次都清零计时 → 核心不再跟着开关抖。
    // 判断某 CPU 是否属于大核簇(cluster1, Policy::CpuPolicy[1] 起, 到下一有效簇起点)。
    // 用 Kin 既有的簇映射(比 cpu_capacity 探测更可靠: hwMaxB/hwMinB 也基于此簇)。非首簇允许整簇停泊。
    bool isBigClusterCpu(int cpu) {
        int start = Config::Policy::CpuPolicy[1];
        if (start < 0) return false;
        int end = kCoreCount;
        for (int d = 0; d < kClusterCount; d++) {
            int s = Config::Policy::CpuPolicy[d];
            if (d != 1 && s > start && s < end) end = s;
        }
        return cpu >= start && cpu < end;
    }

    void reconcileAutoCores(int fgCat, bool gamingNow) {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        if (!autoActive_.load()) return;
        int want[kCoreCount];
        desiredOnlineForCat(fgCat, want);
        // 临界功率(≥power_throttle_critical_w)强制关大核(保命): 覆盖类别在线集合, 不受关核总开关影响。
        // 大核簇=cluster1(非首簇, 允许整簇停泊); cpu0 永不在大核簇, 安全护栏天然满足。
        if (pwrParkBig_) {
            for (int i = 0; i < kCoreCount; i++)
                if (isBigClusterCpu(i)) want[i] = 0;
        }
        char path[256];
        bool changed = false;
        // 停泊迟滞计时: 游戏中强制重置; 类别变化重置。
        const long long now = nowMs();
        if (gamingNow || fgCat != parkCat_) { parkCat_ = gamingNow ? INT_MIN : fgCat; parkCatSince_ = now; }
        const bool parkOk = (now - parkCatSince_ >= kParkHystMs);
        // 与基础档 online() 完全相同的关核写法: 写 /sys/devices/system/cpu/cpuN/online。先上线后下线, 去重。
        // (基础档省电档 Core6/7=0 就是这样关核且生效的, 直接复用。)切回不停核类别会把核写回 1(可逆)。
        for (int i = 0; i < kCoreCount; i++) {       // 先上线(即时)
            if (want[i] == 1 && lastAutoOnline_[i] != 1) {
                FastSnprintf(path, sizeof(path), onlinePath, i);
                utils.WriteInt(path, 1);
                lastAutoOnline_[i] = 1;
                changed = true;
            }
        }
        for (int i = 0; i < kCoreCount; i++) {       // 再下线(过迟滞才停)
            if (want[i] == 0 && lastAutoOnline_[i] != 0) {
                // 类别停泊需满 6s 迟滞; 临界功率停泊立即生效(保命, 旁路迟滞)。
                if (!parkOk && !pwrParkBig_) continue;
                FastSnprintf(path, sizeof(path), onlinePath, i);
                utils.WriteInt(path, 0);
                lastAutoOnline_[i] = 0;
                changed = true;
            }
        }
        // 诊断: 实际改了核才打一行, 让用户能在日志直接核对(总开关是否开/期望在线集合/类别)。
        if (changed) {
            char m[kCoreCount + 1];
            for (int i = 0; i < kCoreCount; i++) m[i] = (char)('0' + (want[i] ? 1 : 0));
            m[kCoreCount] = '\0';
            logger.Debug("关核: 类=%d 总开关=%d 功率停泊=%d 期望在线=%s 已写 cpuN/online",
                        fgCat, Config::AutoMode::app_policy_core_enable ? 1 : 0, pwrParkBig_ ? 1 : 0, m);
        }
    }

    void SchedParam() {
        // auto(智能档): 不写 config 的 governor 调速参数(up_rate_limit_us 等),让 governor 跑内核默认、
        // 完全由 FSM/智能定频,而非按 config 静态配置。 (用户要求: auto 下不读 config 参数)
        if (autoActive_.load()) return;
        char path[256];
        for (int i = 0; i < kClusterCount; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (Performances::CpuGovernor[i].empty()) continue;
            for (int j = 0; j < kMaxSchedParams; j++) {
                if (conf.schedParam[i].Name[j].empty()) continue;
                FastSnprintf(path, sizeof(path), SchedParamPath, Policy::CpuPolicy[i], Performances::CpuGovernor[i].c_str(), conf.schedParam[i].Name[j].c_str());
                // 阻塞写：governor 类节点用 O_NONBLOCK 会 EAGAIN 静默失败(同 scaling_governor)
                utils.FileWriteIfChangedBlocking(path, conf.schedParam[i].Value[j]);
                logger.Debug("CPU簇: %d 调速器参数: %s 值: %s", Policy::CpuPolicy[i], conf.schedParam[i].Name[j].c_str(), conf.schedParam[i].Value[j].c_str());
            }
        }
    }

    void applyAllConfig() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        // 配置变更(改频率/换模式/恢复默认)必清去重缓存: 否则若 sysfs 已被外部(如提频)抬高、
        // 而缓存仍是旧低值, 恢复同值会被判"未变"跳过 → 高频留住降不回 ("恢复配置也不生效")。
        invalidateFreqCache();
        applyCurrentMode();
        SchedParam();
        online();
        function.gpuFreqControl();
        sysTweaks_.applyProfileTune(conf.mode);   // 每档 VM/网络/WiFi(ASB)
        applyTouchBoost();                        // 触控加速(内核 input boost)按 config 开/关

        if (!Config::AppProfile::enable) return;
        // 保活中且游戏进程仍在 → 重应用游戏画像(用新配置), 不因重载切到当前前台:
        // 防 Scene/编辑配置触发的重载在开着小窗时把游戏档丢掉。否则清保活按当前前台重评。
        if (!heldPkg.empty() && utils.isPackageRunning(heldPkg.c_str())) {
            int m = Config::AppProfile::findMatchingModel(heldPkg.c_str());
            if (m >= 0) {
                Config::AppProfile::currentMatch.store(m);
                applyWithProfile();
                // config 重载后 applyWithProfile 用画像 governor(sugov_ext) 覆盖了 scx,
                // 必须重切游戏态调速器, 否则游戏在前台但跑在 sugov_ext 下
                applyGamingGov(strcmp(Config::AppProfile::Models[m].modelName.c_str(),
                                      "game_heavy") == 0, "配置重载(保活中重应用画像)");
                return;
            }
        }
        heldPkg.clear();
        heldMissStreak_ = 0;   // 换持有对象: 清历史未命中计数, 不让旧包的计数影响新包
        forceReevalForeground();
    }

    // perapp 改变后包名不变但画像可能变，必须强制重算（绕过 pkg==lastTopApp 去重）
    void onPerappChanged() {
        // 持 applyMtx 改写全局画像表，与 applyAppProfile/applyBaseMode 的并发读互斥
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        conf.loadPerApp();
        logger.Info("perapp_powermode.txt 已重载 (%d 条映射), 重算当前前台画像",
                    Config::AppProfile::perAppCount);
        invalidateFreqCache(); // sysfs 可能被外部改过, 同值切档不能被去重跳过
        heldPkg.clear();       // 保活包的映射可能已变, 重评后按需重建
        forceReevalForeground();
    }

    void forceReevalForeground() {
        // lastTopApp 与前台线程共享, 必须持 applyMtx(可重入, 调用方已持锁也安全)
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        lastTopApp.clear();
        evalForegroundOnce(true);
    }

    // 返回文件内容哈希，用于去重：外部轮询可能反复回写相同内容触发 IN_CLOSE_WRITE，内容未变则跳过重载
    size_t fileContentHash(const char* path) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        std::string data;
        char tmp[4096];
        ssize_t n;
        while ((n = read(fd, tmp, sizeof(tmp))) > 0) data.append(tmp, (size_t)n);
        close(fd);
        if (data.empty()) return 0;
        return std::hash<std::string>{}(data);
    }

    void onModeChanged(size_t& lastHash) {
        size_t h = fileContentHash(configPath);
        if (h == 0) h = fileContentHash(storageConfigPath);
        if (h != 0 && h == lastHash) {
            logger.Debug("mode.txt 内容未变,跳过重载");
            return;
        }
        // readConfig 改写 Performances/AppProfile 等全局 string，必须与画像应用互斥
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        if (conf.readConfig()) {
            applyAllConfig();
            Config::GameList::loadUserList();   // 用户可能刚改了 gamelist.txt
            configGeneration_.fetch_add(1, std::memory_order_release);
            wakeFsm();
            lastHash = h;
        } else {
            logger.Warn("配置重载失败，跳过应用");
        }
    }

    void onJsonChanged(size_t& lastHash) {
        // 联合哈希: config.json(出厂层) + settings.json(App 开关覆盖层)任一变化都要重载。
        // 与 JsonConfig 的读取路径一致：/sdcard 不可见时使用真实共享存储路径，
        // 否则文件虽可被读取，热重载却会因哈希为 0 而静默失效。
        size_t configHash = fileContentHash(jsonPath);
        if (configHash == 0) configHash = fileContentHash("/storage/emulated/0/Android/Kin/config.json");
        size_t settingsHash = fileContentHash(settingsJsonPath);
        if (settingsHash == 0) settingsHash = fileContentHash("/storage/emulated/0/Android/Kin/settings.json");
        size_t h = configHash ^ (settingsHash * 31);
        if (h != 0 && h == lastHash) {
            logger.Debug("config/settings 内容未变,跳过重载");
            return;
        }
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        if (conf.readConfig()) {
            function.ReloadFunC();
            applyAllConfig();
            configGeneration_.fetch_add(1, std::memory_order_release);
            wakeFsm();
            lastHash = h;
        } else {
            logger.Warn("JSON 配置重载失败，跳过应用");
        }
    }

    // 单 inotify 实例 + 阻塞 read 监控目录下三个配置文件（合并原三线程，消除 5s select 定时唤醒）
    // /sdcard/ 是 FUSE，inotify 不能监控文件，只能监控目录再过滤文件名
    void fileWatchTask() {
        if (waitStop(2000)) return;
        const char* watchDir = "/sdcard/Android/Kin";
        size_t lastModeHash = 0, lastJsonHash = 0;
        while (!shouldExit_.load()) {
            int fd = inotify_init1(IN_CLOEXEC);
            if (fd < 0) {
                logger.Error("inotify_init 失败: %s", strerror(errno));
                if (waitStop(5000)) break;
                continue;
            }
            int wd = inotify_add_watch(fd, watchDir, IN_CLOSE_WRITE | IN_MOVED_TO);
            const int wdStorage = inotify_add_watch(fd, "/storage/emulated/0/Android/Kin",
                                                     IN_CLOSE_WRITE | IN_MOVED_TO);
            if (wd < 0 && wdStorage < 0) {
                logger.Error("inotify_add_watch 失败 %s 和 /storage/emulated/0/Android/Kin: %s",
                             watchDir, strerror(errno));
                close(fd);
                if (waitStop(5000)) break;
                continue;
            }
            // perapp_powermode.txt 已移到 AI/ 子目录, 追加监听它(事件仍按文件名路由)。
            // 先确保目录存在, 否则 add_watch 失败会导致热重载失灵。
            mkdir("/sdcard/Android/Kin/AI", 0755);
            int wdAi = inotify_add_watch(fd, "/sdcard/Android/Kin/AI", IN_CLOSE_WRITE | IN_MOVED_TO);
            loadAppLabels();   // 启动即加载一次 App 名字映射(之后靠 inotify 热重载)

            bool alive = true;
            alignas(struct inotify_event) char buf[4096];
            while (alive && !shouldExit_.load()) {
                // poll 同时等 inotify 与停止 eventfd，析构写 eventfd 即可打断阻塞
                struct pollfd pfds[2] = {
                    { fd,           POLLIN, 0 },
                    { stopEventFd_, POLLIN, 0 },
                };
                nfds_t nfds = (stopEventFd_ >= 0) ? 2 : 1;
                int pr = poll(pfds, nfds, (stopEventFd_ >= 0) ? -1 : 1000);
                if (pr < 0) {
                    if (errno == EINTR) continue;
                    alive = false;
                    break;
                }
                if (pr == 0) continue;  // 无 eventfd 时的兜底超时，回头查停止标志
                if (nfds == 2 && (pfds[1].revents & POLLIN)) break;  // 停止请求
                if (!(pfds[0].revents & POLLIN)) {
                    if (pfds[0].revents) alive = false;  // 错误/挂起，重建 inotify
                    continue;
                }
                ssize_t len = read(fd, buf, sizeof(buf));
                if (len <= 0) {
                    if (len < 0 && errno == EINTR) continue;
                    alive = false;
                    break;
                }
                for (char* ptr = buf; ptr < buf + len; ) {
                    auto* ev = reinterpret_cast<struct inotify_event*>(ptr);
                    ptr += sizeof(struct inotify_event) + ev->len;
                    if (ev->mask & (IN_IGNORED | IN_UNMOUNT)) { alive = false; continue; } // watch 失效，重建
                    if (ev->len == 0) continue;
                    if      (strcmp(ev->name, "mode.txt") == 0)             onModeChanged(lastModeHash);
                    else if (strcmp(ev->name, "config.json") == 0)          onJsonChanged(lastJsonHash);
                    else if (strcmp(ev->name, "settings.json") == 0)        onJsonChanged(lastJsonHash);
                    else if (strcmp(ev->name, "perapp_powermode.txt") == 0) onPerappChanged();
                    else if (strcmp(ev->name, "topapp.txt") == 0)           onTopAppChanged();
                    else if (strcmp(ev->name, "pkg_label.txt") == 0)        loadAppLabels();
                }
            }
            if (wd >= 0) inotify_rm_watch(fd, wd);
            if (wdStorage >= 0) inotify_rm_watch(fd, wdStorage);
            if (wdAi >= 0) inotify_rm_watch(fd, wdAi);
            close(fd);
        }
    }

    // 阻塞等待 top-app cpuset 变动事件（前台切换会改写该 cgroup），超时返回 0，inotify 失败返回 -1
    int waitTopAppEvent(int inotifyFd, int wd, int maxWaitMs) {
        (void)wd;
        if (inotifyFd < 0) return -1;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(inotifyFd, &rfds);
        int maxFd = inotifyFd;
        // 一并监听停止 eventfd，析构即时打断等待（不消费，外层查 shouldExit_ 退出）
        if (stopEventFd_ >= 0) {
            FD_SET(stopEventFd_, &rfds);
            if (stopEventFd_ > maxFd) maxFd = stopEventFd_;
        }
        struct timeval tv;
        tv.tv_sec  = maxWaitMs / 1000;
        tv.tv_usec = (maxWaitMs % 1000) * 1000;
        int ret = select(maxFd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0) return (errno == EINTR) ? 0 : -1;
        if (ret == 0) return 0;
        if (stopEventFd_ >= 0 && FD_ISSET(stopEventFd_, &rfds)) return 0;
        char buf[4096];
        ssize_t n = read(inotifyFd, buf, sizeof(buf));
        (void)n;
        return 1;
    }


    // 守护进程绑定低位核 0-3，避免抢占游戏超大核；主线程设亲和性后新线程继承（参考 uperf 最佳实践）
    void pinSelfToLittleCores() {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c = 0; c <= 3; c++) CPU_SET(c, &set);
        if (sched_setaffinity(0, sizeof(set), &set) == 0)
            logger.Info("守护进程已绑定低位核 0-3(避开超大核 6-7), 不抢占前台游戏");
        else
            logger.Debug("绑定低位核失败 errno=%d(忽略, 不影响功能)", errno);
        // 后台优先级：自身工作均为微秒级 sysfs 写，nice 10 不影响生效时机但不与前台争时间片
        setpriority(PRIO_PROCESS, 0, 10);
    }



    bool readNodeTrim(const char* p, char* out, size_t cap) {
        out[0] = '\0';
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        ssize_t n = read(fd, out, cap - 1);
        close(fd);
        if (n <= 0) { out[0] = '\0'; return false; }
        out[n] = '\0';
        for (ssize_t i = n - 1; i >= 0 &&
             (out[i]=='\n'||out[i]=='\r'||out[i]==' '||out[i]=='\t'); --i) out[i] = '\0';
        return true;
    }

    // 游戏档下 ORMS 常把 top-app cpuset 收窄（踢掉 6-7），事件驱动+幂等+节流矫正回全核
    // 实时回读各簇 governor/min/max, 与上次写入值(配置目标)不符即清缓存强制重应用,
    // 对抗 ROM/perf HAL 运行时改写。返回是否检测到漂移并已写回。
    bool enforceState() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        // auto+FSM 接管中且无 App 画像: FSM 每 2s 自重写频率界, 此处不再纠偏(否则会写静态基础档与 FSM 打架)。
        if (autoActive_.load() &&
            Config::AppProfile::currentMatch.load() < 0) {
            return false;
        }
        char path[256], cur[64];
        bool drift = false;
        for (int i = 0; i < kClusterCount && !drift; i++) {
            int p = Config::Policy::CpuPolicy[i];
            if (p == -1) continue;
            if (!lastGovernor[i].empty()) {
                FastSnprintf(path, sizeof(path), GovernorPath, p);
                if (readNodeTrim(path, cur, sizeof(cur)) && strcmp(cur, lastGovernor[i].c_str())) drift = true;
            }
            if (!drift && !lastMinFreq[i].empty()) {
                FastSnprintf(path, sizeof(path), MinFreqPath, p);
                if (readNodeTrim(path, cur, sizeof(cur)) && strcmp(cur, lastMinFreq[i].c_str())) drift = true;
            }
            if (!drift && !lastMaxFreq[i].empty()) {
                FastSnprintf(path, sizeof(path), MaxFreqPath, p);
                if (readNodeTrim(path, cur, sizeof(cur)) && strcmp(cur, lastMaxFreq[i].c_str())) drift = true;
            }
        }
        if (drift) {
            logger.Debug("CPU 状态被外部改写 → 强制写回配置");
            invalidateFreqCache();
            applyWithProfile();
            // applyWithProfile 用画像 governor(sugov_ext) 覆盖了 scx,
            // 若游戏态调速器在位必须立即重切, 否则 ColorOS 改写后 scx 永久丢失
            if (gamingGovActive_) {
                applyGamingGov(true);
            }
        }
        return drift;
    }

    // 游戏档下把"参考簇"(policy0, ROM 实际写入的风驰)的调速器复制到配置留空的其它在线簇。
    // 根因: 省电关大核 → 进游戏大核迟上线, 错过 ROM 一次性风驰, 以默认 walt 上线; 留空时 Kin 不写。
    // 周期性(每 enforce tick)读参考簇 → 镜像到留空簇, 无竞态, 纯跟随 ROM 实际选择(不写死 hmbird/scx)。
    // 仅作用于 isGame 且该簇配置 governor 为空; 显式配置的簇不动。返回是否写入。
    bool propagateGameGovernor() {
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        if (gamingGovActive_) return false;   // ROM/OIface 放权窗口内 Kin 绝不写 scaling_governor
        int cur = Config::AppProfile::currentMatch.load();
        if (cur < 0 || cur >= Config::AppProfile::modelCount) return false;
        const auto& m = Config::AppProfile::Models[cur];
        if (!m.isGame) return false;
        int refP = Config::Policy::CpuPolicy[0];
        if (refP < 0) return false;
        char ref[64], path[256];
        FastSnprintf(path, sizeof(path), GovernorPath, refP);
        if (!readNodeTrim(path, ref, sizeof(ref)) || !ref[0]) return false;
        bool wrote = false;
        for (int i = 1; i < Config::kClusterCount; i++) {
            int p = Config::Policy::CpuPolicy[i];
            if (p == -1) continue;
            if (!m.Governor[i].empty()) continue;   // 配置显式指定 → 交给常规写入, 不镜像
            char aff[256];
            FastSnprintf(aff, sizeof(aff),
                         "/sys/devices/system/cpu/cpufreq/policy%d/affected_cpus", p);
            if (!utils.FileStartsWithDigit(aff)) continue;   // 离线簇跳过
            char cur2[64];
            FastSnprintf(path, sizeof(path), GovernorPath, p);
            if (readNodeTrim(path, cur2, sizeof(cur2)) && strcmp(cur2, ref) != 0) {
                if (utils.FileWriteIfChangedBlocking(path, string_t(ref))) {
                    logger.Info("游戏档: 簇%d 调速器跟随参考簇 → %s", p, ref);
                    wrote = true;
                }
            }
        }
        return wrote;
    }

    // 实时监测线程: 周期回读纠正; 稳定久了退避省电, 一旦被改写立即回 2s 紧盯
    void cpuEnforceTask() {
        if (waitStop(6000)) return;   // 等启动稳定
        int interval = 2000, stable = 0;
        while (!shouldExit_.load()) {
            {
                std::lock_guard<std::recursive_mutex> lk(applyMtx);
                sysTweaks_.retryRestores();
            }
            // 息屏期间无前台切换/调度变化需要强制, 退避到 30s 省过夜累积唤醒
            // (autoModeTask 自身息屏即用 60s, 30s 仍更勤)。-1(未知)按亮屏处理, 不退避;
            // 亮屏后下一轮 stable/interval 复位, 立即恢复 2s 快节奏先做一次强制。
            if (autoMode_.screenState() == 0) {
                stable = 0; interval = 2000;
                if (waitStop(30000)) break;
                continue;
            }
            bool changed = enforceState();
            if (propagateGameGovernor()) changed = true;   // 游戏档留空簇跟随 ROM 风驰
            // 游戏态周期性矫正 cpuset (与调速器检测同频, 适配天玑/骁龙)。
            // 矫正命中也算 changed: cpuset 正被外部反复收窄时应保持 2s 紧盯, 不退避到 5s。
            if (correctGameTopAppCpus()) changed = true;
            if (changed) { interval = 2000; stable = 0; }
            else if (++stable >= 5) interval = 5000;
            if (waitStop(interval)) break;
        }
    }

    // 返回是否真的下发了一次矫正(供调用方判断"状态仍在被外部改写", 不要退避巡检间隔)
    bool correctGameTopAppCpus() {
        // 读 Models/Cpuset 全局 string，与配置重载互斥
        std::lock_guard<std::recursive_mutex> lk(applyMtx);
        // 游戏态判定: 手动模式看画像匹配, 自动模式看调速器是否在位 (覆盖手动+智能双模)
        int cur = Config::AppProfile::currentMatch.load();
        bool inGame = (cur >= 0 && cur < Config::AppProfile::modelCount
                       && Config::AppProfile::Models[cur].isGame);
        if (!inGame && !gamingGovActive_ && !gamingGovEngaged_) return false;

        // 目标全核范围: 优先用配置的 top_app(游戏档应为 0-7), 否则用当前全部在线 CPU。
        char desired[64] = { 0 };
        if (Config::Cpuset::enable && !Config::Cpuset::top_app.empty())
            snprintf(desired, sizeof(desired), "%s", Config::Cpuset::top_app.c_str());
        else if (!readNodeTrim("/sys/devices/system/cpu/online", desired, sizeof(desired)))
            return false;
        if (!desired[0]) return false;

        // 【必须校验读取成功】否则 now 恒为空串 → strcmp 永不相等 → 每轮巡检都误判"被收窄",
        // 无休止重写+刷日志且永不收敛(节点不存在/cgroup v2 机型/SELinux 拦读时就会这样)。
        char now[64] = { 0 };
        if (!readNodeTrim("/dev/cpuset/top-app/cpus", now, sizeof(now)) || !now[0]) {
            logger.Debug("游戏档: top-app cpuset 节点读不到, 跳过矫正");
            return false;
        }
        // 已是全核范围 → 不矫正 (适配天玑/骁龙, 不在代码里写死 0-7)
        if (strcmp(now, desired) == 0) return false;

        constexpr long long kFixMinMs = 1000;    // 节流：最多每秒矫正一次，防 ping-pong
        long long t = nowMs();
        if (t - lastGameCpusetFixMs < kFixMinMs) return false;
        lastGameCpusetFixMs = t;

        // cpuset 子集 ⊆ 父集：先放开 root，再写 top-app/foreground
        utils.FileWriteIfChangedBlocking("/dev/cpuset/cpus", desired);
        utils.FileWriteIfChangedBlocking("/dev/cpuset/top-app/cpus", desired);
        utils.FileWriteIfChangedBlocking("/dev/cpuset/foreground/cpus", desired);
        logger.Debug("游戏档: top-app cpuset 被收窄(%s) → 矫正回 %s", now, desired);
        return true;
    }

    std::string getForegroundPkg(bool fresh, const char** source = nullptr) {
        auto setSource = [source](const char* value) { if (source) *source = value; };
        setSource("unknown");
        auto blacklisted = [this](const std::string& pkg) {
            std::lock_guard<std::recursive_mutex> lock(applyMtx);
            return Config::AppProfile::isBlacklisted(pkg.c_str());
        };
        // ── App 无障碍推送 = 绝对最高优先级 ──
        // 只要推送新鲜(未超 TTL; App 心跳每 ~10s 重推保鲜)就【无条件信任】, 绝不用 top-app/cpuset
        // 去二次校验。原因: 开小窗/自由窗口时 top-app cpuset 会切成小窗应用的包名, 拿它核对会把
        // 正确的主应用推送误判成"陈旧"丢掉 —— 正是本次要修的 bug。App 侧已按窗口面积选出主应用
        // (过滤小窗/输入法), 推送的就是当前主应用, 守护只管用。任何本地检测(cpuset/mFocusedApp/
        // dumpsys)优先级都低于无障碍推送, 仅在【完全没有新鲜推送】(无障碍未开/被杀)时才兜底。
        bool havePush = false;
        std::string pushed;
        {
            std::lock_guard<std::mutex> lk(pushMtx_);
            if (!pushedTopApp_.empty() && nowMs() - lastPushMs_.load() < kPushTtlMs) {
                havePush = true;
                pushed = pushedTopApp_;
                // 推送是黑名单(桌面/输入法作为主应用)= 主屏/无 App 前台 → pushed 留空, 按 idle 处理,
                // 【不回退 cpuset】(否则又会捞到小窗/后台残留)。有推送就以推送为准, 哪怕它说"在桌面"。
            }
        }
        if (havePush) {
            if (blacklisted(pushed)) pushed.clear();
            if (!pushed.empty()) {
                std::lock_guard<std::mutex> lk(fgMtx_); lastGoodFgPkg_ = pushed;
                setSource("accessibility");
            } else {
                setSource("accessibility_blacklist");
            }
            return pushed;   // 可能为空(桌面/黑名单主应用) → 调用方按 类=-1/idle 处理
        }
        // ── 以下均为【无新鲜推送】的兜底(无障碍未开/被杀) ──
        // auto/FSM: 前台仅用于 gameHint/类别, 用 cpuset 直读(/dev/cpuset/top-app +
        // /proc/pid/cmdline, 无 popen/dumpsys)即可, 不走下面的重 dumpsys 路径 —— 省开销。
        if (autoActive_.load()) {
            std::string fast = utils.getTopAppFast();
            // 顶层取空/null(cpuset 偶发读空/新 ROM 不投放)、或黑名单(输入法/launcher/systemui)
            // → 不报它, 回退到上个非黑名单真实前台(进程仍在才用), 防 类=-1 全核回弹。
            const bool bad = fast.empty() || fast == "null" ||
                             blacklisted(fast);
            if (bad) {
                // 焦点兜底(fas-rs 同款 mFocusedApp, AOSP 标准, 跨 ROM): 推送缺席(无障碍被杀/
                // 未开)且 cpuset 也给不出答案时, 问窗口管理器"焦点是谁"。≥5s 硬限频,
                // 无障碍正常工作时这里永远不会走到 —— 不增加日常开销。
                const long long fnow = nowMs();
                long long lastP = lastFocusProbeMs_.load(std::memory_order_relaxed);
                if (fnow - lastP >= 5000 &&
                    lastFocusProbeMs_.compare_exchange_strong(lastP, fnow, std::memory_order_relaxed)) {
                    std::string focus = utils.getFocusedAppWindow();
                    if (!focus.empty() && focus != "null" &&
                        !blacklisted(focus)) {
                        std::lock_guard<std::mutex> lk(fgMtx_);
                        lastGoodFgPkg_ = focus;
                        setSource("window_focus");
                        return focus;
                    }
                }
                std::string cand;
                { std::lock_guard<std::mutex> lk(fgMtx_); cand = lastGoodFgPkg_; }
                // /proc 全表扫描限频；焦点兜底的失败路径不能每个 FSM tick 重扫。
                if (lastGoodFgPresence_.alive(cand, 2000, [this](const std::string& pkg) {
                    return utils.isPackageRunning(pkg.c_str());
                })) { setSource("last_good_alive"); return cand; }
                setSource(fast.empty() ? "unknown" : "top_app_cpuset");
                return fast;   // 无可用回退 → 照旧(后续按 -1/黑名单处理)
            }
            { std::lock_guard<std::mutex> lk(fgMtx_); lastGoodFgPkg_ = fast; }
            setSource("top_app_cpuset");
            return fast;
        }
        // ② dumpsys 黑名单感知(手动档)
        Utils::ForegroundSnapshot s = utils.getForegroundCached(fresh ? 0 : 2500);
        if (s.valid) {
            for (const auto& p : s.standardVisible)
                if (!blacklisted(p)) {
                    setSource("activity_task");
                    return p;
                }
            if (!s.topPkg.empty()) {
                setSource("activity_task_blacklist");
                return s.topPkg;   // 全是黑名单 → 退回最顶层
            }
        }
        setSource("fallback_chain");
        // 即使是事件驱动的 fresh 路径也不能绕过 dumpsys 节流；无障碍推送和
        // cpuset/focus 兜底已优先返回，真正走到这里时使用短缓存即可。
        return utils.getTopAppCached(fresh ? 1500 : 2500);
    }

    // App 推送 topapp.txt 变更: 记录包名+时刻, 立即重评前台(走 getForegroundPkg 会用到推送值)
    void onTopAppChanged() {
        char buf[256];
        if (!readNodeTrim(topAppPath, buf, sizeof(buf)) || !buf[0]) return;
        if (!kin::validPackageName(buf)) return;
        bool changed;
        {
            std::lock_guard<std::mutex> lk(pushMtx_);
            lastPushMs_.store(nowMs());          // 先保鲜后改值, 都在锁内(防"新包名+旧时戳"错配)
            changed = (pushedTopApp_ != buf);
            if (changed) pushedTopApp_ = buf;
        }
        // 心跳同值重推: 只保鲜不唤醒 —— 否则亮屏空闲期每 10s 击穿一次 LIGHT_IDLE 慢拍(白耗电)。
        if (!changed) return;
        // 智能/FSM 独占模式: 前台变化 → 立即唤醒 FSM tick 重采样(秒进/出游戏态, 不等 2s)。
        // evalForegroundOnce 在 auto 下本就 early-return, 改走唤醒更直接。
        if (autoActive_.load()) {
            // 息屏: 前台与调度无关(FSM 息屏恒 DEEP_IDLE), 推送值已存好, 但【不叫醒】FSM —— 省息屏唤醒。
            // (即便 App 端漏关息屏心跳推送, daemon 这层也不会被每 10s 的重推拖醒。)
            // 亮屏后下一次推送(此时 screenState()!=0)即叫醒 FSM, 不影响解锁响应。
            if (autoMode_.screenState() == 0) return;
            wakeFsm();
            return;
        }
        // 手动模式: 即时重评前台画像。
        evalForegroundOnce(true);
    }

    // App 无障碍服务是否正在提供新鲜的前台推送(此时检测准确, 不需保活兜底)
    bool appPushActive() {
        std::lock_guard<std::mutex> lk(pushMtx_);
        return !pushedTopApp_.empty() && nowMs() - lastPushMs_.load() < kPushTtlMs;
    }

    // fresh=true: 事件驱动重评，推送值即时生效；无推送时 Activity 探测仍受 2s 硬节流。
    // fresh=false: 允许用较长缓存（空闲复查/低频路径，省电）。
    void evalForegroundOnce(bool fresh) {
        // 串行化"取前台 + 改 lastTopApp/currentMatch + 应用"，防两线程并发写 sysfs
        std::lock_guard<std::recursive_mutex> lk(applyMtx);

        // pushMode = App 无障碍正在推送前台(检测准)。仅用于下方日志措辞;保活两种模式都启用。
        const bool pushMode = appPushActive();

        // 保活: 检测到游戏/keep_alive 画像后, 只要其进程存活就一直维持该画像 ——
        // 小窗/分屏/输入法/通知栏/回桌面/切到别的(非保活)App/打开本 App 都不掉档,
        // 直到游戏进程真正退出才解除。 (App 推送模式同样生效: 小窗会被报成前台, 不能据此掉档。)
        // auto(智能/FSM 独占)无视黑名单/画像/保活: 前台处理整段不跑(CPU/GPU 全程由 FSM 接管,
        // launcher/systemui 等低负载自然给低频), 仅清掉残留保活后直接返回, 杜绝与 FSM 抢写。
        // gameHint 由 autoModeTask 自行采样前台判定, 不依赖此处。
        if (autoActive_.load()) {
            heldPkg.clear();
            return;
        }
        if (!heldPkg.empty()) {
            if (utils.isPackageRunning(heldPkg.c_str())) {
                heldMissStreak_ = 0;  // 进程在 → 清空连续未命中计数
                if (!fresh) return;   // 空闲复查: 进程在就维持, 不取前台
                std::string cur = getForegroundPkg(true);
                if (cur.empty() || cur == "null" || cur == heldPkg) return;   // 还在游戏/取不到 → 维持
                int m = Config::AppProfile::isBlacklisted(cur.c_str())
                          ? -1 : Config::AppProfile::findMatchingModel(cur.c_str());
                // 切到普通 App / 黑名单(launcher/systemui/本App)但游戏仍在 → 维持游戏档, 不掉
                if (m < 0 || !(Config::AppProfile::Models[m].isGame ||
                               Config::AppProfile::Models[m].keepAlive)) return;
                heldPkg.clear();      // 切到"另一个"保活类 App → 移交(走下方重建)
                lastTopApp.clear();
            } else {
                // /proc 瞬断去抖: 原先只等 50ms 再查一次就定生死, 但应用切换/系统卡顿期的扫描失败
                // 可持续数秒, 而一次误判的代价是完整掉档(清 currentMatch + 落基础档 + 还原调速器)再重建。
                // 【不能在此原地长睡补偿】: 本函数全程持 applyMtx, 长睡会阻塞 cpuEnforceTask/配置重载。
                // 故保留原 50ms 短重查(锁持有时长不变), 改用【跨轮连续未命中】兜底:
                // 只有连续 kHeldMissMax 轮巡检都查不到进程才真解除, 单次瞬断不再掉档。
                usleep(50000);
                if (utils.isPackageRunning(heldPkg.c_str())) { heldMissStreak_ = 0; return; }
                if (++heldMissStreak_ < kHeldMissMax) {
                    logger.Debug("保活: %s 进程查不到(%d/%d), 暂不解除(疑似 /proc 瞬断)",
                                 labelForLog(heldPkg).c_str(), heldMissStreak_, kHeldMissMax);
                    return;
                }
                heldMissStreak_ = 0;
                logger.Info("保活: %s 已退出 → 解除", labelForLog(heldPkg).c_str());  // 退出打印一次
                heldPkg.clear();
                Config::AppProfile::currentMatch.store(-1);
                lastTopApp.clear();
                applyWithProfile();   // 立即落基础, 防去重跳过致高频残留
                applyGamingGov(false, "保活解除/游戏进程退出");   // 游戏退出: 风驰调速器条件还原(下方取不到新前台时也不悬空)
            }
        }

        std::string pkg = getForegroundPkg(fresh);
        if (pkg.empty() || pkg == "null") return;

        if (pkg == lastTopApp) return;
        lastTopApp = pkg;

        // 黑名单前台(launcher/systemui/输入法等)或无匹配画像 → matched=-1
        const auto decision = Config::AppProfile::decideForeground(pkg.c_str(), autoActive_.load());
        const int matched = decision.matched;

        // [真黑名单·方案A] 黑名单 App 前台 → 彻底松手交还系统(解锁频率+默认调速器+全核),不套任何档位。
        // 普通无匹配 App(非黑名单)仍走下方"跟随全局"。
        // 智能模式无视黑名单: FSM 全程接管(launcher/systemui 等低负载自然给低频), 不释放, 否则与 FSM 抢。
        if (decision.release) {
            if (!blacklistReleased) {
                blacklistReleased = true;
                Config::AppProfile::currentMatch.store(-1);
                logger.Info("前台: %s → 释放接管(黑名单, 交还系统默认)", labelForLog(pkg).c_str());
                applyGamingGov(false, "黑名单前台/释放接管");   // 交还系统前先条件还原风驰调速器(releaseControl 会写默认 governor)
                releaseControl();
            }
            return;
        }
        blacklistReleased = false;

        // 建立保活须在去重之外: 重载清掉 heldPkg 后 match 可能不变(被去重跳过), 仍要恢复持有。
        // 两种检测模式都建立(匹配到游戏/保活画像即持有); auto 下 matched 已为 -1, 不会建立。
        if (matched >= 0) {
            const auto& mdl = Config::AppProfile::Models[matched];
            if (mdl.isGame || mdl.keepAlive) {
                if (heldPkg != pkg) heldMissStreak_ = 0;   // 换保活目标: 清掉上一个包的未命中计数
                heldPkg = pkg;
            }
        }

        // 无匹配/黑名单 → 跟随全局:优先用 perapp "*" 行(全局默认),没有再回落基础档。
        // 不再强制省电;若用户想让无匹配/黑名单应用省电,设 perapp 全局行 "* powersave" 即可。
        const int newMatch = decision.selected;

        // 日志: 前台每变一次都打一行"应用 → 当前生效的档"(画像/全局/基础都打一次), 让用户看清切到哪个模式;
        // 到这里 pkg 必已变化(上方 pkg==lastTopApp 已早退)。实际写 sysfs 仍按去重(档没变不重复下发)。
        int oldMatch = Config::AppProfile::currentMatch.load();
        if (matched >= 0) {
            const auto& mdl = Config::AppProfile::Models[matched];
            logger.Info("前台: %s → 画像档[%s]%s", labelForLog(pkg).c_str(), mdl.modelName.c_str(),
                        mdl.isGame ? (pushMode ? " (游戏)" : " (游戏, 保活至退出)") : "");
        } else if (newMatch >= 0) {
            logger.Info("前台: %s → 全局档[%s]", labelForLog(pkg).c_str(),
                        Config::AppProfile::Models[newMatch].modelName.c_str());
        } else {
            logger.Info("前台: %s → 基础档[%s]", labelForLog(pkg).c_str(), conf.mode.c_str());
        }
        if (newMatch != oldMatch) {
            Config::AppProfile::currentMatch.store(newMatch);
            applyWithProfile();
        }
        // ── 风驰 governor 放权(gaming_gov): 画像模式命中 game_heavy 时拉起 OIface，
        // 不指定/还原 scaling_governor，由 ROM/风驰选择；其它前台恢复 Kin 的普通 governor 管理。 ──
        const bool romFengchi = matched >= 0 &&
            strcmp(Config::AppProfile::Models[matched].modelName.c_str(), "game_heavy") == 0 &&
            !Config::AutoMode::gaming_gov.empty();
        if (romFengchi) ensureOifaceAlive();
        applyGamingGov(romFengchi);
    }

    void appProfileTask() {
        if (waitStop(5000)) return; // 等待系统启动完成

        // 事件驱动+尾沿防抖：监听 top-app cpuset 变动替代轮询，重操作等前台"安静"kQuietMs 后一次写入
        // 快速连切时持续顺延，不写入风暴；inotify 不可用时退化为轮询
        constexpr int kPollIdleMs  = 4000;  // 回退轮询 / inotify 空闲复查间隔
        constexpr int kQuietMs     = 300;   // 安静窗口：无新切换事件持续此时长视为已落定
        constexpr int kMaxDeferMs  = 1200;  // 硬上限：持续事件也保证最终生效，不无限顺延

        int fd = inotify_init1(IN_CLOEXEC);
        int wd = -1;
        if (fd >= 0) {
            // 排除 IN_ACCESS：读 top-app/cpus 会触发 IN_ACCESS 造成自诱发 busy-loop
            wd = inotify_add_watch(fd, cpusetEventPath,
                                   IN_MODIFY | IN_ATTRIB | IN_MOVED_TO | IN_CREATE);
            if (wd < 0) { close(fd); fd = -1; }
        }
        logger.Debug(fd >= 0
            ? "场景画像监控线程已启动 (事件驱动: 监听 top-app 切换, 支持通配符匹配)"
            : "场景画像监控线程已启动 (inotify 不可用, 回退轮询; 支持通配符匹配)");

        evalForegroundOnce(true);

        while (!shouldExit_.load()) {
            // auto/FSM 独占: 前台变化只用于"叫醒 FSM 重采样", 故空闲等待放长到 60s(省唤醒),
            // 真正的 top-app 切换由 inotify 即时返回, 不受此超时影响。手动档维持 4s 复查。
            const bool autoFsm = autoActive_.load();
            int ev = waitTopAppEvent(fd, wd, autoFsm ? 60000 : kPollIdleMs);
            if (shouldExit_.load()) break;
            if (ev < 0) {
                if (waitStop(kPollIdleMs)) break;
                if (autoFsm) { wakeFsm(); continue; }   // inotify 不可用回退: 周期叫醒 FSM
                evalForegroundOnce(true);
                correctGameTopAppCpus();
                continue;
            }
            if (ev == 0) {
                if (autoFsm) continue;                  // auto: 无前台变化 → 不唤醒(省电)
                evalForegroundOnce(false);
                correctGameTopAppCpus();
                continue;
            }

            // auto: 内核确认 top-app 组变了 → 【先即时唤醒一拍】(切换要快),
            // 再进尾沿防抖吸收事件风暴, 结束后补唤醒收尾。(前台以无障碍推送为准, 不再拿 cpuset 校验推送)
            if (autoActive_.load()) {
                wakeFsm();
            }
            // 尾沿防抖：等前台安静再做重操作，避免连切时频率写风暴
            int deferred = 0;
            while (deferred < kMaxDeferMs) {
                int more = waitTopAppEvent(fd, wd, kQuietMs);
                if (more <= 0) break;
                deferred += kQuietMs;
            }
            if (shouldExit_.load()) break;
            if (autoActive_.load()) {
                wakeFsm();
                continue;
            }
            evalForegroundOnce(true);
            correctGameTopAppCpus();
        }

        if (fd >= 0) {
            if (wd >= 0) inotify_rm_watch(fd, wd);
            close(fd);
        }
    }

    void Init() {
        const int myPid = getpid();
        const char* checkNames[] = {"Kinetic", "CpuTurboScheduler", "MW_CpuSpeedController"};

        // Step 1: Graceful termination (SIGTERM) -- exclude current PID
        for (const char* name : checkNames) {
            char cmd[64];
            char buf[256] = { 0 };
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token != nullptr) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) {
                        kill(pid, SIGTERM);
                        logger.Debug("发送 SIGTERM 到 %s (pid: %d)", name, pid);
                    }
                    token = strtok(nullptr, " \t\n");
                }
            }
        }
        sleep(1); // Allow graceful shutdown

        // Step 2: Force kill (SIGKILL) -- exclude current PID
        for (const char* name : checkNames) {
            char cmd[64];
            char buf[256] = { 0 };
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token != nullptr) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) {
                        kill(pid, SIGKILL);
                        logger.Debug("发送 SIGKILL 到 %s (pid: %d)", name, pid);
                    }
                    token = strtok(nullptr, " \t\n");
                }
            }
        }
        sleep(1); // Allow force kill to take effect

        // Step 3: Final check -- if any other instance still running, exit
        for (const char* name : checkNames) {
            char cmd[64];
            char buf[256] = { 0 };
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token != nullptr) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) {
                        logger.Error("Kin调度已经在运行(%s pid: %d), 当前进程(pid:%d)即将退出", name, pid, myPid);
                        printf("\n!!! \n!!! Kin调度已经在运行(%s pid: %d), 当前进程(pid:%d)即将退出 \n!!!\n\n", name, pid, myPid);
                        exit(-1);
                    }
                    token = strtok(nullptr, " \t\n");
                }
            }
        }

        logger.clear_log();
        bool configOk = conf.readConfig();
        logger.Info("%s v%d · %s · 日志=%s · 构建 %s %s",
                    Meta::name.c_str(), Meta::version, Meta::author.c_str(),
                    Meta::loglevel.c_str(), __DATE__, __TIME__);
        function.AllFunC();
        if (configOk) {
            applyCurrentMode();
            online();
            SchedParam();
            function.gpuFreqControl();
            sysTweaks_.applyProfileTune(conf.mode);   // 每档 VM/网络/WiFi
        } else {
            logger.Warn("初始配置加载失败，尝试使用默认配置");
        }
        sysTweaks_.bootTrimOnce();   // 开机一次性: 停调试服务 + 关 WiFi 后台扫描(需 Function.BootTrim)
        applyTouchBoost();           // 触控加速(内核 input boost)按 config 开/关
    }
};
