# Kin Runtime Status Protocol

`AI/auto_status.txt` and `AI/capabilities.txt` are daemon-owned, read-only snapshots for the App.

## File rules

- Files use UTF-8 key/value lines: `key=value`.
- The daemon writes through a same-directory temporary file, completes the write, calls `fsync`, then renames it over the target.
- Readers must tolerate missing files, unknown keys, empty values, and older schema versions.
- A missing capability is reported as unavailable; it must not prevent unrelated scheduling.
- `auto_status.txt` is sampled at approximately 1 Hz and must not be interpreted as a hardware write acknowledgement.

## auto_status keys

Existing keys retain their historical meaning. The App parser accepts the following optional metadata keys; daemon producers should add them only when the corresponding decision value is available:

| Key | Unit | Meaning |
| --- | --- | --- |
| `schema_version` | integer | Status schema version; absent means version 1. Current daemon writes **3** (2 = policy diagnostics, 3 = load governor + FAS frame analysis). Readers must keep tolerating older versions. |
| `seq` | integer | Monotonic snapshot sequence when present |
| `timestamp` | Unix seconds | Snapshot creation time when present |
| `resolved` | string | Resolved mode (`fsm` or a profile) |
| `fsm` | string | Current FSM state |
| `resolved_policy` | token | Final ordinary CPU cap source, or `rom_managed` in the Fengchi branch |
| `policy_reason` | token | Binding cap reason (e.g. `frame_target`, `category`, `joint_dvfs`), candidate reason, or `fengchi_takeover` |
| `policy_scope` | token | `cpu`; these diagnostics do not describe the independent GPU policy |
| `fas_state` | token | `engaged`, `inactive` (including warm-up/unavailable), or `blocked` by Fengchi takeover |
| `write_verification` | token | `daemon_write_result`: describes the daemon writer result; it is not a hardware acceptance/readback assertion |
| `last_write_nodes` | string | Per-node tally of the last FSM CPU write batch (`total=,written=,unchanged=,failed=`, plus `unknown=`/`dropped=` when non-zero). Absent when the batch recorded no node path, or when Fengchi takeover skipped the batch. Counts daemon writer results, not kernel acceptance. |
| `last_write_failed` | string | Comma-separated node paths that failed in that batch, at most 4, sanitized to path-safe characters. Absent when nothing failed. |
| `battery_state` | token | `disabled`, `charging`, `normal`, `low`, `critical`, `unknown`, or `low_held_unknown`; describes the actual low-battery hysteresis used for alpha |
| `foreground_package` / `foreground_category` | string | Daemon's last verified foreground app and category; absent means unavailable |
| `bucket` | integer | Smart bucket 0..11 |
| `daypart` | integer | Smart day-part index |
| `alpha` / `alpha_raw` | x1000 | Smart power-saving tendency and raw bucket value |
| `conf` | x1000 | Smart confidence |
| `cpu_max_little` / `cpu_max_big` | kHz | Current target CPU caps |
| `temp` | Celsius | Decision temperature |
| `headroom` | percent | Vendor thermal headroom; valid only when `headroom_valid=1` |
| `gpu` / `load1` / `cpu_busy` / `cur_ma` / `power_w` | mixed | Runtime telemetry |
| `soft_clamp` / `hard_clamp` / `vendor_clamp` | 0\|1 | Vendor clamp classification; `soft_clamp` is a de-aggression hint, not a hard restriction |
| `vendor_clamp_1h` | count | Vendor-clamp occurrences in the last hour |
| `anticlamp_attack` / `anticlamp_attack_total` | 0\|1 / count | Legacy attack telemetry; currently always `0` because vendor caps are read-only |
| `explore` / `base_cost` / `consistency` | mixed | Smart exploration direction, bucket baseline current, and consistency x1000 (only meaningful when the bucket has ≥3 samples) |
| `foreground_source` | token | Which detector produced `foreground_package` (`accessibility`, `cpuset`, `dumpsys`, `unknown`, …) |
| `thermal_state` | token | `normal`, `warm_skin`, `vendor_limited`, `hot`, `critical`, `hard_clamp`, or `unknown` |

### Schema 3: load governor and FAS frame analysis

These keys describe the **decision target** of the two closed loops added in schema 3. They are
intentionally separate from `cpu_max_little`/`cpu_max_big`, which are the final values handed to
the writer after FAS, app-category, camera, thermal and policy-resolver caps.

| Key | Unit | Meaning |
| --- | --- | --- |
| `loadgov_enable` | 0\|1 | Whether the load-feedback ceiling loop is enabled |
| `loadgov_busy_little` / `loadgov_busy_big` | percent | Busiest-core utilisation of that cluster this sample; `-1` = not available |
| `loadgov_perf_little` / `loadgov_perf_big` | x1000 | Ceiling the loop itself computed as a fraction of that cluster's hardware maximum; `-1` = loop not primed yet |
| `fas_frame_analysis` | 0\|1 | Whether FAS frame-level analysis is enabled |
| `fas_loading` | 0\|1 | Currently inside a loading/transition episode (FAS holds the ceiling low) |
| `fas_post_guard` | frames | Remaining post-loading guard frames during which downgrades are blocked |
| `fas_gear_dwell` | frames | Frames elapsed since the last gear upgrade (downgrade is blocked below the dwell threshold) |
| `fas_frame_source` | token | `per_frame` when per-frame data is available, otherwise `window` |

`loadgov_perf_*` is **not** the written value. When it differs from `cpu_max_*`, a later cap
(FAS, category, camera, thermal, resolver) tightened the result — that difference is expected and
is the reason both are reported. Likewise `fas_post_guard`/`fas_gear_dwell` are controller state,
not proof of a hardware write.

The additional policy keys are schema-2 diagnostics; legacy `resolved=fsm` and `reason=smart_policy` remain unchanged. For registered caps that tie at a final cluster maximum, the summary reason prefers thermal, battery, power, app category, camera, then FAS. A reason can describe a binding cap on either cluster; it is not a full per-cluster explanation or an acknowledgement from the hardware. `last_write_scope` identifies the daemon writer path (for example `fsm_cpu_batch`) and `last_write_result` is `written`, `unchanged`, `failed`, or `unknown`. `last_write_nodes`/`last_write_failed` narrow that single result to the individual nodes the daemon attempted, so a batch that was entirely `unchanged` can be told apart from one that partially failed; they still do not prove the kernel kept the value, and nodes written outside `FileWriteIfChangedBlocking` (trigger writes, GPU atomic channel, uclamp boost) are not counted. Battery state is sourced from the alpha controller's hysteresis; the final resolver also applies an explicit battery cap after FSM overrides/boost.

The App must show `stale` when `timestamp` is older than its freshness threshold and must not label stale data as a successful live write.

## capabilities keys

Capability keys use `<name>_exists=0|1`, with optional `<name>_reason` and resource-specific values such as `_max_khz` and `_min_khz`. The daemon probes nodes read-only. `node_missing`, `permission_denied`, and `read_failed` are informational reasons only.

## Ownership boundary

Kin-Daemon owns policy decisions and the existing resource writers. The App reads these snapshots and submits configuration through its existing transaction path; it does not write hardware nodes directly. A status snapshot describes a decision target, not proof that another kernel/vendor component accepted it.
