# STM32G474 HRTIM：四路 Chirp（500 kHz–1 MHz）、Timer C/D 双路、100 周期干净启停 — 设计 / CubeMX / 初始化 / 中断边界

本文档面向 **RM0440**（STM32G4 HRTIM）与 **STM32CubeG4** 官方例程习惯写法，供本地工程交给 Cursor 做实现与调试。默认器件 **STM32G474**，外设 **HRTIM1**。

---

## 1. 目标与术语（对齐需求）

| 需求 | 建议的工程释义 |
|------|----------------|
| 4 路激励，Chirp，100 个波形 | 典型映射：**HRTIM1 Timer C** 的 **CH1/CH2** + **Timer D** 的 **CH1/CH2** 共四路输出引脚。一次 **thread_pulse** 产生 **恰好 100 个完整 PWM 周期**（每路计数一致，或按你定义的“周期”一致）。 |
| 500 kHz–1 MHz，单调升/降 | 你自行离线计算 **100 组** `PER` / `CMP`（或等价的 Ton/Toff 编码）。硬件上必须满足 RM 的 **最小脉宽 / 最小间隔**（见 §5）。 |
| 干净开头/结尾，少用 ISR 砍波形 | **Repetition 计数器本身不会自动把引脚拉到 idle**；它只产生 **REP 事件**（中断/DMA）。真正“不再输出”仍要靠 **输出级 ODIS**、**计数器停振** 或 **burst idle** 等。目标是：**在周期边界**完成停机，且 **ISR 里不写 CMP/PER**。 |
| Timer C、D 各管两路，担心漂移 | 同一 **HRTIM1** 内 **fHRTIM 同源**；漂移主要来自 **软件更新不同步** 或 **各定时单元使用不同 roll-over 相位**。应用层应选定 **单一时间轴**（强烈推荐 **Master**）作为“第 k 步”的边界。 |
| 同一 Timer 上两路不能同时高 | 这是 **crossbar 时序设计约束**：在 **同一个** `PER` 时间窗内，为 **OUT1/OUT2** 分配 **不重叠** 的 `[set, reset]` 窗口（或互斥脉冲）。 |
| Chirp 流畅，第 k 周期结束立刻进入 k+1 | 使用 **preload（影子寄存器）+ 受控的 update 事件**；大批量改表时用 **MUDIS / TxUDIS** 冻结更新（RM 用例）。 |

---

## 2. RM0440 里必须接受的“硬事实”（条件 / 边界）

### 2.1 Master 没有引脚

Master 只产生 **PER / CMP1..4 / REP / SYNC** 等内部事件，经 **crossbar** 驱动 TIMA..F 的输出逻辑（RM0440 §28.3.5）。四路波形一定落在 **Timer C/D（或你选的 TIMA..F）** 的 **OUT1/OUT2** 上。

### 2.2 Repetition 的行为（计数与“第 100 个”）

RM0440 §28.3.4（连续模式要点）：

- 定时单元使能时，内部 **repetition** 从 `HRTIM_REPxR`（Master 为 `HRTIM_MREPR`）装载。
- **每次**计数器 **roll-over**（连续模式下到 `PER` 后回到 0）或 **reset** 事件，会使 repetition **减 1**（注意单稳/单次等模式的例外描述）。
- `REPxR = 0`：每个周期边界都可产生 REP 相关行为（若使能）。
- `REPxR > 0`：每 **`(REPxR + 1)`** 个周期产生一次 **REP 事件**（中断/DMA 若打开）。

因此：**若要以“第 100 个 roll-over 之后”为界**：写 **`MREP = 99`（或 `REPxR = 99`）** 且在整个 burst 期间 **不改 MREP**，则 **REP 事件在语义上对应“跨 100 个周期的那次边界”**（实现时务必用示波器 + 逻辑分析仪对拍 **MCEN/输出边沿/ISR** 的相位，见 §12）。

**关键边界**：RM 同时指出——若启动瞬间 **计数器初值 > PER**，或运行中把 **PER 改小到小于当前计数** 且未按手册处理，可能导致 **不 roll-over**，从而 **repetition 不递减** → 表现为“永远不到 100”或波形怪异。初始化时必须 **CNT=0**（或安全初值）+ **软件 update** 后再允许输出/计数。

### 2.3 “REP 到了就自动没有波形”在硅片上的真实含义

- **REP 事件 ≠ 硬件自动关断输出**。要 **无多余毛刺地结束**，推荐顺序（与 RM §28.3.14 输出管理一致）：
  1. **在周期边界**（或 RM 允许的同步点）执行 **ODISx**（`HRTIM_ODISR` 里对应通道位），让输出回到 **idle** 定义电平；
  2. 再 **清除 `MCEN` / `TxCEN`** 停计数器；
  3. 关闭 **REP 中断使能**，清标志，避免重复进入。

**为减少 ISR 尾巴造成的风险**（中断响应落在周期中的不确定性）：

- **优先方案**：**REP → DMA** 写 **`ODISR` + 停 `MCR` 使能位** 的 **寄存器序列**（HRTIM 大量事件可触发 DMA；RM 有 DMA burst 章节）。DMA 的 **写序** 与 **HRTIM 总线延迟** 仍须用示波器验证，但通常比“重 ISR + 多寄存器”更可控。
- **折中方案**：REP ISR **极短**：只做 **硬件寄存器写**（1–3 个写），**禁止** `printf`、互斥锁、浮点、复杂分支。

### 2.4 DLL 与高分辨率

DLL 必须在启动波形前 **校准完成**（轮询 `DLLRDY` 或按 ST LL 例程配置连续校准）。DLL 工作频段与 **fHRTIM** 相关；若你改变系统时钟树，应重新验证 HR 分辨率与 **最小脉宽** 余量。

### 2.5 PER / CMP 合法区间（“奇怪杂波”的常见根因）

RM0440 对 **PER/CMP** 相对 **fHRTIM** 有 **最小/最大** 约束（手册表格 **Table 232** 一带；并见 “Null duty cycle exception” 章节）。实现 Cursor 时必须把离线算好的 100 组数 **先夹紧到合法区**，否则可能在 **0% / 100% duty**、**CMP 对齐** 附近出现例外边沿。

**经验法则（调试导向）**：在 500 kHz–1 MHz 量级，若 **CKPSC / HR multiply** 选得过粗，有效 tick 数变少，**Ton** 可能逼近最小脉宽 → 单点失败。CubeMX 里 **Timer clock prescaler** 与 **HR mode** 要和你的表一致。

---

## 3. 架构建议（四路 + 同步 + Chirp）

### 3.1 单一“步进轴”：Master 作为第 k 步边界（强烈推荐）

**思想**：把 **Chirp 的第 k 个 PWM 周期**定义为 **一次 Master roll-over**（或一次 **Master REP 子分频**，但首版建议 **Master PER[k]** 直接等于该步频率）。

- **优点**：Timer C 与 Timer D 的 **输出边沿**都参照 **同一计数器相位**，天然解决你担心的 **C/D 漂移**（仍须保证 **同一 fHRTIM**、**同一 DLL**、**同一更新相位**）。
- **做法概要**：
  1. `MPER` / `MCMPx`（若需要）在 preload 里按表更新；
  2. Timer C/D 的 **SETx/RSTx** 主要选 **Master compare / Master period** 事件，而不是各自自由跑频（除非你明确要四路异步频）。

**若四路每周期频率必须不同**：单一 Master 计数轴不够，需要 **多个计数器** 或 **更复杂的 reset 同步**；这会显著增加调试难度。文档建议：**先统一到 Master 轴**，验证 100 周期与无毛刺，再考虑异步扩展。

### 3.2 “100 个波形”与 MREP 放哪里？

两种常见模式（二选一，不要混用导致计数翻倍）：

| 模式 | 说明 |
|------|------|
| **A. 仅 Master 承载 burst** | `MREP = 99` 固定；Timer C/D `REPxR = 0`（每周期不额外分频）。**REP 中断/DMA 只接 Master**。适合 **Master 定义步进频率** 的架构。 |
| **B. 每定时器各自 100 周期** | Master 可能只作同步；`TIMCREP`、`TIMDREP` 各 = 99。要求 **C/D 的 roll-over 与“步进表”严格对齐**，否则会出现 **第 100 步不同步**。 |

你当前描述更像 **一次 thread_pulse，四路同时开始、同时结束** → **优先模式 A**。

### 3.3 Timer C 与 Timer D：两路同高互斥

在 **每个周期**内，为 **OUT1**、**OUT2** 分配 **不重叠** 的高电平窗口。例（仅说明结构，数值由你表驱动）：

- **OUT1**：`Set on MSTPER`（或 counter reset），`Reset on MCMP1`
- **OUT2**：`Set on MCMP2`，`Reset on MCMP3`
- 约束：`0 < CMP1 < CMP2 < CMP3 < MPER` 且保留 RM 最小间隔。

这样 **同一 Timer 的两路在时间上互斥**。

---

## 4. Chirp “流畅换频”：预载 / 更新门控（RM + ST 习惯）

### 4.1 为什么必须谈 preload

HRTIM 大量寄存器为 **preload + active** 双缓冲。RM0440 **Table 244** 归纳 **Master / TIMA..F** 的 **update 源**（软件 `MSWU` / `TxSWU`、**repetition**、**roll-over**、burst DMA 结束等）。

**Chirp 第 k→k+1 步**的可靠做法：

1. CPU 在 **安全区**（通常在 **周期开始后、下一次 update 之前**）把 **下一行** `PER'/CMP'` 写入 **preload 寄存器**；
2. 选一个 **统一 update 事件**（例如 **Master repetition = 0** 时的 update，或 **roll-over + MREPU**）；
3. 若一次要改 **多个定时器** 的多个寄存器：按 RM 示例，先置位 **`MUDIS` + `TCUDIS` + `TDUDIS`**（名字以 RM 为准）冻结更新 → 写多寄存器 → 再清 `*UDIS`，让 **同一 update** 生效。

### 4.2 `RSYNCU`（从定时器）与来自 Master 的 update

当从定时器选择 **跟随 Master update**（`MSTU` 一类连接，具体位名以 RM/Cube 生成代码为准）时，注意 RM 对 **`RSYNCU`** 的描述：**来自相邻定时器或软件 update** 的请求可 **立即生效** 或 **重同步到 reset/roll-over**（RM0440 §28.3.x 更新传播章节）。Chirp 首版建议：**显式选用“在 roll-over/repetition 边界转移 preload”**，避免在周期中途误更新。

### 4.3 流畅性 vs CPU 负载

- **表很小（100 行）且 CPU 很快**：可在 **周期中断（UPD）**里换行（仍建议 **不写 active，只写 preload**）。
- **追求极致与省 CPU**：**DMA burst** 把 SRAM 表推到 HRTIM preload，并在 burst 末尾产生 **统一 update**（RM DMA burst 章节）。CubeMX 里打开 **HRTIM + DMA** 相关请求，对照 ST 例程 `HRTIM_DMA` 系列。

---

## 5. CubeMX / CubeIDE 配置清单（面向 Cursor 的可执行项）

> 下列为 **配置意图**；不同 CubeMX 版本菜单文案可能略有差异，以生成代码为准。

### 5.1 时钟树

- `SYSCLK` 与 **HRTIM1 kernel clock** 按数据手册允许的最高档配置（常见 170 MHz 档）。
- 记录 **`fHRTIM`** 与 **APB2 定时器时钟倍频**关系，便于把 **500 kHz–1 MHz** 换算成 **MPER 计数**。

### 5.2 HRTIM1

1. **启用 Master**（Master timer）。
2. **启用 Timer C、Timer D**；各 **Output 1/2** 绑定到目标引脚（参考数据手册 AF 表）。
3. **计数模式**：连续 up（首版避免 up-down 增加 crossbar 心智负担）。
4. **Prescaler**：C/D/Master **一致**（首版强烈建议一致）。
5. **Preload（PREEN）**：对将参与 Chirp 的 **`PER`、`CMP1..4`、`SET/RST`（若运行期不改可常量）** 打开 preload；为每个定时器勾选 **合适的 update 源**（见 §4）。
6. **DLL calibration**：按例程选 **Continuous** 或 **单次 + 轮询就绪**。
7. **Output idle 极性**：定义 **RUN/IDLE** 与 fault 行为；与硬件驱动器使能极性一致。
8. **NVIC**：
   - 若用 Master REP：`HRTIM1_Master_IRQn`；
   - 若用更新中断：`HRTIM1_TIMC_IRQn`、`HRTIM1_TIMD_IRQn`（按你实际使能位）。
9. **DMA（可选）**：`Master REP` 或 `Update` 触发 **memory → HRTIM burst**，表项包含 `MPER`、`MCMPx`、`TCPER`…（按你架构裁剪）。

### 5.3 GPIO

- **Very High** slew / speed；**无错误上下拉**；走线参考开关电源栅极驱动建议（此部分为硬件，但影响“毛刺”观感）。

---

## 6. 初始化序列（推荐顺序，减少上电毛刺）

下列顺序比“随意写寄存器”更稳，便于 Cursor 对照实现：

1. **开时钟**：`HRTIM1`、`GPIO`、（若用）`DMA`。
2. **HRTIM fault 清标志**（`ICR` 清 FLT）——避免上电残留锁死输出逻辑。
3. **DLL 校准并等待就绪**。
4. **配置 Master**：`CKPSC`、`CONT`、`MPER[0]`、`MCMP*` 初值、`MREP=99`（若 burst 在 Master）。
5. **配置 Timer C/D**：`PER[0]`、`CMP*`、**crossbar**（`SETx1R/RSTx1R/SETx2R/RSTx2R`）满足 **互斥高电平**。
6. **配置 preload / update 路径**（`PREEN`、`MREPU`/`TxREPU`、`MSTU`、`UPDGAT`、`RSYNCU`）——此处最容易出错，需与 RM **Table 244–248** 对照。
7. **软件 update 一次**：`MSWU` + `TCSWU` + `TDSWU`（名称以生成代码为准），确保 **active 寄存器**已等于第 0 行表值。
8. **计数器清零**：`MCNTR`、`CNTxR`（C/D）= 0。
9. **输出预置**：对每个将打开的通道，用 **SST/SRT** 软件强制到 **idle 相反侧**或目标初态（RM §28.3.14），避免 **OEN 瞬间**的 X 态。
10. **最后一步才 `OENR`**；随后 **一次性置位 `MCEN` + `TCCEN` + `TDCEN`**（同一 `MCR` 写，利于同步起振）。

---

## 7. `thread_pulse` 启动 / 再触发（多段 burst）

若应用会 **连续多次** thread_pulse：

- **每次**重新装载 **第 0 行表**、`MREP=99`、CNT=0、软件 update、清中断标志、再 `OENR` + 开 `CEN`。
- 若上一次 stop 用了 **ODIS**：再次启动前确认 **输出路径重新 OEN** 且 **fault 未锁存**。
- **不要**假设 RAM 表在 stop 后仍一致——建议 **每次 burst 重新指向表头**。

---

## 8. 中断服务程序：要做 / 不要做（给 Cursor 的硬规则）

### 8.1 可以做（短、确定、边界对齐）

- **清中断标志**（写 `MICR` / `TIMxICR` 对应位）。
- **写 `ODISR`** 关闭四路输出（或按需求关闭）。
- **清 `MCEN` / `TxCEN`** 停计数器。
- **关闭中断使能**（`MDIER` / `TIMxDIER`），防止重复触发。
- **置位软件状态机**（`volatile done`）给 thread。

### 8.2 不要做（高概率引入“开头/结尾杂波”或表错乱）

- **不要在 REP ISR 里改 `PER/CMP` 的活动值**（除非你已经完全理解 preload + `*UDIS` 机制并能证明在边界内）。
- **不要 `printf`/日志**（除非你能接受周期抖动）。
- **不要**在关断顺序里 **先停计数器再 ODIS**（可能在半周期冻结电平；与驱动器策略相关，首版按 §2.3 顺序）。
- **不要**在 ISR 里 **长时间等待**其它外设。
- **不要**忽略 **双缓冲**：误以为写 `PERxR` 立即改变当前沿。

### 8.3 Chirp 行推进放哪里？

**首选**：**UPD / DMA** 在 **周期开始**把 **下一行 preload** 写好；**REP** 只做 **burst 结束**。

若资源极度紧张：**REP ISR 只做 stop**；**用 TIMx UPD ISR 或 DMA** 推进行索引 `k`。

---

## 9. 与官方代码的对照入口（便于本地打开例程）

在 **STM32CubeG4** 仓库中优先检索：

- `HRTIM_Basic_PWM_Master`：**Master PER/CMP → 从定时器输出** 的 crossbar 范例。
- 关键词：`HRTIM_DMA`、`burst`、`preload`、`repetition` 的例程目录（随包版本变动，以你安装的 CubeG4 为准）。

手册：

- **RM0440** 第 **28** 章整章（HRTIM）。
- 参考手册 PDF：  
  `https://www.st.com/resource/en/reference_manual/rm0440-stm32g4-series-advanced-armbased-32bit-mcus-stmicroelectronics.pdf`

---

## 10. 调试与验收（示波器 / 逻辑分析仪）

1. **四路 + Master 同步指示**（可选把 **SCOUT** 或 GPIO 翻转作为 `k` 边界标记）。
2. 统计每路 **上升沿数量** = **100**（或你定义的波形数）。
3. 看 **burst 开始前 1 µs** 与 **结束后 1 µs**：不允许出现 **宽度 < 你定义的最小 Ton** 的尖峰（除非硬件测量噪声）。
4. 变更 `k` 时，检查 **相邻周期频率** 是否单调；检查 **四路边沿相对相位** 是否与设计 crossbar 一致。
5. **温度/电压角**：DLL 与最小脉宽在角点再测一遍。

---

## 11. 与本仓库示例代码的关系

- `src/hrtim_master_100wave_test.c`：**单路 TD2**、**固定** `MPER/MCMP1`、`MREP=99` 的 **最小停机示范**（寄存器级）。它 **未覆盖** 四路、Chirp、preload 换行；但 **停机顺序（REP → ODIS → 停 CEN）** 可作为 burst 结束的子集参考。
- 四路 Chirp 请在 **你本地 Cube 工程** 中扩展；把本文档 §5–§8 作为 Cursor 的约束输入。

---

## 12. 给 Cursor 的一句话任务拆分（建议 prompt 片段）

> 在 STM32G474 上，用 HRTIM1：Master 作为 k 步时间轴，`MREP=99`；Timer C/D 各 2 路输出，crossbar 保证 C 上两路不重叠、D 上两路不重叠；100 行表驱动 `MPER/MCMPx` 预载，在 Master roll-over/repetition update 同步刷新；burst 结束用 **DMA 或极短 ISR** 写 `ODISR` 并停 `MCEN/TCCEN/TDCEN`；禁止在 REP ISR 改活动 CMP/PER；全路径满足 RM0440 Table 232 最小脉宽。

---

**文档版本**：与工作区 `docs/rm0440_hrtim_g474_notes.txt`、`test_plan/hrtim_phased_tests.txt` 同步演进；若实现与本文冲突，以 **示波器事实 + RM0440** 为准。
