# Kin-Daemon 优化交接

## 范围与状态

未最终完成。工作仓库为 `E:\项目文件\Kin\Kin-Daemon`；保留现有未提交改动，不用 reset/checkout 清理。不要将 Windows 测试通过等同于 Android 实机验证。

## 本轮：负载闭环调频（P0-2 / P0-3，移植 ChiRi CLG 决策内核）

分支 `p0/release-scaling-min`。参考项目：ChiRi 千漓调度（`github.com/Aibeto/ChiRiScene`）。

### 先纠正一个错误结论（避免以后重提）

计划里原本把「释放 scaling_min」列为第一优先级，依据是 `AutoFsm::Caps` 有 `minL/minB` 字段。
**核查后发现 Kin 早就做到了**：`writeFsmCaps` 传空下限（`FreqWriter(pol, string_t(), bmx, gov)`，
注释写明「空下限 = 不写 scaling_min」），auto 模式入口 `applyCurrentMode` 不走 `applyWithProfile`
而是 `primeAutoFsm()`，后者显式把 min 写成 `0`（hw-min）。所以这一条**无需改动**。
遗留的小问题：auto 路径上 `capsToWrite.minL/minB` 是死值（算出来、经 PolicyResolver 搬运、
最终不落盘），读代码时容易误以为 min 在受控。未改（不落盘是设计），仅记录。

### 真正缺的是闭环

`computeCaps` 是 `state + alpha` 的纯函数，状态不变则 caps 恒定 —— **2 秒 tick 内 scaling_max
是个常数，对负载没有任何反馈**。这才是"开环"的确切含义。

### 已落地（两个提交）

**提交 1 `faa4cd9`：算法层（纯函数，未接线）**
- 新增 `include/LoadGovernor.hpp`：EMA 平滑 / 尖峰抑制 / headroom 过渡带 / 双向死区 /
  升降频确认 tick / 极低负载立即降频 / 滞回带慢升 / normalize 自愈。零 IO，主机可测。
- 缺省参数 = ChiRi 在 SM8550 真机调过的那一组。
- 新增 `tests/test_load_governor.cpp`（CTest `kin_load_governor`），14 组用例。
- 变异验证 5 项全部被捕获：死区 gate 单侧化 / 去掉首 tick 豁免 / fastDown 判据 `<=`→`<` /
  升频窗口 off-by-one / `perfCeil` 默认改 0.80。
- **测试抓到一个真 bug**：首 tick 没有历史却套用了尖峰抑制（`lastUtil=0`，刚接管遇高负载
  被判成巨大跳升 → 抑制到 0.14 → 比 `perfInit=0.45` 还低 → 决策成了**降频**）。修法 = 首 tick
  豁免尖峰抑制（没有历史就没有尖峰可言），并加 `testFirstTickNoSuppression` 锁死。

**提交 2：接线（方案 A = 缩短 FSM tick + 闭环作为"只能收紧"的一层）**
- `AutoMode::Metrics` 新增 `clusterBusy[4]`（每簇**最忙核** busy%，-1=读不到），由
  `readWeightedBusy` 的逐核 `/proc/stat` 增量顺带算出 —— **不新增采样、不新增 fd**。
  取簇内 max 而非均值：1 线程跑在 6 核簇上是「1 核忙 5 核闲」，均值会抹成 ~17% 误判成轻载。
- `scheduler.hpp`：`applyLoadGovernor()` 插在 **用户 fsm_policy 覆盖之后、FAS/类别/热之前**。
  fsm_policy 是用户配的**预算**，闭环只能在预算内收紧；后续每一层仍可继续取 min。
  闭环输出与预算取 `min`，**永不抬高** → 关掉即逐位回退。
- 首次从"当前预算"起步（`loadGovPrimed_`），不从固定 `perfInit`，否则第一拍会把预算当跳变。
- 复位点：退出 auto、息屏→亮屏沿（否则带着 DEEP_IDLE 时的低 currentPerf 醒来，要爬好几拍）。
- 配置：`loadgov_enable` / `loadgov_tick_ms`(600) / `loadgov_up_pct` / `loadgov_down_pct` /
  `loadgov_headroom_pct` / `loadgov_smooth_up_pct` / `loadgov_deadzone_pct` / `loadgov_dwell_ms`。

### 关键设计：FSM 节奏与闭环解耦（否则会引入新 bug）

`AutoFsm` 的确认窗口是**按 tick 数**标定的：`UP_WINDOW=2`（注释口径 ×2s=4s）、
`DOWN_WINDOW=5`（×2s=10s）、`kPairConfirmWindow=3`。tick 缩到 600ms 后若仍每拍推进状态机，
等价于把升档压到 1.2s、降档压到 3s —— 状态机会明显 churn（上游现场日志已证 MODERATE↔HEAVY
来回本身就是主要抖动源）。故新增 `fsmDue` 抽帧：**状态机 + 会话记账 + status 文件仍按
`kFsmTickMs`(2s)，只有采样 + 闭环 + 写频跑 600ms 快拍**。最小驻留是墙钟秒，不受影响。

### 未验证 / 不能吹的

- **未在真机跑过**。Windows 主机 CTest 23/23 通过（排除 3 项既有失败：`kin_node_writer` /
  `kin_node_value` 超时、`kin_gamelist` 读不到文件 —— 已用 `git stash` 在干净树上复跑确认
  同样失败，与本改动无关）；Android arm64 编译链接通过（8 个既有 shadow 警告，无新增）。
- **tick 从 2s 降到 600ms 的实际功耗影响未测**。ChiRi 的教训是"有副作用的读不能挂高频采样"
  （每秒读 Adreno gpuclk 会让 playback 从 1.76W 涨到 2.59W）。已核过 `AutoMode::sample()`
  全是缓存 fd 上的 `pread`、无 popen，但**没有实测唤醒数与功耗**。
- **闭环收益（省多少电、掉帧有没有变差）未测**。可观测口径已加：Debug 日志每 FSM 拍打一行
  「负载闭环: 小核 busy=?% perf=? 上限=?MHz | 大核 ...」。判读：perf 长期贴 1.0 而 busy 不高
  → headroom/门槛没起作用；perf 恒定不动 → 撞上死区或 FSM 预算。
- `loadgov_dwell_ms` 缺省 **0（关闭）**：Kin 的确认窗口（up 3 tick / down 2 tick @600ms
  = 1.8s/1.2s）本身已是驻留，再加一层会把周期翻倍并让状态机与 sysfs 失步。

### 回退

- `loadgov_enable: false` → 逐位回到原行为（闭环只读不写，且 tick 回旧口径 4s/2s）。
- 单项调参：`loadgov_up_pct`（升频门槛，调高更省电/更不跟手）、`loadgov_smooth_up_pct`
  （升频步长）、`loadgov_deadzone_pct`（写频死区）。
- 整层回退 = 删 `applyLoadGovernor` 调用 + `LoadGovernor.hpp`（提交 2 的 diff 是独立的）。

### 下一步

1. 真机长时观察：Debug 日志的 perf/busy 关系、`WriteStatusTracker` 的写频次数/分钟。
2. 若功耗反升 → 先查 `loadgov_tick_ms` 从 600 放到 1000，再查 `sample()` 里哪一项有副作用。
3. 之后才是 P1-1（FAS 逐帧 + jank + loading）、P1-2（稳态下探 T11）、P2-1（帕累托甜点封顶）。

## 最新一轮：恢复路径审计 + 修两个真实缺陷

处理交接清单 item 4（“退出恢复可能清空 held 标记，需故障注入验证；uclamp 单位需分别核对”）。

### 先确认：共享原语已经是干净的

`OwnedNode` / `OwnedSettings` / `releaseOwned` 的故障注入测试在早前几轮就存在（`test_owned_node` / `test_owned_settings` / `test_restore_policy`），覆盖：还原写入失败 → 仍持有、读不到 → 仍持有、外部改写 → 释放且不覆写、部分还原 + `retryRestore`。本轮重跑全部通过，**没有发现新问题**。

### 缺陷 1（真实 bug）：功率 boost 的探测节点 ≠ 写入节点

- 原 `pwrBoostProbe()`：cgroup `cpu.uclamp.min` 存在 → 模式 1；否则 `sched_util_clamp_min` 存在 → 模式 2；否则 `sched_boost` 存在 → 模式 3。
- 但 `pwrBoostMainNode()` 对模式 2 和 3 **都返回 `sched_boost`**。于是在“有 `sched_util_clamp_min`、没有 `sched_boost`”的内核上：选中一个写不进去的模式 → boost 静默失效；而且写失败时 `pwrBoostState_` 不落定 → **每个 tick 重试一次写不存在的节点**。
- 修正：新增纯函数 `include/PowerBoostNode.hpp::selectPowerBoostMode(cgroupExists, schedBoostExists)`，**只以实际写入节点是否存在为准**；模式 3 不再产生（它与模式 2 目标相同，只会让日志更难解释）；日志文案由“sysctl”改为“sched_boost”。
- `sched_util_clamp_min` 常量保留仅作说明（它是 UCLAMP_MIN 的“允许上限”，不是 boost 源）。

### 缺陷 2：top-app uclamp 可能在“无法还原”的前提下接管

- 原 `applyTopAppUclampMin()` 在探测时快照原值，但**原值为空也照常写入**；退出时 `uclampOrig_.empty()` 被当作“已还原”直接释放所有权 → **我们的 boost 值被永久留在节点上**（重启或别的写入者才会改变）。
- 修正：接管前强制要求原值可读，读不到本拍不动、下拍重新快照（与相机保持的既有规则“读不到原值则跳过接管”一致）。`uclampOrig_` 于是不可能为空时仍持有，还原表达式里的 `uclampOrig_.empty()` 例外也随之去掉。
- 新增的 `applyDeepIdleBgUclamp()`（上一轮）本来就是这个语义：`rd()` 为空 → `currentPct=-1` → 不写。

### uclamp 单位核对结果

- cgroup `cpu.uclamp.min/max`：**百分比 0–100**。`applyTopAppUclampMin` 直接写 pct；`applyPowerBoost` 模式 1 把 `power_boost_value`(0–1024) 按 `×100/1024` 换算后写——正确。
- `/proc/sys/kernel/sched_boost`：**0/1**，模式 2 写 `wantOn?1:0`——正确。
- `/proc/sys/kernel/sched_util_clamp_min`：**0–1024**，Kin 只探测不写入（非 boost 源）——无错用。
- 未发现“把百分比当 0–1024 写”或反向的跨用。

### 验证

- 新增 `tests/test_power_boost_node.cpp`（CTest `kin_power_boost_node`）：四种存在组合 + 返回值域回归护栏。
- **变异验证**：把选型改回旧语义（有 cgroup 就 1，否则一律 2）→ `FAIL neither present: got 2 want 0`（已恢复）。
- 结果：Windows 主机 CTest **24/24 通过**，Android arm64 daemon 编译链接成功（8 个既有 shadow 警告，无新增），`git diff --check` 通过。
- 过程记录：本轮改 `scheduler.hpp` 的日志行时多写了一个 `)`，**主机测试完全没发现**（`scheduler.hpp` 不在主机测试编译范围内），是 Android 构建报的错。这就是为什么每轮都必须跑 Android 编译，不能只跑主机 CTest。

## 前轮：上游 V64 智能模式剩余两项（背景 uclamp + 争议传感器）

至此 `asb_fsm.h` / `asb_smart.h` 里可移植的智能模式改动已**全部落地**。

### 1. 息屏 DEEP_IDLE 背景 uclamp 上限（上游实测 140 mA → 50 mA）

- **新建通道**：Kin 原来只有 top-app `uclamp.min`（性能下限）与相机 foreground `uclamp.max`，**没有 background 通道**。新增 `applyDeepIdleBgUclamp()`，目标节点按 cgroup v1/v2 两套候选探测 `background` / `system-background` 的 `cpu.uclamp.max`，全探不到就整段跳过（不写不存在的节点）。
- **只压不抬**：决策抽成纯函数 `include/IdleUclamp.hpp::deepIdleBgUclampTarget`——已低于 cap、读不到当前值、开关关闭、cap 越界都返回“不动”。
- **原值用 `OwnedNode` 记**（与 SysTweak/pwrBoost 同款）：离开 DEEP_IDLE、关闭开关、退出 auto 三条路径都条件还原；节点被外部改过就不覆盖。
- **18 不是 0**：空 uclamp 等于告诉调度器“这个任务不需要 CPU”，工作会直接停（上游在 profile 轨道上踩过）。
- **新配置键**：`bg_uclamp_enable`（默认开）+ `bg_uclamp_cap`（默认 18，0=不压）。真机上若发现息屏后大文件下载/备份变慢，设 false 即立即还原。

### 2. 争议热源多一拍确认（上游 `source_confidence == 1`）

- 上游用 0/1/2 级置信度；Kin 没有这个字段，所以用**它自己的热决策暴露的矛盾**来定义争议：皮肤锚定决策温认为还没到 veto，而结温已到/超过 `junction_hard_temp` 把决策温硬抬到 veto。新函数 `thermalSourcesDisputed()` 与该硬顶分支严格一致（单测里有交叉验证循环）。
- **单一热源（没有外壳传感器 / 皮肤闸门关闭 / 皮肤读数是垃圾值）不算争议** —— 那是缺失不是矛盾，上游同样不对缺失加罚，否则没有皮肤传感器的设备会平白多一拍延迟。
- 负载驱动的迁移 +1 拍；**热升档 `thermalToSustained` 仍然不受影响**。

### 验证

- 新增三个主机单测：`tests/test_idle_uclamp.cpp`（CTest `kin_idle_uclamp`）、`tests/test_thermal_decision.cpp`（`kin_thermal_decision`）、`test_autofsm` 扩两个用例（争议 +1 拍、热升档不被拖慢）。
- **变异验证（4 次，均已恢复）**：
  - 去掉争议 +1 → `kin_autofsm` 返回 9（MODERATE 只用 3 拍）
  - 让争议也作用到热升档 → `FAIL thermal escalation slowed by dispute: 4`
  - 去掉“皮肤侧未到 veto”条件 → `FAIL both hot: got 1 want 0`
  - `currentPct <= capPct` 改成 `<` → `FAIL already at cap: got 18 want -1`
- 结果：Windows 主机 CTest **23/23 通过**，Android arm64 daemon 编译链接成功（仍只有 8 个既有 shadow 警告，无新增），`git diff --check` 通过。

### 必须说清楚：这两项都**未在实机验证**

- 背景 uclamp 的收益（140→50 mA）是**上游在它自己的参考机上测的**，Kin 这里只证明了逻辑正确 + 节点存在才写 + 条件还原。**真实节电量、以及对息屏后台任务（同步/下载/备份）的实际影响未测**。这是本轮唯一一个“默认开启但未经真机标定”的写入。
- 争议判定的阈值（`junction_hard_temp`）沿用 Kin 现有配置；争议在真实设备上到底多久发生一次、能减多少抖动，**未测量**。

## 前轮：多窗口扫描 dumpsys 按活动度分级节流（每拍开销）

按权重排序取的第三项。auto 模式下唯一剩下的“周期性 popen”就在这里。

- **问题**：`scheduler.hpp` 的多窗口游戏扫描每拍都问一次 ActivityManager（`getForegroundCached(2000)`），而这条 dumpsys 只在小窗/分屏里游戏不是顶层包时才用得上。原实现固定 2s → 亮屏期间常驻约 0.5 次/秒的 `popen`+`fork`，按代码自己的注释（dumpsys 每次 100–300ms）估算，安静亮屏时每小时约 1800 次探测。
- **改法**（上游 V64 同思路：前台探测 8s / 25s / 120s 按活动度分级）：新增纯函数 `include/ProbeCadence.hpp::multiWindowScanTtlMs`，判据只用 Kin 已有遥测，不引入新采样：
  - 已到 HEAVY 及以上，或 GPU ≥50 / CPU ≥50 → **8s**（刚进小窗游戏的最可能窗口）
  - GPU ≥25 / CPU ≥30 → **25s**
  - 其余（含遥测读不到 -1） → **120s**
- **为什么不会变迟钝**：小窗里真在跑的游戏会把 GPU/CPU 顶起来，下一拍（2s）就落回 8s 档；只有“确实什么都不动”时才会降到 120s，而那时也没有游戏可找。遥测读不到时按安静档处理（省电优先）。
- **与手动档无冲突**：TTL 只决定“多久之内不重探”，`getForegroundCached` 本身有 2s 重试地板与并发去重；手动档路径请求 0/2500ms 时仍会在缓存过期后立即重探，不会被 auto 的长 TTL 挡住。
- **新增主机单测** `tests/test_probe_cadence.cpp`（CTest `kin_probe_cadence`）：三档边界（24/25、29/30、49/50）、读不到遥测、HEAVY 优先、全输入域都落在三档内且不低于 2s。断言用**字面值 8000/25000/120000**而不是引用默认值，否则把默认值改回 2000 也能“自洽通过”。
- **变异验证**：`quietMs` 改回 2000 → `FAIL default quiet: got 2000 want 120000`；`>=` 改成 `>` → `FAIL gpu active edge: got 25000 want 8000`。均已恢复。
- 结果：Windows 主机 CTest **21/21 通过**，Android arm64 daemon 编译链接成功（仍只有 8 个既有 shadow 警告），`git diff --check` 通过。

**未验证/不能吹的**：实际唤醒数与功耗降幅**未在实机测量**（本轮只证明了“安静时从每 2s 降到每 120s”这个代码事实，没有测到 mA）。小窗游戏的检测延迟从“最多 2s”变成“最多 8s（有负载时）/120s（完全安静时）”，这是有意的取舍。

## 前轮：热否决按皮肤温加权（上游 V64 智能模式第三项）+ 学习方向可单测

按上一轮列的权重排序取的第二项（收益/风险/可验证性最优：完全可在主机验证，不依赖实机标定）。

- **移植 V64 `asb_smart.h:2337`**：结温触发热否决但**皮肤/外壳仍凉（<40℃）**时，`dt_alpha` 只加 50 而非 100。依据：皮肤凉说明散热吃住了、用户没感觉，也还远于 OEM 自己的 43–45℃ 皮肤上限，此时按满推力压频是在裁一个本来扛住了的手机。**皮肤真热、或设备没有物理外壳传感器（`max_skin_c` 未知/为 0）时保持满推力，与改动前完全一致。**
- **新增会话皮肤峰温** `sessMaxSkinC_`（Kin 原来只有 `m.skinC` 瞬时值，没有会话级峰值；上游用的是 `max_skin_c`）。无外壳传感器时恒 -1。
- **抽出纯函数 `include/SmartLearning.hpp`**（`kin::smartDtAlpha`）：这段权重决定 alpha 的长期漂移方向，学反了就是“越热越放开”（上游 V64 就修过一次并因此重置了学习模型），而它原来内联在 `commitSession` 里只能靠读代码确认。现在输入只有会话统计量、输出只有 dt_alpha，逐条可断言。`AutoMode::commitSession` 改为调它，行为与改动前等价（除新增的皮肤加权）。
- **新增配置键** `dt_thermal_hit_skin_cool`（默认 50，与其它 dt_* 一致可配）：设成与 `dt_thermal_hit` 相等即完全退回旧行为，用户/机型可退。
- **新增主机单测** `tests/test_smart_learning.cpp`（CTest `kin_smart_learning`）：方向护栏（任何热相关组合不得产出负值）、信号叠加、极热不与热叠加、无传感器/-1/0 三种“无皮肤”语义、皮肤 39/40℃ 边界、配置退出口。
- **变异验证**：把 `thermalHitSkinCool` 改回 100 → `FAIL thermal hit, cool skin: got 100 want 50`；把 `maxSkinC > 0` 改成 `>= 0` → `FAIL thermal hit, zero skin reads as absent: got 50 want 100`。两次变异均已恢复。
- 结果：Windows 主机 CTest **20/20 通过**，Android arm64 daemon 编译链接成功（仍只有 8 个既有 shadow 警告），`git diff --check` 通过。

至此上游 V64 智能模式可安全移植的三项（GAMING 包名校验、相邻档位确认窗口、皮肤温加权）**已全部落地**；剩下的是需要实机标定或架构前提的两项，见下方“未移植”清单。

## 前轮：同步上游 ASB V64 智能模式 + AutoFsm 首个主机单测

上游核对基准：`github.com/krilikd-factory/AutoSystemBoost` 当前 `module.prop` = V65/650，`CHANGELOG.md` = V64。

- **频率界已对齐**：逐行核对 `src/asb_fsm_bounds.generated.h` 的 12 个界（battery/balanced 的 floor/ceil × 小核/大核）与 `AutoFsm.hpp` 常量，**完全一致**，本轮无需改动。
- **移植 V64「GAMING 需要包名表同意」**（上游 `asb_fsm.h:1204` pkg_agrees）：新增 `fgNotGame` 入参。前台包名已被明确分类为**非游戏**（视频/社交/地图/音频…）时，GPU 单指标不再把状态推到 GAMING，改按 CPU 在做什么评级（GPU 重载仍到 HEAVY，但要 CPU 一起忙）。上游证据：无游戏运行的捕获里出现过 GPU 100% + load 1.3 的 GAMING 样本；信息流自动播放视频同样能打满 GPU，而 GAMING 是整条阶梯里护栏最松的一档。
- **比上游更保守的一处**：上游判据是 `app_hint == 0 || hint >= GAMING`，即包名表不认识的 App 一律放行。Kin 保持同样语义但用「类别已分类且不是游戏」：`fgCat >= 0 && !gameHint`。**类别未知(-1)时保留旧行为**，未被画像/系统类别覆盖的第三方游戏不会被误降档。
- **移植 V64「相邻档位对多要一拍确认」**（上游 `asb_fsm.h:2025`）：MODERATE↔HEAVY、MODERATE↔SUSTAINED 的最少确认拍数提到 3（`kPairConfirmWindow`）。上游现场数据：一次会话 20 次切换里 14 次是 MODERATE↔HEAVY 来回，而 CPU 负载全程只有 8.9%（GPU 在 0~61% 摆动）；每次切换≈4.5 次 sysfs 写。**热限升档 `thermalToSustained` 豁免**，真烫仍按原节奏降档。
- **scheduler 接线**：`fgCat`（`resolveCategory`）提前到 FSM 更新之前计算，供 GAMING 校验与下方类别钳制共用同一份缓存结果，不增 popen。
- **新增 AutoFsm 主机单测**（`tests/test_autofsm.cpp`，CTest `kin_autofsm`）：这是 AutoFsm 第一次有可执行测试。此前它无法在 Windows 主机编译（经 `Logger.hpp → Utils.hpp` 依赖 POSIX `O_NONBLOCK`/`pread`/`localtime_r`/两参 `mkdir`）。用**测试专用垫片** `tests/host_posix_shim.hpp` 解决，生产代码零改动。
- **变异验证（不是只看通过）**：把 `kPairConfirmWindow` 改回 2 → 用例 2 报 `FAIL pair confirm window: 3`；把 `!fgNotGame` 去掉 → 用例 3 报 `FAIL gated gaming: entered GAMING on GPU alone`。两次变异都恢复原状。
- 结果：Windows 主机 CTest **19/19 通过**，Android arm64 daemon 编译链接成功（仍只有 8 个既有 shadow 警告），`git diff --check` 通过。

### 本轮**未**移植的上游项（已核对，需设备或架构前提）

1. ~~皮肤温感知的 thermal_hit 权重~~ **已移植（本轮）**：见上方最新一轮记录。
2. **DEEP_IDLE 背景 uclamp 封顶 18%**（上游实测 140→50 mA）。Kin **完全没有 background uclamp 通道**（只有 top-app uclamp.min 与相机 foreground uclamp.max），也没有 per-profile 背景基线值。照搬等于凭空发明一个会影响后台同步/音频的值，**必须实机标定**，不做无验证写入。
3. **争议传感器的 +1 拍确认**：Kin 无 `source_confidence` 概念（上游的 0/1/2 级置信）。要做得先给 Kin 的热决策源加置信度。
4. **thermal lean 反向 bug**：核对 Kin `dt_hot=+40 / dt_vhot=+80 / dt_clean_cool=-60` 与 alpha 语义（alpha↑=更省电→上限更低），**方向正确，Kin 没有这个 bug**，无需移植。
5. **息屏 swappiness 改按内存 PSI 门控**：属 SysTweaks 范畴，与智能模式无直接关系，单独排期。

## 前轮：逐节点写入结果

- `WriteStatusTracker` 增加批内逐节点结果：`recordNode(path, status)` 按 written/unchanged/failed/unknown 计数，保留最多 4 个失败节点路径（超出计入 `dropped`），路径截断到 95 字节并过滤为路径安全字符。旧的 `record(status)` 不参与节点统计，既有聚合语义（failed 优先 → written → 全 unchanged）不变。
- `Utils::FileWriteIfChangedBlocking` 改走 `recordNode`，因此 CPU scaling_min/max、cpuset、uclamp、sched sysctl 等所有经该函数的节点自动获得逐节点结果；`FileWriteBlocking`、触控、初始化路径不纳入。
- 状态协议新增 `last_write_nodes` 与 `last_write_failed`（仅在批内有节点记录时输出，风驰接管不输出），使"整批同值跳过"与"部分节点写失败"可区分。默认参数下 `policyStatusFields` 输出字节不变。
- 测试：`test_write_status` 扩展到无路径记录不入账、计数摘要、失败路径过滤/截断、换批清空、跨线程不污染、`dropped` 上限；`test_policy_status` 覆盖明细追加与风驰不输出。Windows 主机 CTest **18/18 通过**，Android arm64 daemon 编译链接成功（8 个既有 shadow 警告），`git diff --check` 通过。
- 未完成/未验证：`test_syswrite_existing` 已补逐节点断言，但本机无 Linux 主机也未装 WSL，**该测试本轮未运行**（Windows 上 `Utils.hpp` 因缺 `O_NONBLOCK` 无法编译，与既有结论一致）；写后回读仍不等同内核接受确认；GPU 原子通道、风驰、boost 同拍结果仍未纳入同一批；`last_write_nodes` 只是 writer 结果，不是硬件回读证据。

## 前轮：节点写入验证原语

- 新增并测试 `NodeWriter.hpp`：区分 Written/Unchanged/Invalid/OpenFailed/ReadFailed/TooLong/WriteFailed/Mismatch，支持 existing-only 写入、可选写后回读；不自动接入 write-only 节点，避免改变 GPU/触发型节点语义。
- 新增 `test_node_writer.cpp`，覆盖缺失、同值跳过、变化写入和回读；主机 CTest **14/14 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- NodeWriter 当前是验证原语，生产路径仍由 Utils/Function 逐步迁移；下一步应按节点能力把 CPU/cpuset/uclamp 纳入并记录每节点结果，不能一次性替换所有 FileWrite。

## 前轮：策略条目校验

- 新增 `PolicyEntry`/`validatePolicyEntry`，统一校验 App policy 与 FSM policy 的 maxL/maxB/gpu 范围；非法值分别回退到不限制/未设置，cores 只接受明确 `0`，并强制 cpu0 在线。
- JsonConfig 的两类策略表实际调用同一校验器；测试覆盖非法百分比、GPU、cores 和 FSM/App 不同回退语义。
- 主机 CTest **12/12 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- 仍缺各条目间的 min/max 依赖、GPU 与 CPU 联合约束、校验失败原因诊断；真实策略 resolver 的每簇输出与 App policy cores 接管仍待实机验证。

## 前轮：完整基础参数校验

- ConfigValidation 扩展到夜间小时、tick 周期、学习率、掉电阈值和 warm/cool/hot/vhot/veto 温度关系。
- JsonConfig 统一回写这些参数；测试覆盖越界回退、依赖关系和合法值保持。
- 主机 CTest **12/12 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- 仍未覆盖 app_policy/fsm_policy 每个条目的语义依赖、conf/MAD/obs 参数、quiet-night 边界和配置错误原因诊断；不等于全部配置已完成校验。

## 前轮：功率阈值配置校验

- 配置校验扩展到 float 功率阈值与依赖：普通 throttle 必须 >0，critical 必须严格高于普通阈值且不超过 30W，boost jank load 限制在 0..32。
- JsonConfig 解析后统一回写校验结果；测试覆盖非法 float、临界阈值自动回退和合法值保持。
- 主机 CTest **12/12 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- 尚未覆盖夜间/tick/学习系数、FAS/FSM policy 每项依赖、校验失败原因诊断和实机功率标定。

## 前轮：扩展配置统一校验

- `ConfigValidation.hpp` 扩展覆盖 camera_hold 百分比、game uclamp、power boost 值/PSI/busy/cap、high/critical throttle 百分比、FAS 目标帧率。
- `JsonConfig` 解析后统一调用同一校验器并回写这些字段，避免各段独立校验产生不一致。
- 测试覆盖越界回退、依赖关系和合法值保持；主机 CTest **12/12 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- 尚未纳入 float 功率阈值、夜间/tick/学习参数、FSM policy/app policy 每项依赖和校验失败原因诊断；不能宣称配置校验全部完成。

## 前轮：配置统一校验

- 新增 `ConfigValidation.hpp`，统一校验低电阈值依赖、FSM 热进出依赖、anti-clamp soft/hard 依赖、attack_temp 和 explore/reward 范围。
- `JsonConfig::parseAutoMode` 实际调用校验器，合法配置行为不变，非法配置回退安全默认/相关默认。
- 测试先失败（非法 thermal enter 的 exit 期望未随回退同步），修正后通过；主机 CTest **12/12 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- 尚未覆盖所有 AutoMode 参数（FAS/FSM policy/boost/camera 等），也未将校验失败原因写入状态诊断；本轮不是完整配置审计。

## 前轮：策略输入边界

- 审计发现 `PolicyResolver::appPolicy` 可能接受负 max/异常 min；新增归一化：无效 max 回退本 tick FSM base，负 min 回退 base，并重新保证 min<=max。
- 测试先失败 exit=14，修复后通过；主机 CTest **11/11 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- 尚缺统一 Config 校验（thermal/FSM/FAS/anti-clamp 参数依赖）、真实低电 cap 在最终频率合并中的安全覆盖；本轮只修 resolver 入参。

## 前轮：/proc 存活扫描节流

- 焦点兜底使用的 `lastGoodFgPkg_` 存活检查增加 2s 缓存，失败路径不再每个 FSM tick 扫整棵 `/proc`。
- 新增通用 `PresenceCache.hpp` 和 TTL/key 切换测试；当前粘滞游戏路径已有局部 2s 节流，后续可迁移到 helper，配置重载的低频检查仍保留。
- 主机 CTest **10/10 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- `findGamePid` 仍只在 FAS 进场/换游戏调用；完整真实 `/proc` 计数和实机唤醒功耗未测。

## 前轮：前台来源诊断

- AutoMode 状态追加 `foreground_source`；scheduler 在实际返回路径标记 accessibility/accessibility_blacklist/top_app_cpuset/window_focus/activity_task/window_fallback 等来源。
- 不改变前台选择顺序；仅补充诊断。推送黑名单显式标为 accessibility_blacklist，避免 App 误以为没有来源。
- 主机 CTest **9/9 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- 仍需测试 getForegroundPkg 端到端来源、粘滞包与真实前台分离；write result 仍非每节点聚合。

## 前轮：GPU 读后写去重

- `Function::writeSysfs` 对 GPU 频率节点先尝试精确读当前值；明确相等才跳过，不可读/读失败仍写，兼容 write-only sysfs。
- 比较复用 `NodeValue.hpp`，不会再因固定前缀截断误判；写入值仍由原有频率 snap/范围校验决定。
- 主机 CTest **9/9 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- GPU 写入结果尚未纳入 WriteStatus batch；GPU 实际路径/节点能力需实机或 Linux sysfs 测试验证。

## 前轮：批量写入结果

- `WriteStatusTracker` 增加 batch 聚合；`writeFsmCaps()` 在 CPU 簇写入前后开启/结束 batch，状态文件读取 `writeBatchStatus()`，不再把最后一次单节点结果冒充整批结果。
- 聚合规则：failed 优先，其次 written，全部 unchanged 才 unchanged；未知/无节点不会被当成功。仍只覆盖 FileWriteIfChangedBlocking 的节点，触控/初始化/直接 FileWriteBlocking 路径不自动纳入。
- 主机 CTest **9/9 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- 已知限制：批次只覆盖 FSM scaling writes；Fengchi、GPU 原子通道、uclamp/boost 同拍结果尚未统一纳入；写成功仍不是内核接受确认。

## 前轮：写入结果诊断

- 新增 `WriteStatus.hpp`：`unknown/written/unchanged/failed`；`Utils::FileWriteIfChangedBlocking` 实际记录比较和写入结果。
- AutoMode 状态追加 `write_verification`，scheduler 在 CPU caps 写入后同步；此字段只说明 daemon 写路径结果，不说明内核最终接受。
- 主机 CTest **9/9 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- tracker 当前是 Utils 实例的最后一次变化检测结果；直接 `FileWriteBlocking` 路径未自动纳入，尚缺每节点/每簇聚合与写后回读。

## 前轮：热状态诊断

- 新增 `ThermalStatus.hpp` 纯诊断分类：hard_clamp/critical/hot/warm_skin/vendor_limited/normal/unknown；只读取已有决策输入，不改变 AutoFsm 行为。
- AutoMode 状态文件追加 `thermal_state`，与既有 battery_state、policy_reason 并列；协议明确为诊断而非硬件确认。
- 测试覆盖 hard clamp 优先、热/皮肤/厂商限制/未知；主机 CTest **9/9 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- 仍需实现 `foreground_source`、`last_write_result`、真实写入失败归类及恢复；GPU/fengchi/blacklist 仍不是统一 resolver 的完整来源。

## 前轮：真实低电迟滞

- 抽取 `BatteryPolicy.hpp`，AutoMode::alphaFor 实际调用该类维护迟滞/800-900 alpha 地板，不再仅测试模拟 cap。
- 修复 charging=true 但 capacity=-1 时旧逻辑提前返回、低电迟滞不退出；拒绝 >100 异常容量解除低电锁。保留正常 low_batt 进入 / 40% 退出行为；开关关闭只停覆盖，迟滞仍更新。
- 状态追加 battery_state（disabled/charging/normal/low/critical/unknown/low_held_unknown）。测试覆盖阈值、迟滞、充电未知容量、异常容量、开关、状态 token；旧逻辑先失败 exit=4，修正后通过。
- 主机 CTest **8/8 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。
- 未实现：低电 alpha 对应上限在用户 FSM 百分比覆盖/boost/风驰之后的硬性优先级。不要把本轮状态机测试说成完整硬件安全覆盖验证。

## 前轮：CPU 策略诊断

- `PolicyStatus.hpp` 通过 AutoMode::writeFsmStatus 实际追加 resolved_policy/policy_reason/policy_scope=cpu/fas_state/write_verification=unverified，保留旧字段。风驰分支显示 rom_managed/fengchi_takeover，而非错误标记普通 FSM cap。
- 对输出 token 作长度/字符验证；未验证硬件写入，不输出成功断言。
- resolver 对候选已含 cap（上限相等）也保留来源，回归先失败 exit=13、修复后通过。候选 reason 不再仅根据 fgCat 非空就标成 profile。
- policy_status 测试覆盖普通/风驰/FAS 状态及换行注入；主机 CTest **7/7 通过**，Android daemon 编译链接成功（8 个现有 shadow 警告），diff 检查通过。
- 仍需区分真实前台与粘滞游戏包、输出 foreground_source；旧 cpu_max 单位文档与实际调用 /1000 不一致，风驰分支旧 caps 值不代表实际硬件目标。这些未在本轮静默改变协议。
- thermal_state 后续已接入；last_write_result 仍未实现；状态文件端到端与实机验证未完成。

## 前轮：普通 CPU cap 来源接入

- resolver 拆分 thermal/battery/power/app/camera/fas/other 槽，避免 App 和 FAS 限制不同簇时覆盖彼此的原因。诊断并列优先级按上述顺序，数值始终取最小。并非完整 ROM/黑名单策略优先级。
- scheduler 登记 FAS、类别、相机实际收紧值和高/临界功率 cap。末端候选可 boost，但不能再放开已登记限制。这是行为修复，需要实机检查帧率/响应性。
- 新增跨来源顺序/归因回归测试：旧共享槽失败 exit=11，修正后通过。主机 CTest 6/6、Android daemon 链接、diff 检查均通过；8 个既有 shadow 警告。
- 后续已修复 cap 与候选相等时的归因，并接入状态字段；多个同类型提交仍共用一个元数据槽，尚不是完整的每簇原因解释。
- 尚未将真实低电状态机转换为显式 cap，风驰/黑名单/GPU 仍走既有分支。不要宣称全部策略优先级已完成。

## 前轮：Activity Task 探测节流

- `ForegroundCache.hpp` 从 Utils 提取实际 Activity 快照缓存。每实例只允许一个探测，其他并发调用立即返回 invalid，不排队等待慢 dumpsys；失败也缓存，2s 后才重试；TTL 从探测完成计时。
- fresh/ttl=0 不再绕过 Activity 2s 下限；无障碍推送不经此缓存，响应不受影响。无推送时事件响应可能增加最多约 2s 延迟。
- `test_foreground_cache.cpp` 可控时钟验证失败节流/恢复、成功 TTL、慢探测计时，线程 gate 验证同实例并发只执行一个 probe。旧算法先失败 exit=1，修复后通过。
- 本轮主机 CMake/CTest **6/6 通过**，Android daemon 编译链接成功（8 个既有 shadow 警告），diff 检查通过。测试没有执行实际 dumpsys，未测实机功耗。
- 仍未限频：独立窗口焦点回退、getTopAppCached 的并发空结果路径、部分 /proc 扫描；多窗口发现逻辑未改。

## 前轮：节点值比较

- 新增 `include/NodeValue.hpp`，被 `Utils::FileWriteIfChangedBlocking` 实际调用。逐段读取到 EOF，处理 EINTR；最大 4096 字节，超过限制返回 TooLong，不能把相同前缀误认成相等。仅修剪行尾空白。
- 新增真实临时文件测试 `tests/test_node_value.cpp`，覆盖不存在、空值、外部漂移、行尾/行首空白、255 字节前缀、4096/4097 边界。保留旧算法时测试失败 exit=3；修复后通过。
- 同步 ASCII 副本后 CMake/CTest **5/5 通过**；原仓库 Android NDK daemon 编译链接通过，8 个既有 shadow 警告；diff 检查通过。
- 这只验证读取比较，不等于完整 sysfs 写入/恢复、并发所有权或 Android 实机验证。读取失败仍返回 false，写后验证与失败诊断尚未实现。

## 前轮实际改动

- PolicyResolver 不再把初始 FSM 上限初始化为安全 cap，避免无安全限制时误挡功率 boost。
- cap 与普通候选分开保存；普通候选更新不清除已登记限制。
- 最终诊断选择实际限制某簇的 cap，跨类型并列优先 thermal、battery、普通 cap。同一普通 cap 槽仍只保留聚合上限/最后收紧的元数据，尚不是完整策略优先级模型。
- scheduler 的 joint-DVFS 热限制登记到 resolver，防后续功率 boost 撤销此限制；风驰旁路未改动。
- 新增 boost 误限复现、安全原因保持、调用顺序、非法 cap、逐 tick 重建测试。boost 复现先失败 exit=6，修正后通过。

## 验证证据与工具

- `D:\kin-daemon-verify\build`：同步 include/tests/CMakeLists.txt 后 CMake 构建成功，CTest 4/4 通过。
- 原仓库 `build-android-arm64`：Android NDK 编译链接成功，产物 `bin\Kinetic`；8 个现有 shadow 警告。未安装到设备、未运行。
- `git diff --check` 通过。
- Windows CTest 不注册 Linux 专用 syswrite 测试；该测试未运行。
- 实际工具已存在，不再重复安装或声称缺失：
  - `C:\Program Files\CMake\bin\cmake.exe` / `ctest.exe`
  - `D:\msys64\mingw64\bin\g++.exe`（执行前把该 bin 加入 PATH，保留原 PATH）
  - Android 构建缓存已有 NDK `C:\Android\sdk\ndk\28.2.13676358` 和 Ninja `C:\Android\sdk\cmake\3.31.6\bin\ninja.exe`。
- Android 可直接在原中文路径构建。Windows 主机 CMake 使用 ASCII 副本；每轮先同步本轮修改，不用旧副本冒充新验证。
- Git Bash 下不要使用 `cmd /c`（参数转换会导致命令未执行）；可用绝对路径工具或单引号包裹 PowerShell 命令。PATH 用双引号保留 `$PATH` 展开。

## 下一步必须处理

1. 完整策略合并仍未完成：普通 CPU 的 FAS/App 类别/相机/功率/joint-DVFS 热 cap 已登记；未接管风驰、黑名单、真实电量状态机或 GPU 所有优先级。低电测试仅测试合并器数值，不覆盖真实电量状态机。
2. ~~FreqWriter 仍会命中内存缓存提前 return~~ **已过时**：核对 `scheduler.hpp` FreqWriter，缓存短路已移除，每拍都经 `compareNodeValue` 读后比较；`lastMin/Max/Governor` 只在整簇 offline 时清空，仅作外部同步记录、不再当所有权证明。剩下未做的仍是“读后写≠跨模块写入所有权管理”。
3. FileWriteIfChangedBlocking 的截断前缀误判已修复，read EINTR 已处理；逐节点结果已记录（`last_write_nodes`/`last_write_failed`）；写后回读、恢复失败仍需测试。不要机械地把写-only/触发型节点改成 read-before-write。
4. ~~退出恢复路径可能在恢复失败时清空 held 标记~~ **已审计并修正**：共享原语的故障注入测试早前就有且全过；真正的问题在两个调用点——功率 boost 探测节点≠写入节点（已改为纯函数选型）、top-app uclamp 在无法还原时仍接管（已加前置条件）。uclamp 单位（0–100 / 0–1024 / 0–1）已逐个核对，无错用。
5. ~~多窗口周期 dumpsys~~ **已处理**（本轮分级 TTL）。焦点兜底原本就带 5s 硬限频 + CAS 防重入，`getTopAppFast` 带 300ms TTL 且已改按 `cgroup.procs` 枚举，粘滞/`lastGoodFgPkg_` 存活检查带 2s 缓存。仍未做的是**实机唤醒计数与功耗测量**，不能把“少调了 dumpsys”当成“省了多少电”。
6. Linux syswrite 测试、真实调度器集成测试、退出恢复/黑名单/风驰/FAS 互斥测试、统一状态字段、实机功耗/调度验证仍未完成。
7. ~~ASB 智能模式同步剩余两项~~ **已完成**（本轮）：背景 uclamp 通道 + DEEP_IDLE 封顶、争议热源 +1 拍。**但两者都缺实机验证**：背景 uclamp 是本项目目前唯一一个“默认开启、未经真机标定”的写入，需在真机上确认息屏后台任务不受影响、并量出实际 mA 降幅；否则应把 `bg_uclamp_enable` 默认改 false。

每轮应完成可验证行为，不再仅替换几个调用后把静态 grep 当测试通过。任务全部验收后再暂停对应周期任务，未验收不得标记最终完成。
