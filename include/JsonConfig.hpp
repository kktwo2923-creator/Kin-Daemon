#pragma once

#include "Json/json.h"
#include "Config.hpp"
#include "ConfigValidation.hpp"
#include "Logger.hpp"
#include <stdexcept>
#include <cstring>

using namespace Config;
using namespace qlib;

class JsonConfig {
private:
    static constexpr const char* configPath = "/sdcard/Android/Kin/config.json";
    static constexpr const char* modePath   = "/sdcard/Android/Kin/mode.txt";
    // App 开关覆盖层: 与 config.json 同结构(只含 Function 子集)。config.json 提供出厂值,
    // settings.json 键存在即覆盖 —— 模块升级/恢复出厂只动 config.json, 用户设置不丢。
    static constexpr const char* settingsPath = "/sdcard/Android/Kin/settings.json";

    Logger logger;
    json_view_t json;
    json_view_t settingsJson;   // settings.json 解析文档(生命周期须覆盖 parse 后的读取)

    char buff[256];

public:
    SchedParam  schedParam[kClusterCount];
    std::string mode;
    int         baseModelIdx = -1;

    void LoadConfig() {
        const char* paths[] = {
            modePath,
            "/storage/emulated/0/Android/Kin/mode.txt"
        };
        ifstream file;
        const char* usedPath = modePath;
        for (const char* path : paths) {
            file.open(path);
            if (file.is_open()) { usedPath = path; break; }
            file.clear();
        }
        if (!file.is_open()) {
            // mode.txt 不存在时默认 balance，但保留明确诊断；service.sh 负责创建。
            fprintf(stderr, "无法打开配置文件: %s 或 %s，使用默认 balance\n",
                    paths[0], paths[1]);
            mode = "balance";
            return;
        }
        fprintf(stderr, "Kin mode.txt: %s\n", usedPath);
        std::string temp;
        getline(file, temp);
        size_t end = temp.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) temp.erase(end + 1);
        else                          temp.clear();
        mode = std::move(temp);
    }

    bool switchConfig() const {
        return mode == "powersave" || mode == "balance" ||
               mode == "performance" || mode == "fast" ||
               mode == "auto";
    }

    // qlib::json 严格类型，用户常把 bool/int 写成字符串，这些 helper 同时接受两种形态。
    static string_t intToStr(int v) {
        char tmp[16];
        FastSnprintf(tmp, sizeof(tmp), "%d", v);
        return string_t(tmp);
    }

    static bool parseBoolString(const char* s, bool& out) {
        if (!s || !*s) return false;
        if (!strcmp(s, "true") || !strcmp(s, "1") ||
            !strcmp(s, "on")   || !strcmp(s, "yes")) {
            out = true; return true;
        }
        if (!strcmp(s, "false") || !strcmp(s, "0") ||
            !strcmp(s, "off")   || !strcmp(s, "no")) {
            out = false; return true;
        }
        return false;
    }

    template <class Node>
    bool readBool(Node& node, const char* key, bool& out) {
        try {
            out = node[key].template get<bool>();
            return true;
        } catch (...) {}
        try {
            string_t s = node[key].template get<string_t>();
            return parseBoolString(s.c_str(), out);
        } catch (...) {}
        return false;
    }

    template <class Node>
    bool readInt(Node& node, const char* key, int& out) {
        try {
            out = node[key].template get<int>();
            return true;
        } catch (...) {}
        try {
            string_t s = node[key].template get<string_t>();
            if (s.empty()) return false;
            out = Fastatoi(s.c_str());
            return true;
        } catch (...) {}
        return false;
    }

    template <class Node>
    bool readFlt(Node& node, const char* key, float& out) {
        try {
            out = node[key].template get<float>();
            return true;
        } catch (...) {}
        try {
            string_t s = node[key].template get<string_t>();
            if (s.empty()) return false;
            out = strtof(s.c_str(), nullptr);
            return true;
        } catch (...) {}
        return false;
    }

    template <class Node>
    bool readStr(Node& node, const char* key, string_t& out) {
        try {
            out = node[key].template get<string_t>();
            return true;
        } catch (...) {}
        return false;
    }

    // 读频率，0/-1 视为未设置
    template <class Node>
    bool readFreq(Node& node, const char* key, string_t& out) {
        int v;
        if (readInt(node, key, v)) {
            if (v <= 0) return false;
            out = intToStr(v);
            return true;
        }
        return false;
    }

    template <class Node>
    void parseModelNode(Node& node, AppProfileModel& out) {
        readStr (node, "model_name", out.modelName);
        readStr (node, "mode",       out.modeName);
        readBool(node, "is_game",    out.isGame);
        readBool(node, "keep_alive", out.keepAlive);

        // packages
        try {
            auto& arr = node["packages"];
            int n = 0;
            for (int p = 0; p < AppProfileModel::MAX_PACKAGES; p++) {
                try {
                    string_t pkg = arr.array()[p].template get<string_t>();
                    if (pkg.empty()) break;
                    out.packages[n++] = pkg;
                } catch (...) { break; }
            }
            out.packageCount = n;
        } catch (...) {}

        // 频率 freq_min_c0 / freq_max_c0 / ...
        for (int i = 0; i < kClusterCount; i++) {
            FastSnprintf(buff, sizeof(buff), "freq_min_c%d", i);
            readFreq(node, buff, out.MinFreq[i]);
            FastSnprintf(buff, sizeof(buff), "freq_max_c%d", i);
            readFreq(node, buff, out.MaxFreq[i]);
        }

        parseGovernorNodes(node, out);

        readFreq(node, "gpu_min_mhz", out.GpuMinFreq);
        readFreq(node, "gpu_max_mhz", out.GpuMaxFreq);
        // 兼容旧嵌套写法 GpuFreq:{min_freq,max_freq}
        try {
            auto& gpu = node["GpuFreq"];
            if (out.GpuMinFreq.empty()) readFreq(gpu, "min_freq", out.GpuMinFreq);
            if (out.GpuMaxFreq.empty()) readFreq(gpu, "max_freq", out.GpuMaxFreq);
        } catch (...) {}

        try {
            auto& coreNode = node["CoreOnline"];
            for (int i = 0; i < kCoreCount; i++) {
                FastSnprintf(buff, sizeof(buff), "Core%d", i);
                int v;
                if (readInt(coreNode, buff, v)) {
                    out.Online[i] = (v == 1) ? 1 : (v == 0 ? 0 : -1);
                }
            }
        } catch (...) {}
    }

    template <class Node>
    void parseGovernorNodes(Node& node, AppProfileModel& out) {
        // qlib::json 不支持迭代 object key，故用白名单枚举调速器参数
        static const char* commonParams[] = {
            "hispeed_freq", "hispeed_load", "hispeed_cond_freq",
            "rtg_boost_freq", "pl", "zone_max_util_pct",
            "above_hispeed_delay", "go_hispeed_load", "target_loads",
            "min_sample_time", "timer_rate", "boost",
            "boostpulse_duration", "io_is_busy", "down_rate_limit_us",
            "up_rate_limit_us"
        };
        constexpr int N = sizeof(commonParams) / sizeof(commonParams[0]);

        for (int i = 0; i < kClusterCount; i++) {
            FastSnprintf(buff, sizeof(buff), "gov_c%d", i);
            try {
                auto& gov = node[buff];
                readStr(gov, "governor", out.Governor[i]);
                try {
                    auto& params = gov["params"];
                    int spCount = 0;
                    for (int k = 0; k < N && spCount < kMaxSchedParams; k++) {
                        string_t v;
                        if (readStr(params, commonParams[k], v) && !v.empty()) {
                            out.SchedParamName [i][spCount] = commonParams[k];
                            out.SchedParamValue[i][spCount] = v;
                            spCount++;
                        }
                    }
                    out.SchedParamCount[i] = spCount;
                } catch (...) {}
            } catch (...) {}
        }

        // 兼容 "governor":{"c0":"hmbird",...} 嵌套格式，仅当 gov_cN 未读到时补读
        try {
            auto& govObj = node["governor"];
            for (int i = 0; i < kClusterCount; i++) {
                if (!out.Governor[i].empty()) continue;
                FastSnprintf(buff, sizeof(buff), "c%d", i);
                string_t v;
                if (readStr(govObj, buff, v) && !v.empty())
                    out.Governor[i] = v;
            }
        } catch (...) {}
    }

    void resetState() {
        // 热重载: 把整张 catPolicyTable 复位到内置默认(maxL/maxB/gpu/cores 全部), 否则用户删某键后
        // 旧钳值/停泊掩码会残留至重启。app_policy_core_enable 不在此复位 —— 解析时 readBool 每次重写(缺失=false)。
        for (int i = 0; i < Config::AutoMode::kCatPolicyCount; i++) Config::AutoMode::catPolicyTable[i] = Config::AutoMode::catPolicyDefault[i];
        // 同理复位 fsm_policy 覆盖表到「全部未设置」(maxL/maxB=-1, gpu=0), 删键后不残留旧的按状态上限。
        for (int i = 0; i < 6; i++) Config::AutoMode::fsmPolicyTable[i] = Config::AutoMode::FsmPolicy{};
        for (int i = 0; i < kClusterCount; i++) {
            for (int j = 0; j < kMaxSchedParams; j++) {
                schedParam[i].Name [j] = "";
                schedParam[i].Value[j] = "";
            }
            Performances::MinFreq[i]     = "";
            Performances::MaxFreq[i]     = "";
            Performances::CpuGovernor[i] = "";
            LaunchBoost::BoostFreq[i]    = "";
        }
        for (int i = 0; i < kCoreCount; i++) Performances::Online[i] = -1;
        // 热重载时需清簇映射，防止删掉的 cN 键残留旧值
        for (int i = 0; i < kClusterCount; i++) Policy::CpuPolicy[i] = -1;
        GpuFreq::min_freq = "";
        GpuFreq::max_freq = "";
        Meta::name = ""; Meta::author = ""; Meta::loglevel = "";

        // ── Function 标量全量复位到编译期默认(settings.json 覆盖层的地基) ──
        // 覆盖层语义: config.json(出厂) → settings.json(App 开关)逐层覆盖; 热重载时任一层删了键,
        // 值必须回落到上一层/编译默认, 不能残留旧值。故每次重载先整体复位再重新解析两层。
        // 各默认值与 Config.hpp 内联初始化一字不差。
        Cpuset::enable = false;
        Cpuset::top_app = ""; Cpuset::foreground = ""; Cpuset::background = "";
        Cpuset::system_background = ""; Cpuset::restricted = "";
        LaunchBoost::enable = false; LaunchBoost::boost_rate_limit_ms = 35;
        OfficialMode::enable = false;
        SysTune::enable = true; BootTrim::enable = false;
        BgTrim::enable = false; BgTrim::interval_s = 90;
        TouchBoost::enable = false; TouchBoost::duration_ms = 1200; TouchBoost::level_pct = 55;
        Scheduler::enable = false;
        Scheduler::Sched_energy_aware = false; Scheduler::Sched_schedstats = false;
        Scheduler::Sched_latency_ns = ""; Scheduler::Sched_migration_cost_ns = "";
        Scheduler::Sched_min_granularity_ns = ""; Scheduler::Sched_wakeup_granularity_ns = "";
        Scheduler::Sched_nr_migrate = ""; Scheduler::Sched_util_clamp_min = "";
        Scheduler::Sched_util_clamp_max = "";
        GpuFreq::enable = true;
        AppProfile::enable = false;
        // AutoMode 标量(学习/温控/FAS/风驰/抢频/分类开关)
        AutoMode::enable = true; AutoMode::thermal_path = "";
        AutoMode::warm_temp = 50; AutoMode::cool_temp = 55; AutoMode::hot_temp = 70;
        AutoMode::vhot_temp = 80; AutoMode::veto_temp = 60;
        AutoMode::thermal_skin_gate = true; AutoMode::skin_temp = 47; AutoMode::junction_hard_temp = 100;
        AutoMode::low_batt = 20; AutoMode::crit_batt = 10;
        AutoMode::night_start = 23; AutoMode::night_end = 6;
        AutoMode::tick_sec = 2; AutoMode::learn_rate = 80; AutoMode::drain_high = 14;
        AutoMode::loadgov_enable = true; AutoMode::loadgov_tick_ms = 600;
        AutoMode::loadgov_up_pct = 88; AutoMode::loadgov_down_pct = 62;
        AutoMode::loadgov_headroom_pct = 120; AutoMode::loadgov_smooth_up_pct = 60;
        AutoMode::loadgov_deadzone_pct = 3; AutoMode::loadgov_dwell_ms = 0;
        AutoMode::cool_lean = ""; AutoMode::battery_bias = 0;
        AutoMode::dt_vhot = 80; AutoMode::dt_hot = 40; AutoMode::dt_drainy = 30;
        AutoMode::dt_sustained = 20; AutoMode::dt_thermal_hit = 100; AutoMode::dt_clean_cool = -60;
        AutoMode::dt_thermal_hit_skin_cool = 50;
        AutoMode::bg_uclamp_enable = true; AutoMode::bg_uclamp_cap = 18;
        AutoMode::explore_enable = true; AutoMode::explore_pct = 4;
        AutoMode::explore_offset = 60; AutoMode::reward_step = 8;
        AutoMode::anticlamp_enable = true; AutoMode::anticlamp_soft_pct = 70;
        AutoMode::anticlamp_hard_pct = 45; AutoMode::anticlamp_backoff = 40;
        AutoMode::anticlamp_attack_enable = false; AutoMode::anticlamp_attack_temp = 46;
        AutoMode::anticlamp_attack_hold = 8; AutoMode::anticlamp_attack_probe = 4;
        AutoMode::anticlamp_attack_budget = 6; AutoMode::anticlamp_attack_backoff_s = 30;
        AutoMode::anticlamp_attack_max_backoffs = 2;
        AutoMode::conf_var_enable = true; AutoMode::mad_scale = 60; AutoMode::obs_retain_pct = 97;
        AutoMode::auto_battery_enable = true; AutoMode::night_quiet_enable = true;
        AutoMode::fsm_sustained_enter = 54; AutoMode::fsm_sustained_exit = 48;
        AutoMode::fsm_warmup_grace_s = 45; AutoMode::fsm_warmup_bypass_temp = 60;
        AutoMode::fas_enable = true; AutoMode::fas_target_fps = 0; AutoMode::fas_auto_game = false;
        AutoMode::gamelist_enable = true; AutoMode::auto_dnd = false;
        AutoMode::fsm_weighted_load = true; AutoMode::fsm_joint_dvfs = true; AutoMode::game_uclamp_min = 20;
        AutoMode::game_fpsgo = false;
        AutoMode::power_enable = true; AutoMode::power_boost_value = 128;
        AutoMode::power_boost_jank_enable = true; AutoMode::power_boost_jank_psi = 20;
        AutoMode::power_boost_jank_busy = 85; AutoMode::power_boost_jank_load = 4.0f;
        AutoMode::power_boost_jank_busy_floor = 45;
        AutoMode::power_boost_cap_pct = 85;
        AutoMode::power_throttle_w = 7.5f; AutoMode::power_throttle_critical_w = 9.5f;
        AutoMode::power_throttle_big_pct = 80; AutoMode::power_throttle_critical_pct = 60;
        AutoMode::power_throttle_gpu_mhz = 680; AutoMode::power_throttle_critical_gpu_mhz = 460;
        AutoMode::camera_hold_enable = true; AutoMode::camera_hold_max_pct = 80; AutoMode::camera_hold_pkgs.clear();
        AutoMode::quiet_night_enable = true; AutoMode::quiet_night_tick_ms = 300000;
        AutoMode::oiface_keepalive_enable = true;
        AutoMode::app_policy_enable = true; AutoMode::app_policy_core_enable = false;
        // 游戏态调速器切换: 删键回到内置默认(hm/hmbird/scx), 不残留用户旧偏好列表。
        AutoMode::gaming_gov = "hm,hmbird,scx";

        AppProfile::modelCount      = 0;
        AppProfile::blacklistCount  = 0;
        AppProfile::currentMatch.store(-1);
        baseModelIdx = -1;
        { std::lock_guard<std::recursive_mutex> lk(AppProfile::profileMtx);
        for (int i = 0; i < AppProfile::MAX_BLACKLIST; i++)
            AppProfile::packageBlacklist[i] = "";
        for (int i = 0; i < AppProfile::MAX_MODELS; i++) {
            auto& m = AppProfile::Models[i];
            m.modelName = ""; m.modeName = ""; m.isGame = false; m.keepAlive = false; m.packageCount = 0;
            for (int j = 0; j < AppProfileModel::MAX_PACKAGES; j++) m.packages[j] = "";
            for (int j = 0; j < kClusterCount; j++) {
                m.MinFreq[j] = ""; m.MaxFreq[j] = ""; m.Governor[j] = "";
                m.SchedParamCount[j] = 0;
                for (int k = 0; k < kMaxSchedParams; k++) {
                    m.SchedParamName [j][k] = "";
                    m.SchedParamValue[j][k] = "";
                }
            }
            m.GpuMinFreq = ""; m.GpuMaxFreq = "";
            for (int j = 0; j < kCoreCount; j++) m.Online[j] = -1;
        }
        }  // profileMtx
    }

    // 发布 GPU 原子快照，供 gpuFreqGuard 跨线程无锁读取
    static void publishGpuSnapshot() {
        GpuFreq::min_mhz.store(Fastatoi(GpuFreq::min_freq.c_str()));
        GpuFreq::max_mhz.store(Fastatoi(GpuFreq::max_freq.c_str()));
        GpuFreq::enabled.store(GpuFreq::enable ? 1 : 0);
    }

    bool readConfig() {
        resetState();
        publishGpuSnapshot();   // 重载期间/失败时与清空后的 string 保持一致

        const char* configPaths[] = {
            configPath,
            "/storage/emulated/0/Android/Kin/config.json"
        };
        ifstream ifs;
        const char* usedConfigPath = configPath;
        for (const char* path : configPaths) {
            ifs.open(path, std::ios::binary);
            if (ifs.is_open()) { usedConfigPath = path; break; }
            ifs.clear();
        }
        if (!ifs.is_open()) {
            logger.Error("无法打开配置文件: %s 或 %s", configPaths[0], configPaths[1]);
            return false;
        }
        logger.Debug("Kin config.json: %s", usedConfigPath);
        std::string text((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
        int rc = json::parse(&json, text.data(), text.data() + text.size());
        if (rc != 0) { logger.Error("解析 config.json 失败 错误码: %d", rc); return false; }

        // ---- meta ----（loglevel 须在其余节点解析前生效）
        try {
            auto& m = json["meta"];
            readStr (m, "name",     Meta::name);
            readInt (m, "version",  Meta::version);
            readStr (m, "author",   Meta::author);
            readStr (m, "loglevel", Meta::loglevel);
        } catch (...) { logger.Warn("meta 节点缺失"); }
        if (!Meta::loglevel.empty()) logger.setLogLevel(Meta::loglevel);

        // ---- Policy ----
        try {
            auto& p = json["Policy"];
            for (int i = 0; i < kClusterCount; i++) {
                FastSnprintf(buff, sizeof(buff), "c%d", i);
                int v;
                if (readInt(p, buff, v)) {
                    Policy::CpuPolicy[i] = (v == 255) ? -1 : v;
                }
            }
        } catch (...) { logger.Warn("Policy 节点异常"); }

        // ---- Function ----
        parseFunctionSection(json);

        // ---- settings.json 覆盖层(App 开关) ----
        // 同结构 Function 子集, 用同一套 readXxx 解析: 键存在即覆盖出厂值, 缺键保持 config/默认。
        // 文件不存在/为空/解析失败 → 忽略覆盖层(仅 config 生效), 不影响守护启动。
        {
            const char* settingsPaths[] = {
                settingsPath,
                "/storage/emulated/0/Android/Kin/settings.json"
            };
            ifstream sf;
            for (const char* path : settingsPaths) {
                sf.open(path, std::ios::binary);
                if (sf.is_open()) break;
                sf.clear();
            }
            if (sf.is_open()) {
                std::string st((std::istreambuf_iterator<char>(sf)),
                               std::istreambuf_iterator<char>());
                if (!st.empty()) {
                    int rc2 = json::parse(&settingsJson, st.data(), st.data() + st.size());
                    if (rc2 == 0) parseFunctionSection(settingsJson);
                    else logger.Warn("settings.json 解析失败 错误码: %d, 已忽略覆盖层", rc2);
                }
            }
        }

        LoadConfig();
        // mode.txt 可能被外部写入非法值，回退 balance 避免守护进程整体失效
        if (mode.empty()) {
            logger.Warn("情景模式为空，回退到 balance");
            mode = "balance";
        }
        if (!switchConfig()) {
            logger.Warn("情景模式无效: '%s'，回退到 balance", mode.c_str());
            mode = "balance";
        }
        logger.Info("当前情景模式: %s", mode.c_str());

        parseBlacklist();
        parseModels();
        applyBaseModel();
        loadPerApp();

        publishGpuSnapshot();
        return true;
    }

    // 每行 "包名 画像名"，# 注释，* 开头为全局默认行
    void loadPerApp() {
        using namespace Config;
        AppProfile::perAppCount = 0;
        AppProfile::perAppGlobal = "";
        static const char* paths[] = {
            "/sdcard/Android/Kin/AI/perapp_powermode.txt",
            "/storage/emulated/0/Android/Kin/AI/perapp_powermode.txt",
            // 兼容旧位置(迁移窗口内仍能读到)
            "/sdcard/Android/Kin/perapp_powermode.txt",
            "/storage/emulated/0/Android/Kin/perapp_powermode.txt",
        };
        FILE* fp = nullptr;
        for (const char* p : paths) { fp = fopen(p, "r"); if (fp) break; }
        if (!fp) { logger.Info("perapp_powermode.txt 不存在,跳过(仅用 config 画像)"); return; }
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            char* s = line;
            while (*s == ' ' || *s == '\t') s++;
            size_t len = strlen(s);
            while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' ' || s[len-1] == '\t'))
                s[--len] = '\0';
            if (len == 0 || s[0] == '#') continue;
            if (s[0] == '*') {
                char* sp = s + 1;
                while (*sp == ' ' || *sp == '\t') sp++;
                if (*sp) AppProfile::perAppGlobal = sp;
                continue;
            }
            char* sep = s;
            while (*sep && *sep != ' ' && *sep != '\t') sep++;
            if (!*sep) continue;
            *sep = '\0';
            char* mode = sep + 1;
            while (*mode == ' ' || *mode == '\t') mode++;
            if (!*mode) continue;
            // 可选第三列 = FAS 目标帧率(如 "com.x game_heavy 120")。切出 mode 后的下一 token 尝试解析为整数。
            int fps = 0;
            char* msep = mode;
            while (*msep && *msep != ' ' && *msep != '\t') msep++;
            if (*msep) {
                *msep = '\0';
                char* fpsTok = msep + 1;
                while (*fpsTok == ' ' || *fpsTok == '\t') fpsTok++;
                if (*fpsTok >= '0' && *fpsTok <= '9') {
                    int v = atoi(fpsTok);
                    if (v >= 20 && v <= 240) fps = v;   // 合理帧率区间, 越界忽略
                }
            }
            if (AppProfile::perAppCount < Config::AppProfile::MAX_PERAPP) {
                AppProfile::perAppPkg  [AppProfile::perAppCount] = s;
                AppProfile::perAppModel[AppProfile::perAppCount] = mode;
                AppProfile::perAppFps  [AppProfile::perAppCount] = fps;
                AppProfile::perAppCount++;
            }
        }
        fclose(fp);
        // 仅在条数变化时打印，避免 inotify 重载刷屏
        static int lastPerAppCount = -1;
        if (AppProfile::perAppCount != lastPerAppCount) {
            logger.Info("perapp_powermode.txt 已加载: %d 条 App 画像覆盖(优先于 config)",
                        AppProfile::perAppCount);
            lastPerAppCount = AppProfile::perAppCount;
        }
    }

private:
    // doc = config.json 或 settings.json 的解析文档; 两者共用同一套读取逻辑(读取器只覆盖存在的键)。
    void parseFunctionSection(json_view_t& doc) {
        try {
            auto& F = doc["Function"];

            try {
                auto& c = F["Cpuset"];
                readBool(c, "enable",            Cpuset::enable);
                readStr (c, "top_app",           Cpuset::top_app);
                readStr (c, "foreground",        Cpuset::foreground);
                readStr (c, "background",        Cpuset::background);
                readStr (c, "system_background", Cpuset::system_background);
                readStr (c, "restricted",        Cpuset::restricted);
            } catch (...) { logger.Debug("无 Cpuset 节点"); }

            try {
                auto& l = F["LaunchBoost"];
                readBool(l, "enable",              LaunchBoost::enable);
                readInt (l, "boost_rate_limit_ms", LaunchBoost::boost_rate_limit_ms);
                try {
                    auto& bf = l["BoostFreq"];
                    for (int i = 0; i < kClusterCount; i++) {
                        FastSnprintf(buff, sizeof(buff), "c%d", i);
                        readStr(bf, buff, LaunchBoost::BoostFreq[i]);
                    }
                } catch (...) {}
            } catch (...) {}

            try {
                auto& o = F["OfficialMode"];
                readBool(o, "enable", OfficialMode::enable);
            } catch (...) {}

            try { readBool(F["SysTune"],  "enable", SysTune::enable);  } catch (...) {}
            try { readBool(F["BootTrim"], "enable", BootTrim::enable); } catch (...) {}
            try {
                auto& bt = F["BgTrim"];
                readBool(bt, "enable", BgTrim::enable);
                readInt (bt, "interval_s", BgTrim::interval_s);
            } catch (...) {}
            try {
                auto& tb = F["TouchBoost"];
                readBool(tb, "enable", TouchBoost::enable);
                readInt (tb, "duration_ms", TouchBoost::duration_ms);
                readInt (tb, "level_pct", TouchBoost::level_pct);
            } catch (...) {}
            try {
                auto& s = F["Scheduler"];
                readBool(s, "enable",                       Scheduler::enable);
                readBool(s, "sched_energy_aware",           Scheduler::Sched_energy_aware);
                readBool(s, "sched_schedstats",             Scheduler::Sched_schedstats);
                readStr (s, "sched_latency_ns",             Scheduler::Sched_latency_ns);
                readStr (s, "sched_migration_cost_ns",      Scheduler::Sched_migration_cost_ns);
                readStr (s, "sched_min_granularity_ns",     Scheduler::Sched_min_granularity_ns);
                readStr (s, "sched_wakeup_granularity_ns",  Scheduler::Sched_wakeup_granularity_ns);
                readStr (s, "sched_nr_migrate",             Scheduler::Sched_nr_migrate);
                readStr (s, "sched_util_clamp_min",         Scheduler::Sched_util_clamp_min);
                readStr (s, "sched_util_clamp_max",         Scheduler::Sched_util_clamp_max);
            } catch (...) {}

            try {
                auto& g = F["GpuFreq"];
                readBool(g, "enable", GpuFreq::enable);
            } catch (...) {}

            try {
                auto& ap = F["AppProfile"];
                readBool(ap, "enable", AppProfile::enable);
            } catch (...) {}

            try {
                auto& a = F["AutoMode"];
                readBool(a, "enable",      AutoMode::enable);
                readBool(a, "fas_enable",      AutoMode::fas_enable);
                readBool(a, "fsm_weighted_load", AutoMode::fsm_weighted_load);
                readBool(a, "fsm_joint_dvfs",  AutoMode::fsm_joint_dvfs);
                readBool(a, "game_fpsgo",      AutoMode::game_fpsgo);
                // 相机保持(ASB V60 camera_hold) + 夜间安静(ASB V61 Quiet Night)
                readBool(a, "camera_hold_enable",   AutoMode::camera_hold_enable);
                readInt (a, "camera_hold_max_pct",  AutoMode::camera_hold_max_pct);
                readStr (a, "camera_hold_pkgs",     AutoMode::camera_hold_pkgs);
                readBool(a, "quiet_night_enable",   AutoMode::quiet_night_enable);
                readInt (a, "quiet_night_tick_ms",  AutoMode::quiet_night_tick_ms);
                readBool(a, "oiface_keepalive_enable", AutoMode::oiface_keepalive_enable);
                if (AutoMode::camera_hold_max_pct < 40 || AutoMode::camera_hold_max_pct > 100)
                    AutoMode::camera_hold_max_pct = 80;
                if (AutoMode::quiet_night_tick_ms < 60000 || AutoMode::quiet_night_tick_ms > 1800000)
                    AutoMode::quiet_night_tick_ms = 300000;
                readInt (a, "game_uclamp_min", AutoMode::game_uclamp_min);
                readBool(a, "power_enable",              AutoMode::power_enable);
                readInt (a, "power_boost_value",         AutoMode::power_boost_value);
                readBool(a, "power_boost_jank_enable",   AutoMode::power_boost_jank_enable);
                readInt (a, "power_boost_jank_psi",      AutoMode::power_boost_jank_psi);
                readInt (a, "power_boost_jank_busy",     AutoMode::power_boost_jank_busy);
                readFlt (a, "power_boost_jank_load",     AutoMode::power_boost_jank_load);
                readInt (a, "power_boost_jank_busy_floor", AutoMode::power_boost_jank_busy_floor);
                readInt (a, "power_boost_cap_pct",       AutoMode::power_boost_cap_pct);
                readFlt (a, "power_throttle_w",          AutoMode::power_throttle_w);
                readFlt (a, "power_throttle_critical_w", AutoMode::power_throttle_critical_w);
                readInt (a, "power_throttle_big_pct",    AutoMode::power_throttle_big_pct);
                readInt (a, "power_throttle_critical_pct", AutoMode::power_throttle_critical_pct);
                readInt (a, "power_throttle_gpu_mhz",          AutoMode::power_throttle_gpu_mhz);
                readInt (a, "power_throttle_critical_gpu_mhz", AutoMode::power_throttle_critical_gpu_mhz);
                readBool(a, "fas_auto_game",   AutoMode::fas_auto_game);
                readBool(a, "gamelist_enable", AutoMode::gamelist_enable);
                readBool(a, "auto_dnd",        AutoMode::auto_dnd);
                readStr (a, "gaming_gov",      AutoMode::gaming_gov);
                readInt (a, "fas_target_fps",  AutoMode::fas_target_fps);
                readStr (a, "thermal_path", AutoMode::thermal_path);
                readInt (a, "warm_temp",   AutoMode::warm_temp);
                readInt (a, "cool_temp",   AutoMode::cool_temp);
                readInt (a, "hot_temp",    AutoMode::hot_temp);
                readInt (a, "vhot_temp",   AutoMode::vhot_temp);
                readInt (a, "veto_temp",   AutoMode::veto_temp);
                // ASB v59 皮肤温度锚定热决策(App 可调)
                readBool(a, "thermal_skin_gate",  AutoMode::thermal_skin_gate);
                readInt (a, "skin_temp",          AutoMode::skin_temp);
                readInt (a, "junction_hard_temp", AutoMode::junction_hard_temp);
                // 热安全护栏(双侧钳): skin_temp 过低会使偏移(veto-skin)过大 → td 恒≥veto 反向永久否决;
                // 过高则外壳烫手也不否决。junction_hard 上界须 <120(normTemp 封顶 120, 否则硬网不可达),
                // 下界须 >95(本机结温正常带上限, 否则日常重载误触发); 0=关闭语义保留。
                if (AutoMode::skin_temp < 40 || AutoMode::skin_temp > 60) {
                    logger.Warn("AutoMode: skin_temp(%d) 越界[40,60], 回退 47", AutoMode::skin_temp);
                    AutoMode::skin_temp = 47;
                }
                if (AutoMode::junction_hard_temp != 0 &&
                    (AutoMode::junction_hard_temp < 95 || AutoMode::junction_hard_temp > 115)) {
                    logger.Warn("AutoMode: junction_hard_temp(%d) 越界[95,115](0=关), 回退 100",
                                AutoMode::junction_hard_temp);
                    AutoMode::junction_hard_temp = 100;
                }
                readInt (a, "low_batt",    AutoMode::low_batt);
                readInt (a, "crit_batt",   AutoMode::crit_batt);
                readInt (a, "night_start", AutoMode::night_start);
                readInt (a, "night_end",   AutoMode::night_end);
                readInt (a, "tick_sec",    AutoMode::tick_sec);
                // ── 负载闭环调频(越界回缺省, 与 battery_bias 同款就地校验) ──
                readBool(a, "loadgov_enable", AutoMode::loadgov_enable);
                readInt (a, "loadgov_tick_ms", AutoMode::loadgov_tick_ms);
                if (AutoMode::loadgov_tick_ms < 200 || AutoMode::loadgov_tick_ms > 2000) {
                    logger.Warn("AutoMode: loadgov_tick_ms(%d) 越界[200,2000], 回退 600",
                                AutoMode::loadgov_tick_ms);
                    AutoMode::loadgov_tick_ms = 600;
                }
                readInt (a, "loadgov_up_pct", AutoMode::loadgov_up_pct);
                readInt (a, "loadgov_down_pct", AutoMode::loadgov_down_pct);
                readInt (a, "loadgov_headroom_pct", AutoMode::loadgov_headroom_pct);
                readInt (a, "loadgov_smooth_up_pct", AutoMode::loadgov_smooth_up_pct);
                readInt (a, "loadgov_deadzone_pct", AutoMode::loadgov_deadzone_pct);
                readInt (a, "loadgov_dwell_ms", AutoMode::loadgov_dwell_ms);
                // 门槛类一律钳进 (0,100), 且强制 down < up: 否则"滞回带"宽度为 0 或负,
                // 升频减速公式失去意义(LoadGovernor::normalize 还会再兜一层, 这里提前给可见告警)。
                if (AutoMode::loadgov_up_pct   < 10 || AutoMode::loadgov_up_pct   > 100) AutoMode::loadgov_up_pct   = 88;
                if (AutoMode::loadgov_down_pct < 0  || AutoMode::loadgov_down_pct > 99)  AutoMode::loadgov_down_pct = 62;
                if (AutoMode::loadgov_down_pct >= AutoMode::loadgov_up_pct)
                    AutoMode::loadgov_down_pct = AutoMode::loadgov_up_pct - 1;
                if (AutoMode::loadgov_headroom_pct < 100 || AutoMode::loadgov_headroom_pct > 200)
                    AutoMode::loadgov_headroom_pct = 120;
                if (AutoMode::loadgov_smooth_up_pct < 1 || AutoMode::loadgov_smooth_up_pct > 100)
                    AutoMode::loadgov_smooth_up_pct = 60;
                if (AutoMode::loadgov_deadzone_pct < 0 || AutoMode::loadgov_deadzone_pct > 20)
                    AutoMode::loadgov_deadzone_pct = 3;
                if (AutoMode::loadgov_dwell_ms < 0 || AutoMode::loadgov_dwell_ms > 5000)
                    AutoMode::loadgov_dwell_ms = 0;
                readInt (a, "learn_rate",  AutoMode::learn_rate);
                readInt (a, "drain_high",  AutoMode::drain_high);
                readStr (a, "cool_lean",   AutoMode::cool_lean);
                readInt (a, "battery_bias", AutoMode::battery_bias);
                if (AutoMode::battery_bias < 0)   AutoMode::battery_bias = 0;
                if (AutoMode::battery_bias > 300) AutoMode::battery_bias = 300;
                // 会话方向信号 dt 系数(可配置; 缺省保持 ASB 默认值)
                readInt (a, "dt_vhot",        AutoMode::dt_vhot);
                readInt (a, "dt_hot",         AutoMode::dt_hot);
                readInt (a, "dt_drainy",      AutoMode::dt_drainy);
                readInt (a, "dt_sustained",   AutoMode::dt_sustained);
                readInt (a, "dt_thermal_hit", AutoMode::dt_thermal_hit);
                readInt (a, "dt_thermal_hit_skin_cool", AutoMode::dt_thermal_hit_skin_cool);
                readBool(a, "bg_uclamp_enable", AutoMode::bg_uclamp_enable);
                readInt (a, "bg_uclamp_cap",    AutoMode::bg_uclamp_cap);
                if (AutoMode::bg_uclamp_cap < 0)   AutoMode::bg_uclamp_cap = 0;
                if (AutoMode::bg_uclamp_cap > 99)  AutoMode::bg_uclamp_cap = 99;
                readInt (a, "dt_clean_cool",  AutoMode::dt_clean_cool);
                // 探索 + 奖励闭环
                readBool(a, "explore_enable", AutoMode::explore_enable);
                readInt (a, "explore_pct",    AutoMode::explore_pct);
                readInt (a, "explore_offset", AutoMode::explore_offset);
                readInt (a, "reward_step",    AutoMode::reward_step);
                if (AutoMode::explore_pct    < 0)   AutoMode::explore_pct    = 0;
                if (AutoMode::explore_pct    > 30)  AutoMode::explore_pct    = 30;
                if (AutoMode::explore_offset < 0)   AutoMode::explore_offset = 0;
                if (AutoMode::explore_offset > 200) AutoMode::explore_offset = 200;
                if (AutoMode::reward_step    < 0)   AutoMode::reward_step    = 0;
                if (AutoMode::reward_step    > 50)  AutoMode::reward_step    = 50;
                // 厂商压频检测 / anti-clamp
                readBool(a, "anticlamp_enable",   AutoMode::anticlamp_enable);
                readInt (a, "anticlamp_soft_pct", AutoMode::anticlamp_soft_pct);
                readInt (a, "anticlamp_hard_pct", AutoMode::anticlamp_hard_pct);
                readInt (a, "anticlamp_backoff",  AutoMode::anticlamp_backoff);
                if (AutoMode::anticlamp_soft_pct < 1)   AutoMode::anticlamp_soft_pct = 1;
                if (AutoMode::anticlamp_soft_pct > 100) AutoMode::anticlamp_soft_pct = 100;
                if (AutoMode::anticlamp_hard_pct < 1)   AutoMode::anticlamp_hard_pct = 1;
                if (AutoMode::anticlamp_hard_pct > 100) AutoMode::anticlamp_hard_pct = 100;
                if (AutoMode::anticlamp_backoff  < 0)   AutoMode::anticlamp_backoff  = 0;
                if (AutoMode::anticlamp_backoff  > 200) AutoMode::anticlamp_backoff  = 200;
                // anti-clamp 攻击侧(主动对抗抬频; 默认关)
                // Legacy key is accepted but cannot enable vendor-limit overrides.
                AutoMode::anticlamp_attack_enable = false;
                readInt (a, "anticlamp_attack_temp",   AutoMode::anticlamp_attack_temp);
                readInt (a, "anticlamp_attack_hold",   AutoMode::anticlamp_attack_hold);
                readInt (a, "anticlamp_attack_probe",  AutoMode::anticlamp_attack_probe);
                if (AutoMode::anticlamp_attack_temp  < 30)  AutoMode::anticlamp_attack_temp  = 30;
                if (AutoMode::anticlamp_attack_temp  > 95)  AutoMode::anticlamp_attack_temp  = 95;
                if (AutoMode::anticlamp_attack_hold  < 1)   AutoMode::anticlamp_attack_hold  = 1;
                if (AutoMode::anticlamp_attack_hold  > 120) AutoMode::anticlamp_attack_hold  = 120;
                if (AutoMode::anticlamp_attack_probe < 1)   AutoMode::anticlamp_attack_probe = 1;
                if (AutoMode::anticlamp_attack_probe > 120) AutoMode::anticlamp_attack_probe = 120;
                readInt (a, "anticlamp_attack_budget",       AutoMode::anticlamp_attack_budget);
                readInt (a, "anticlamp_attack_backoff_s",    AutoMode::anticlamp_attack_backoff_s);
                readInt (a, "anticlamp_attack_max_backoffs", AutoMode::anticlamp_attack_max_backoffs);
                if (AutoMode::anticlamp_attack_budget       < 1)   AutoMode::anticlamp_attack_budget       = 1;
                if (AutoMode::anticlamp_attack_budget       > 50)  AutoMode::anticlamp_attack_budget       = 50;
                if (AutoMode::anticlamp_attack_backoff_s    < 1)   AutoMode::anticlamp_attack_backoff_s    = 1;
                if (AutoMode::anticlamp_attack_backoff_s    > 600) AutoMode::anticlamp_attack_backoff_s    = 600;
                if (AutoMode::anticlamp_attack_max_backoffs < 1)   AutoMode::anticlamp_attack_max_backoffs = 1;
                if (AutoMode::anticlamp_attack_max_backoffs > 10)  AutoMode::anticlamp_attack_max_backoffs = 10;
                // 置信度方差修正 + eff_obs 滑窗
                readBool(a, "conf_var_enable", AutoMode::conf_var_enable);
                readInt (a, "mad_scale",       AutoMode::mad_scale);
                readInt (a, "obs_retain_pct",  AutoMode::obs_retain_pct);
                if (AutoMode::mad_scale      < 1)   AutoMode::mad_scale      = 1;
                if (AutoMode::mad_scale      > 500) AutoMode::mad_scale      = 500;
                if (AutoMode::obs_retain_pct < 50)  AutoMode::obs_retain_pct = 50;
                if (AutoMode::obs_retain_pct > 100) AutoMode::obs_retain_pct = 100;
                readBool(a, "auto_battery_enable", AutoMode::auto_battery_enable);
                readBool(a, "night_quiet_enable",  AutoMode::night_quiet_enable);
                readInt (a, "fsm_sustained_enter", AutoMode::fsm_sustained_enter);
                readInt (a, "fsm_sustained_exit",  AutoMode::fsm_sustained_exit);
                // FSM 预热宽限(会话开头抑制 SUSTAINED)
                readInt (a, "fsm_warmup_grace_s",     AutoMode::fsm_warmup_grace_s);
                readInt (a, "fsm_warmup_bypass_temp", AutoMode::fsm_warmup_bypass_temp);
                if (AutoMode::fsm_warmup_grace_s   < 0)   AutoMode::fsm_warmup_grace_s   = 0;
                if (AutoMode::fsm_warmup_grace_s   > 300) AutoMode::fsm_warmup_grace_s   = 300;
                if (AutoMode::fsm_warmup_bypass_temp < 40) AutoMode::fsm_warmup_bypass_temp = 40;
                if (AutoMode::fsm_warmup_bypass_temp > 95) AutoMode::fsm_warmup_bypass_temp = 95;
                // 按 App 类别(ApplicationInfo.category)的封顶策略, 可逐类别覆盖内置默认。
                //   "app_policy_enable": 总开关; "app_policy": { "video": {"maxL":85,"maxB":60,"gpu":0}, ... }
                // 键名见 Config::AutoMode::catPolicyName(game/audio/video/image/social/news/maps/productivity/undefined)。
                readBool(a, "app_policy_enable", AutoMode::app_policy_enable);
                // 关核心总开关, 默认 false(opt-in)。解析失败/缺失 → 保持 false。
                readBool(a, "app_policy_core_enable", AutoMode::app_policy_core_enable);
                try {
                    auto& pol = a["app_policy"];
                    for (int i = 0; i < Config::AutoMode::kCatPolicyCount; i++) {
                        try {
                            auto& c = pol[AutoMode::catPolicyName[i]];
                            auto& e = AutoMode::catPolicyTable[i];
                            readInt(c, "maxL", e.maxPctL);
                            readInt(c, "maxB", e.maxPctB);
                            readInt(c, "gpu",  e.gpuPct);
                            // 护栏: 频率% 钳到 [10,100], GPU% 钳到 [0,100]。
                            kin::PolicyEntry checkedEntry{e.maxPctL, e.maxPctB, e.gpuPct};
                            kin::validatePolicyEntry(checkedEntry, false);
                            e.maxPctL = checkedEntry.maxL;
                            e.maxPctB = checkedEntry.maxB;
                            e.gpuPct = checkedEntry.gpu;
                            // cores: 8 字符在线掩码 "11111100"(cpu0..7, 1=在线 0=停泊)。缺失/非法字符
                            // 按在线(=1)处理, 故不写或写错绝不会误关核。硬护栏在 scheduler 落实(不关 cpu0/整簇)。
                            string_t mask;
                            if (readStr(c, "cores", mask) && !mask.empty()) {
                                const char* s = mask.c_str();
                                int len = (int)mask.size();
                                for (int k = 0; k < kCoreCount; k++) {
                                    char ch = (k < len) ? s[k] : '1';   // 短于 8 位的尾部按在线
                                    e.cores[k] = (ch == '0') ? 0 : 1;   // 仅 '0' 视为停泊, 其余=在线
                                }
                            }
                        } catch (...) {}   // 该类别未配置 → 保留内置默认
                    }
                } catch (...) {}           // 无 app_policy 节点 → 全用内置默认
                // 按 FSM 状态(deep_idle..gaming)覆盖内置频率曲线的上限, 可逐状态配置:
                //   "fsm_policy": { "moderate": {"maxL":"75","maxB":"70","gpu":"65"}, ... }
                // maxL/maxB 合法域 [10,100](% 相对本机 cpuinfo_max_freq), 缺失/越界 → -1=保持内置曲线;
                // gpu 合法域 [0,100], 0/缺失/越界 → 用内置 AutoFsm::gpuPctForState。值与 app_policy
                // 同约定写字符串(readInt 两种形态都收)。表已在 resetState 整体复位, 删键不残留。
                try {
                    auto& fp = a["fsm_policy"];
                    int nOverride = 0;
                    for (int i = 0; i < 6; i++) {
                        try {
                            auto& c = fp[AutoMode::fsmPolicyName[i]];
                            auto& e = AutoMode::fsmPolicyTable[i];
                            readInt(c, "maxL", e.maxL);
                            readInt(c, "maxB", e.maxB);
                            readInt(c, "gpu",  e.gpu);
                            // 护栏: 越界一律回到「未设置」而不是半钳 —— 用户写错不影响内置行为。
                            kin::PolicyEntry checkedEntry{e.maxL, e.maxB, e.gpu};
                            kin::validatePolicyEntry(checkedEntry, true);
                            e.maxL = checkedEntry.maxL;
                            e.maxB = checkedEntry.maxB;
                            e.gpu = checkedEntry.gpu;
                            if (e.maxL > 0 || e.maxB > 0 || e.gpu > 0) nOverride++;
                        } catch (...) {}   // 该状态未配置 → 保持未设置
                    }
                    if (nOverride > 0)
                        logger.Info("AutoMode: fsm_policy 覆盖 %d/6 个状态的频率/GPU 上限", nOverride);
                } catch (...) {}           // 无 fsm_policy 节点 → 无覆盖
                // 进/出温度安全护栏: enter 必须严格大于 exit, 否则进出抖动; 异常时回退默认 57/49。
                kin::AutoModeConfigValues checked{
                    AutoMode::low_batt, AutoMode::crit_batt,
                    AutoMode::fsm_sustained_enter, AutoMode::fsm_sustained_exit,
                    AutoMode::anticlamp_soft_pct, AutoMode::anticlamp_hard_pct,
                    AutoMode::anticlamp_attack_temp, AutoMode::explore_pct,
                    AutoMode::explore_offset, AutoMode::reward_step,
                    AutoMode::camera_hold_max_pct, AutoMode::game_uclamp_min,
                    AutoMode::power_boost_value, AutoMode::power_boost_jank_psi,
                    AutoMode::power_boost_jank_busy, AutoMode::power_boost_cap_pct,
                    AutoMode::power_throttle_big_pct, AutoMode::power_throttle_critical_pct,
                    AutoMode::fas_target_fps, AutoMode::power_throttle_w,
                    AutoMode::power_throttle_critical_w, AutoMode::power_boost_jank_load,
                    AutoMode::night_start, AutoMode::night_end, AutoMode::tick_sec,
                    AutoMode::learn_rate, AutoMode::drain_high, AutoMode::warm_temp,
                    AutoMode::cool_temp, AutoMode::hot_temp, AutoMode::vhot_temp,
                    AutoMode::veto_temp};
                kin::validateAutoModeConfig(checked);
                AutoMode::low_batt = checked.lowBatt;
                AutoMode::crit_batt = checked.critBatt;
                AutoMode::fsm_sustained_enter = checked.thermalEnter;
                AutoMode::fsm_sustained_exit = checked.thermalExit;
                AutoMode::anticlamp_soft_pct = checked.antiSoft;
                AutoMode::anticlamp_hard_pct = checked.antiHard;
                AutoMode::anticlamp_attack_temp = checked.attackTemp;
                AutoMode::explore_pct = checked.explorePct;
                AutoMode::explore_offset = checked.exploreOffset;
                AutoMode::reward_step = checked.rewardStep;
                AutoMode::camera_hold_max_pct = checked.cameraPct;
                AutoMode::game_uclamp_min = checked.gameUclamp;
                AutoMode::power_boost_value = checked.boostValue;
                AutoMode::power_boost_jank_psi = checked.boostPsi;
                AutoMode::power_boost_jank_busy = checked.boostBusy;
                AutoMode::power_boost_cap_pct = checked.boostCapPct;
                AutoMode::power_throttle_big_pct = checked.throttleBigPct;
                AutoMode::power_throttle_critical_pct = checked.throttleCriticalPct;
                AutoMode::fas_target_fps = checked.fasTargetFps;
                AutoMode::power_throttle_w = checked.throttleW;
                AutoMode::power_throttle_critical_w = checked.throttleCriticalW;
                AutoMode::power_boost_jank_load = checked.boostJankLoad;
                AutoMode::night_start = checked.nightStart;
                AutoMode::night_end = checked.nightEnd;
                AutoMode::tick_sec = checked.tickSec;
                AutoMode::learn_rate = checked.learnRate;
                AutoMode::drain_high = checked.drainHigh;
                AutoMode::warm_temp = checked.warmTemp;
                AutoMode::cool_temp = checked.coolTemp;
                AutoMode::hot_temp = checked.hotTemp;
                AutoMode::vhot_temp = checked.veryHotTemp;
                AutoMode::veto_temp = checked.vetoTemp;
            } catch (...) { logger.Debug("无 AutoMode 节点, 用默认阈值"); }
        } catch (const qlib::exception& e) {
            logger.Warn("Function 节点异常: %s", e.what());
        }
    }

    void parseBlacklist() {
        try {
            auto& bl = json["package_blacklist"];
            int n = 0;
            { std::lock_guard<std::recursive_mutex> lk(AppProfile::profileMtx);
            for (int i = 0; i < AppProfile::MAX_BLACKLIST; i++) {
                try {
                    string_t pkg = bl.array()[i].get<string_t>();
                    if (pkg.empty()) break;
                    AppProfile::packageBlacklist[n++] = pkg;
                } catch (...) { break; }
            }
            AppProfile::blacklistCount = n;
            }  // profileMtx
            // 仅在条数变化时打印，避免重载刷屏
            static int lastBlacklistCount = -1;
            if (n > 0 && n != lastBlacklistCount) {
                logger.Info("包名黑名单已加载: %d 条", n);
                lastBlacklistCount = n;
            }
        } catch (...) { logger.Debug("无 package_blacklist"); }
    }

    void parseModels() {
        // 首次加载打印详细日志，后续重载降为 Debug 避免刷屏
        static bool firstLoad = true;
        try {
            auto& arr = json["models"];
            int n = 0;
            for (int m = 0; m < AppProfile::MAX_MODELS; m++) {
                try {
                    auto& node = arr.array()[m];
                    string_t mName;
                    readStr(node, "model_name", mName);
                    if (mName.empty()) break;
                    parseModelNode(node, AppProfile::Models[n]);
                    if (firstLoad)
                        logger.Debug("模型已加载: %s (mode=%s, 包名=%d, isGame=%d, c0=[%s,%s])",
                                AppProfile::Models[n].modelName.c_str(),
                                AppProfile::Models[n].modeName.c_str(),
                                AppProfile::Models[n].packageCount,
                                AppProfile::Models[n].isGame ? 1 : 0,
                                AppProfile::Models[n].MinFreq[0].c_str(),
                                AppProfile::Models[n].MaxFreq[0].c_str());
                    n++;
                } catch (...) { break; }
            }
            AppProfile::modelCount = n;
            if (firstLoad) {
                logger.Info("共加载 %d 个模型", n);
                firstLoad = false;
            }
        } catch (...) { logger.Debug("无 models 节点"); }
    }

    void applyBaseModel() {
        // mode=auto 时没有名为 "auto" 的模型: 加载阶段先落到 balance 仅作安全占位,
        // 之后由 applyCurrentMode 的 auto 分支在同一轮内解析出真实基础档并覆盖应用。
        // 占位 balance 在 reload/前台切换路径下并不会写入硬件(applyCurrentMode 直接套解析档),
        // 因此这里静默加载,避免日志反复出现 "基础模型: balance" 让用户误以为 auto 下基础档仍在生效。
        const bool placeholder = (mode == "auto");
        const char* want = placeholder ? "balance" : mode.c_str();
        loadBaseModelByName(want, /*silent=*/placeholder);
    }

public:
    // 把名为 modeName 的基础模型加载进 Performances/schedParam/GpuFreq。
    // 供自动模式(mode=auto)在 powersave/balance/performance 间切换时复用;
    // 不改写 this->mode(仍为 auto)。返回是否找到并加载成功。
    // silent=true: 仅静默载入结构体(用于 auto 占位档),不打印 "CPU簇/基础模型" 日志。
    bool loadBaseModelByName(const char* modeName, bool silent = false) {
        baseModelIdx = -1;
        for (int i = 0; i < AppProfile::modelCount; i++) {
            // qlib::string operator== 已修为按内容比较；此处保守保留 strcmp（语义本就正确）
            const char* mn = AppProfile::Models[i].modeName.c_str();
            if (mn && modeName && strcmp(mn, modeName) == 0) {
                baseModelIdx = i;
                break;
            }
        }

        if (baseModelIdx < 0) {
            logger.Error("models 中找不到 mode='%s' 的基础模型 — 无法应用配置", modeName);
            return false;
        }

        const auto& m = AppProfile::Models[baseModelIdx];
        for (int i = 0; i < kClusterCount; i++) {
            Performances::MinFreq[i]     = m.MinFreq[i];
            Performances::MaxFreq[i]     = m.MaxFreq[i];
            Performances::CpuGovernor[i] = m.Governor[i];
            if (!silent &&
                (!Performances::MinFreq[i].empty() ||
                 !Performances::MaxFreq[i].empty() ||
                 !Performances::CpuGovernor[i].empty())) {
                logger.Info("CPU簇 %d: min=%s max=%s gov=%s",
                            Policy::CpuPolicy[i],
                            Performances::MinFreq[i].c_str(),
                            Performances::MaxFreq[i].c_str(),
                            Performances::CpuGovernor[i].c_str());
            }
        }
        for (int i = 0; i < kCoreCount; i++)
            Performances::Online[i] = m.Online[i];

        for (int i = 0; i < kClusterCount; i++) {
            int cnt = m.SchedParamCount[i];
            for (int j = 0; j < cnt && j < kMaxSchedParams; j++) {
                schedParam[i].Name [j] = m.SchedParamName [i][j];
                schedParam[i].Value[j] = m.SchedParamValue[i][j];
            }
        }
        GpuFreq::min_freq = m.GpuMinFreq;
        GpuFreq::max_freq = m.GpuMaxFreq;
        if (!silent)
            logger.Info("基础模型: %s (mode=%s)", m.modelName.c_str(), mode.c_str());
        return true;
    }
};
