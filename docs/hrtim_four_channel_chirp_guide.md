# STM32G474 HRTIM：四路 Chirp（500 kHz–1 MHz）、Timer C/D 双路、100 周期干净启停 — 设计 / CubeMX / 初始化 / 中断边界

本文档面向 **RM0440**（STM32G4 HRTIM）与 **STM32CubeG4** 官方例程习惯写法，供本地工程交给 Cursor 做实现与调试。默认器件 **STM32G474**，外设 **HRTIM1**。

**离线 Cursor 说明**：若无法打开 RM0440 PDF，**以本文 §附录 A 为手册细节的完整来源**（数值表、原句级条件、寄存器行为均已抄入）；正文 §2–§8 为工程化归纳，与附录一致。

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

RM0440 **Table 232**（Period and compare registers min and max values）给出 **16 位 PER/CMP 可写范围与定时单元预分频 `CKPSC[2:0]` 的关系**（单位：HRTIM 计数 tick，不是秒）：

| `CKPSC[2:0]` | **Min**（PER/CMP 合法下限，除脚注例外） | **Max** |
|:------------:|:----------------------------------------:|:-------:|
| 0 | `0x0060` | `0xFFDF` |
| 1 | `0x0030` | `0xFFEF` |
| 2 | `0x0018` | `0xFFF7` |
| 3 | `0x000C` | `0xFFFB` |
| 4 | `0x0006` | `0xFFFD` |
| **≥ 5** | `0x0003` | `0xFFFD` |

**脚注（RM 原文要点）**：最小值须 **≥ 3 个 fHRTIM 时钟周期**（表内已按 CKPSC 折成具体十六进制）。**仅** `CMP1`、`CMP3` 可写 **`0x0000`** 用于 **跳过一拍 PWM（null duty）**，且须满足 RM **“Null duty cycle exception case”** 的 set/reset 事件组合条件；其它用法不要用 0 凑合。

**另一条硬规则（RM 原句）**：**若 compare 值大于 period 寄存器值，不会产生 compare match 事件**（输出可能卡死在某电平，看起来像“杂波缺失”）。

**窄脉冲 / HR 限制（RM §28.3.11 后 Null duty 小节）**：**严格小于** “3 个 fHRTIM 周期” 所对应的 **最小十六进制**（随 CKPSC 变化，如 CKPSC=0 时为 `0x60`，CKPSC=1 为 `0x30`，依此类推）在 **普通 compare** 中 **禁止**；HR 对 **窄于 3×tHRTIM** 的脉冲另有优先级规则（见 RM **§28.3.7**）。

**经验法则（调试导向）**：500 kHz–1 MHz 下若 CKPSC 选得 **小**（tick 细），Table 232 的 **Min** 较大，你的 `PER` 若算得太短会 **非法**；若 CKPSC **≥5**，Min 降到 `0x0003`，但 **仍须**满足 **3×tHRTIM** 的脉宽物理约束与 **GTCMP** 模式（附录 A.8）不混用。

更完整的原文级摘录见 **§附录 A.2–A.4**。

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

HRTIM 大量寄存器为 **preload + active** 双缓冲。RM0440 **Table 244** 归纳 **Master / TIMA..F** 的 **update 源**（软件 `MSWU` / `TxSWU`、**repetition**、**roll-over**、burst DMA 结束等）。**Master 的四种 update 选项全文级说明见 §附录 A.5**（含 `MSWU` 会取消挂起硬件 update 的警告）。

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

## 附录 A：RM0440 自包含摘录（不打开 PDF 也可实现）

下列文字为 **RM0440 Rev 9 §28.3** 相关条文的 **压缩复述 + 关键原句/数值**，用于本地 Cursor 无法访问 PDF 时的单一事实源。若与 ST 后续修订版冲突，以你安装的 RM 版本为准。

### A.1 Roll-over（连续模式）与“计数不回头”故障

**定义（RM）**：连续模式下，**roll-over** 在计数器 **达到 `HRTIM_PERxR` 后回到 0** 时产生。

**用途列表（RM 原枚举）**：roll-over 可用于：输出 set/reset；**触发 preload→active 的寄存器更新**；IRQ/DMA；burst 时钟/触发；ADC 触发；**递减 repetition counter**。

**关键故障条件（RM 原句）**：

> If the initial counter value is above the period value when the timer is started, or if a new period is set while the counter is already above this value, **the counter is not reset**: it **overflows at the maximum period value** and **the repetition counter does not decrement**.

**工程结论**：启动前 **CNT≤PER** 且已 **软件 update**；Chirp 改 PER 时若可能违反 “新 PER < 当前 CNT”，必须先用 **`MUDIS`/`TxUDIS`** 或 **停计数/强制 reset** 等策略处理，否则 **MREP 计数与波形同时失控**。

### A.2 Repetition counter（TIMA..F 与 Master 同类行为）

**不可读**：内部 repetition **不可读**，只能通过 **`HRTIM_REPxR` / `HRTIM_MREPR`** 预置 reload 值。

**装载时机（RM）**：在 **定时器使能**时（Master 为 **`MCEN`**，从定时器为 **`TxCEN`**），内部 repetition 装入 **`REPxR`/`MREP`**。

**递减条件（RM）**：使能后，**每次**计数器因 **reset 事件** 或 **roll-over** 被清零时，repetition **减 1**。

**到零时（RM）**：内部值为 **0** 时，若使能中断/DMA 使能位（Master：`HRTIM_MDIER` 的 **`MREPIE`**；从定时器：`HRTIM_TIMxDIER` 的 **`REPIE`** / DMA 的 **`REPDE`** 等），则产生 **REP 中断或 DMA**。

**周期计数公式（RM 原句）**：

- 若 **`HRTIM_REPxR = 0`**：**每个周期**都可产生 REP 相关事件（若使能）。
- 若 **`HRTIM_REPxR > 0`**：每 **`(HRTIM_REPxR + 1)`** 个周期产生一次 REP 事件。

因此 **100 个完整周期后触发一次 REP 事件** → 写 **`REPxR = 99`（或 Master `MREP = 99`）**，且 burst 期间 **不要改写该寄存器**。

**可变频率 / reset 的例外（RM）**：在计数器 **未到达 PER 就被 reset** 的连续或单次模式下，**reset 也会递减 repetition**，但 **“在 `TxCEN` 置位后的第一次启动”** 有单独例外句（详见 RM **Figure 216** 附近）；**SYNCIN 启动的单次模式**另有 “仅第一次 reset 后递减” 的描述。若你未使用这些模式，可忽略；若使用，必须对照 RM 图逐条仿真。

### A.3 Table 232：PER/CMP 最小、最大（再列一次便于检索）

见正文 **§2.5** 表。实现时把 **100 行表**全部映射到当前 **`CKPSC[2:0]`** 行的 **Min/Max** 内。

### A.4 Null duty 与 CMP=0 的合法条件（RM 摘要）

RM 允许 **`CMP1`/`CMP3 = 0`** 作为 **跳过一拍** 的特殊情况，**当且仅当**（RM 条件列表，意译）：

- 输出 **SET** 来自 **PERIOD** 事件；
- 输出 **RESET** 来自 **compare 1（或 compare 3）**；
- 该 compare 事件 **仅在本定时单元内**使用，**不**再拿去驱动其它定时单元。

**其它**需要 0% 占空的情形：RM 要求用 **SET/RESET 选同一 compare 值** 且该值 **仍须 ≥ 3×fHRTIM 周期对应的下限**，由 **§28.3.7** 的优先级规则处理。

### A.5 Master 的四种 update（preload→active 的触发源）

RM **Table 244** 前文列出 **Master timer** 的 **4 种 update 选项**（意译编号）：

1. **软件立即 update**：写 **`HRTIM_CR2` 的 `MSWU=1`**；**任何挂起的硬件 update 请求被取消**。
2. **Master roll-over 且 Master repetition=0**：由 **`HRTIM_MCR` 的 `MREPU=1`** 使能。
3. **Burst DMA 完成一次**：**`BRSTDMA[1:0]=01`** 于 `HRTIM_MCR`；可与 `MREPU` 同时为 1。RM **Note**：若 **`SWU`** 置位为强制 update 模式，可在 burst 序列 **刚结束立刻** update；若 `SWU` 为 0，则 **在 burst 结束后的下一次 update 事件**才 update。
4. **Burst DMA 完成后紧跟的一次 Master roll-over**：**`BRSTDMA[1:0]=10`**。

**Master update 可触发中断/DMA**（RM 原句）：Master update 事件可配 **中断或 DMA**。

**Table 244 列出的 Master 可预载寄存器（RM）**：`HRTIM_DIER`、`HRTIM_MPER`、`HRTIM_MREP`、`HRTIM_MCMP1R`..`MCMP4R`（在 **`HRTIM_MCR` 的 `PREEN=1`** 时走 preload 机制）；update 源可为 **软件 / repetition / burst DMA / repetition following burst DMA**。

**Timer x（A..F）** 另有更多 update 源（**软件、本定时器 repetition、本定时器 reset、burst DMA、其它定时器或 Master 的 update、`hrtim_upd_en` 等**），仍以 RM Table 244 为准在 Cube 里逐项对齐。

### A.6 `MUDIS` / `TxUDIS`：多寄存器批量改表的标准节奏（RM 原流程）

RM 在 **§28.3.11** 写明（意译，保留关键句）：

- **`MUDIS` 与各 `TxUDIS` 位在 `HRTIM_CR1`**：可 **暂时禁止** “从 preload 到 active 的转移”，**无论**当前选定的 update 事件是什么；用于 **跨多个定时器改多个寄存器**；**清 0 这些位后**，在下一次合法 update 事件上 **恢复** preload→active。

**RM 给出的第一个范例（Master + TIMB + TIMC 同步）**：

- `HRTIM_MCR`：`MREPU=1` → **在 Master repetition 周期末尾** update；
- `HRTIM_TIMBCR`、`HRTIM_TIMCCR`：置 **`MSTU`** → **TIMB/TIMC 与 Master 同时 update**；
- 软件改表前：置 **`MUDIS`、`TBUDIS`、`TCUDIS`** → 此后 **硬件 update 请求被忽略**，可安全写 preload；
- 改完后：**清 `MUDIS`、`TBUDIS`、`TCUDIS`** → **下一次 Master repetition 事件** 上 **一次性** 把各定时器 preload 转入 active。

**第二个范例（TIMA 驱动 TIMD/TIME）** 同理，用 **`TAUDIS`、`TDUDIS`、`TEUDIS`** 与 **TIMA repetition** 配对（RM 原段落）。

**给四路 Chirp 的直接建议**：把 **Master `MREPU` + TIMC/TIM `MSTU` + `MUDIS/TCUDIS/TDUDIS`** 当作 **“同一时刻换 4 路频率/脉宽”** 的默认答案；ISR **只推进 RAM 索引**，**不写 active**。

### A.7 Crossbar 基本规则（SET/RESET 源）

**语义（RM）**：**Set** → 输出 **active** 边沿；**Reset** → **inactive**。**极性**由 `OUTxR` 的 `POLx` 定义（正极性时 active=逻辑 1）。

**每路输出两个 32 位寄存器**：`HRTIM_SETxyR`、`HRTIM_RSTxyR`（`x`=A..F，`y`=1/2）。**最多 32 个事件 OR 在一起**；**同一事件既选 set 又选 reset → 输出 toggle**。

**1 个 tHRTIM 内多次 toggle（RM）**：**不允许**；同一周期内两次 toggle **只认第一次**。

**软件强制（RM）**：`SETxyR`/`RSTxyR` 里的 **SST/SRT** 可在 **计数器未使能**时用于 **预置输出**；crossbar 的 set/reset 在 **`TxCEN` 置位后**才生效（**软件强制例外**用于上电对齐）。

### A.8 “Greater than” compare（`GTCMP1`/`GTCMP3`）— Chirp 慎用

RM **§28.3.12** 说明：对 **CMP1/CMP3 作为 RESET** 的配置，可开 **“greater than”** 模式（`HRTIM_TIMxCR2` 的 `GTCMPx`）以实现 **周期内尽早** 改占空比。

**致命交互（RM 原句，意译）**：当 **`GTCMPx=1`** 时，**对应 compare 寄存器的 preload 机制被关闭**（**无论 `PREEN` 是否为 1**），写入会 **尽快** 影响比较器逻辑。

**给 Chirp 的结论**：需要 **每周期边界批量换 PER/CMP** 时，**不要**对参与换表的 compare 开 `GTCMPx`，否则 **失去“影子寄存器在同一 update 生效”** 的保证，极易出现 **半周期毛刺**。

### A.9 输出级：RUN / IDLE / FAULT 与 `OEN`/`ODIS`（RM §28.3.14）

**三态（RM）**：**RUN**（crossbar 控制 active/inactive）；**IDLE**（复位后默认、软件 ODIS、burst idle 等）；**FAULT**（故障输入等）。

**关键位（RM）**：`HRTIM_OENR` 的 **`TxyOEN`** 为 **控制兼状态**：软件写 1 进入 RUN；**硬件在回到 IDLE/FAULT 时清 0**。`HRTIM_ODSR` 的 **`TxyODS`** 指示 IDLE 还是 FAULT。`HRTIM_ODISR` 的 **`TxyODIS`** 用于 **软件关断** 到 IDLE。

**优先级（RM）**：**IDLE 优先于 FAULT**（即使在 fault 仍有效时，**置 `ODIS` 仍可进 IDLE**）。**FAULT 优先于 RUN**（fault 配置使能且条件满足时，**即使同时写 OEN 也会进 FAULT**）。

**IDLE 电平**：`OUTxR` 的 **`IDLESx`**：0=idle 时为 **inactive** 电平；1=idle 时为 **active** 电平。

**上电预置（RM 原句）**：在进入 RUN（`TxyOEN=1`）前，可用 **`HRTIM_SETx1R`/`HRTIM_RSTx1R`（软件强制位）** 预置输出电平；**一旦 `TxyOEN=1`，输出立即接到 crossbar**（若此时计数器时钟停，电平取决于复位后初态或“停振且输出关闭时的 RUN 电平”——调试时需对照 `ODSR`）。

**停机顺序的工程推荐（正文 §2.3 与附录一致）**：**先 `ODIS`（进 IDLE，电平由 `IDLESx` 定义）→ 再停 `MCEN`/`TxCEN` → 再关中断使能**。这与 RM 状态图 **“IDLE entry: Software (ODIS)”** 一致。

### A.10 DLL 就绪标志（与启动顺序）

RM 要求 DLL **校准完成**后再依赖 HR 边沿；实现上在 **`HRTIM_ISR` 的 `DLLRDY`** 置位后再启动波形（与 ST LL 例程一致）。具体配置位见 **`HRTIM_DLLCR`**（`CAL`、`CALEN`、`CALRTE` 等）。

### A.11 Burst mode 控制器（可选“硬件 idle 段”）

RM **§28.3.15**：burst 控制器可在 **RUN 与 IDLE** 间按 **BMCMP/BMPER** 插入 **空闲段**；**idle/run 对齐**可选用 **Master 或 TIMA..E 的 reset/roll-over** 作为 burst 计数时钟（`BMCLK` 0000–0101）。这与 “用 repetition 自动停” **不是同一机制**；若要用 burst 做 **精确 N 个 run 段**，需单独按 RM 配 **触发源、BMOM 连续/单次、IDLEM/IDLES** 等，调试复杂度高，**四路 Chirp 首版不建议与表驱动混为第一条路径**。

---

**文档版本**：与工作区 `docs/rm0440_hrtim_g474_notes.txt`、`test_plan/hrtim_phased_tests.txt` 同步演进；若实现与本文冲突，以 **示波器事实 + RM0440** 为准。无法读 PDF 时，以 **§附录 A** 为 RM 细节权威副本。
