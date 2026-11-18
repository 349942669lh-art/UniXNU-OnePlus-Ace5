# A750 GPU 接入框架（阶段机 v1）

> 2026-11-05 增量：运行期电源/共享资源事务及原厂 ISSUE_CMD 受限提交库见
> [GPU_RUNTIME_SUBMISSION.md](GPU_RUNTIME_SUBMISSION.md)。新增代码已接入
> 原生构建和宿主测试，但尚无板级授权/电源/context/fence provider 绑定；
> 不代表下表任何硬件阶段已经通过。下文早期 SCRATCH 阻塞记录不是当前唯一缺口。

本文是 giulia（OnePlus Ace 5 / SM8650 / Adreno 750v2）GPU 接入的**整体框架**，
不是又一份实验记录。它做三件事：

1. 把 `docs/GPU_BRINGUP.md`、`GPU_MAPPING.md`、`GPU_GUNYAH_TRANSPORT.md`、
   `GPU_CPU_PMAP.md`、`GPU_FENCE_PROVIDER.md`、`GPU_TERMINAL_BRINGUP.md` 里
   已经存在的模块，按**启动顺序**排成一条可审计的链；
2. 给每一层定义**进入前必须证明什么**（probe）、**进入时做什么**（enter）、
   **失败/退出怎么处理**（leave）；
3. 用 `ios16/gpu/giulia_gpu_stack.h` 把顺序、门控、证据和回滚固化成代码，
   让"细节"只需填进各自的槽位，不再散落在服务和实验脚本里。

框架本身**不访问硬件、不读 MMIO、不发 HVC**。它只决定：能不能进、凭什么进、
失败了能不能退。

## 1. 现状：GPU 到底哪里还没通

按证据强度分三档。`通`= 有实机证据；`部分`= 有离线或孤立实机证据，
但没有端到端成立；`未通`= 没有可用的实现或没有成功证据。

| 环节 | 现有证据 | 状态 |
| --- | --- | --- |
| UEFI CX/时钟准备（`GIULIA_GPU_MODE=3`） | 实机可跑出 CRC 报告，频率投票与 CM3 复位保持有回读 | 通 |
| 固件解析、打包、摘要固定 | 真实 15 块固件、221184 字节容器、SHA256 逐字节核对 | 通 |
| GMU 宿主 RAM + 4 KiB 页表 + CPU 缓存发布 | 实机 5 段预留、6 表页、104 叶页；`dc zva` panic 已修 | 通 |
| GMU TCM 写入/回读 | 实机 135 个字全部写入并读回，实验后恢复原厂 TCM | 通 |
| Gunyah HVC/RM 查询会话 | 七步只读查询、分片、epoch、空闲 pump 离线通过；实机只有 boot FDT 资源提取 | 部分 |
| carveout `0xd4e23000/0x2dd000` 共享事务 | `MEM_SHARE`+`MEM_LOOKUP` 顺序离线通过；未接进 worker，无 share 快照 | 部分 |
| GPU SMMU SID 5 上下文 | 实机配置、CM3 经 SMMU 读写、未映射翻译故障、撤销后恢复 | 部分（非常驻） |
| GMU START / HFI 初始化 | warmboot 查询、性能/带宽表、`CORE_FW_START` 均 ACK；**START 停在首个 SCRATCH 分配请求** | 部分 |
| AQE / NOP / CP 提交 | NOP 无 ACK；首个 `CP_INIT` 后 CDC 消失、进入 `05c6:900e`，原因未定，CP 入口已禁用 | 未通 |
| hardware fence / CTX0 / 相邻页授权 | 只有布局、解码与 provider 注册接口 | 未通 |
| 命令提交、scanout、Metal 发布 | 无 | 未通 |

**当前唯一的硬阻塞**是 `START` 之后固件发来的第一个请求：

```text
header=0x00700b14 version=1
gpu_addr=0 flags=0x33f kind=2(SCRATCH)
host_handle=0 gmu_handle=2 gmu_addr=0 bytes=4096 alignment=0
```

它要求一块**GPU 与 GMU 都可访问、privileged、可写、host 负责初始化的 4 KiB**，
并且要回填真实的 `gpu_handle`/`gmu_handle`。服务今天还没有把动态分配接上，
所以只能按原厂语义拒绝并复位——这一步就是 `GGSK_HFI` 的进入门槛，也是
`GGSK_CONTROLLER`（队列、CTX0）的前置。应答器的判断逻辑已经实现（见第 4 节），
剩下的工作是把它接到服务的 pool 与映射后端上。

CP 侧另有独立问题：900E 之前的寄存器初始化只做了部分 `gen7_start`，
保护寄存器、power-up register list、ZAP/secure 初始化都不完整，因此
"能发 IB"之前必须先给出 CP 侧的最小可观测闭环。

## 2. 阶段机

15 个阶段，索引即启动顺序。类别决定授权方式：`只读` 不改任何状态，
`宿主` 只改 CPU 页表和宿主 RAM，`设备` 会写 MMIO / 发 HVC / 动固件可见内存。

| 枚举 | 名称 | 类别 | 前置 | 状态 | 现有实现 | probe 必须证明 | 下一步 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `GGSK_BOOT` | boot-handoff | 只读 | — | 部分 | `giulia_gpu_gunyah_boot.h`、`giulia_platform_power.h`、UEFI `GpuGunyah.c` | 本次启动的 ADT 报告 CRC/长度/epoch 有效，`giulia_gpu=N` 与报告标记一致 | 把报告升级为可核对的租约描述（当前只作记录） |
| `GGSK_FIRMWARE` | firmware | 只读 | `GGSK_BOOT` | 通 | `giulia_gpu_pack.h`、`giulia_gpu_firmware.h` | 容器 SHA256、四成员摘要、长度与版本匹配 | 保持摘要固定，新增成员时同步 `hardware/` |
| `GGSK_HOST_MEMORY` | host-memory | 宿主 | `GGSK_FIRMWARE` | 通 | `GiuliaGPU::prepareMemory`、`giulia_gpu_memory.h`、`giulia_gpu_cache.*` | 5 段预留、表页/叶页数量与页表遍历一致，缓存发布到一致性点 | 为动态分配补一条可回收的池路径 |
| `GGSK_POWER` | power-lease | 设备 | `GGSK_BOOT`+`GGSK_HOST_MEMORY` | 部分 | `giulia_gpu_power_control.h`（`gg_power_ops`） | CX 开启、GMU 625 MHz/hub 200 MHz/SMMU 票使能、CM3 复位保持、租约独占 | 写 XNU 运行时 AOP/ClockDxe 后端，UEFI 报告不能当运行时函数 |
| `GGSK_GUNYAH` | gunyah-channel | 设备 | `GGSK_BOOT` | 部分 | `giulia_gpu_gunyah_{hvc,rm,queries,session,owner,irq}.h` | 动态 self/peer VMID、peer READY、独占 TX/RX capability、IRQ 租约、epoch 匹配 | 让 worker 常驻持有 `ggown_state` 并对外提供租约句柄 |
| `GGSK_SHARE` | share | 设备 | `GGSK_GUNYAH`+`GGSK_HOST_MEMORY` | 部分 | `giulia_gpu_gunyah_share.h` | `MEM_SHARE` 后 `MEM_LOOKUP` 返回同一 parcel，self/peer RW、label5、peer READY | 把 share 快照接进 worker，作为 `ggfn_import` 的输入 |
| `GGSK_SMMU` | smmu-active | 设备 | `GGSK_POWER`+`GGSK_HOST_MEMORY` | 部分 | `giulia_gpu_smmu.h`、`vendor/ArmSmmu4K` | SID 5 与 GPU bank 0/1（vendor 保留）配置读回、fault-on-unmatched、ACTLR/security、TLBI 完成 | 从"实验后撤销"改为"本驱动常驻持有"并加故障处理 |
| `GGSK_GMU_LOAD` | gmu-load | 设备 | `GGSK_SMMU`+`GGSK_FIRMWARE`+`GGSK_HOST_MEMORY` | 部分 | `giulia_gpu_gmu.h`、`giulia_gpu_gmu_impl.h` | 复位保持下 TCM 逐字回读、普通 RAM 内容与 pack 一致、无陈旧 ready | 在 POVER/SMMU 常驻后跑一次端到端 load |
| `GGSK_GMU_START` | gmu-start | 设备 | `GGSK_GMU_LOAD` | 未通 | 同上（`gg_gmu_start`） | 释放复位顺序正确、HFI transport ready、17 队列就绪、CM3 无 fault | 先解决 SCRATCH 应答，再谈 START 完成 ACK |
| `GGSK_HFI` | hfi-client | 设备 | `GGSK_GMU_START` | 部分 | `giulia_gpu_hfi.h`、`giulia_gpu_hfi_alloc.h` | feature/performance/bandwidth/`CORE_FW_START` 均有带状态的 ACK；动态请求由自有映射应答 | 应答器已实现并过 21 组宿主用例；待接服务的 pool/映射后端 |
| `GGSK_CONTROLLER` | controller | 设备 | `GGSK_SHARE`+`GGSK_SMMU`+`GGSK_GUNYAH` | 未通 | `giulia_gpu_fence.h`、`giulia_gpu_fence_queue.h`、`giulia_gpu_fence_cpu.h` | 表头/队列布局、CTX0 client 1、队列前后各一页授权、门铃 label6、Normal NC 守卫 | 补控制器相邻页授权与 CTX0 注册 |
| `GGSK_FENCE` | fence-provider | 设备 | `GGSK_CONTROLLER`+`GGSK_GMU_START` | 未通 | `giulia_gpu_fence_provider.h`、`giulia_gpu_fence_native.c` | provider proof bits 完整、epoch 一致、跨 VM 锁与完成后端可观察 | 写 fence 完成/错误/复位生命周期 |
| `GGSK_SUBMIT` | submit | 设备 | `GGSK_FENCE`+`GGSK_HFI` | 未通（已有离线实现） | `giulia_gpu_submit.{h,c}`：受限 ISSUE_CMD 与 fence 生命周期，未绑定 provider | 一条自有 IB 到达真实 fence，MEM_WRITE 结果回读一致 | 900E 定因后完成 CP、context、硬件 fence provider 并绑定 |
| `GGSK_SCANOUT` | scanout | 设备 | `GGSK_SUBMIT` | 未通 | 无（显示通路独立可用） | GPU 帧与 CPU 参考像素一致，swap offer/retire 成功 | 接 NativeMobileDisplay 等价的 offer/retire |
| `GGSK_METAL` | metal-publish | 设备 | `GGSK_SUBMIT`+`GGSK_SCANOUT` | 未通 | 无 | 加速器发布后 CARenderServer 走原生设备且无回退 | 复用 sheng 的 publish 路径但按 iOS 16 重新验证 |

## 3. 框架规则

代码在 `ios16/gpu/giulia_gpu_stack.h`，规则写成不变量，不是注释：

1. **只有前置全部 enter 才能 enter**。缺 provider 或 probe 失败会阻塞依赖它的阶段，
   既不跳过，也不允许用常量满足。
2. **probe 只读，且紧挨着 enter 执行**。`ggsk_advance()` 内部先调 `probe()`，
   失败即不进；上一次启动的"ready"不会被复用。
3. **宿主变更与设备写入分开授权**。`ggsk_policy` 两项默认 0，
   也就是默认只能准备与观察——和现在服务里 `HardwareReady=false` 一致。
4. **设备阶段 enter 失败 → 全栈 quarantine，并停止前进**；
   `leave` 返回不确定（非 0）→ 同样 quarantine，并**停止反序释放**，
   因为资源可能还持有 DMA。
5. **拓扑在首次 advance 时冻结**，一个阶段只能有一个 owner，注册后不能替换。
6. **计数器是累计的**：写过硬件就一直算写过；retreat 只表示"已确认停止"，
   不表示硬件已空闲。

依赖图是 DAG 而不是前缀：`GGSK_GUNYAH` 只依赖 `GGSK_BOOT`，
所以电源租约没通时仍然可以先把 Gunyah 通道打通，日志会同时显示
`blocked=power-lease` 与 `entered` 里的 gunyah-channel。

## 4. 第一个打通点：`F2H_MSG_MEM_ALLOC` 应答

`GGSK_HFI` 的硬门槛是 START 之后固件发来的内存请求。本轮把它做成独立单元
`ios16/gpu/giulia_gpu_hfi_alloc.h`：只做协议判断，不碰硬件，宿主侧 21 组用例可复现。

- 解析固件包（`hdr/version/desc`），校验 id=20、type=`HFI_MSG_CMD`、size 字段与
  帧长一致、`mem_kind<26`、flags 只接受已知位、拒绝 secure、size 非零、
  对齐指数 ≤30；
- 按原厂语义计算有效 flags：普通 SYSRAM 没有 GFX 访问时折入 `0x38`，并且必须
  返回 GMU 地址；有 GFX 访问时只有 `GMU_ACC` 才返回 GMU 地址；
- 只接受 provider 真正拥有的块：地址必须页粒度、GMU 地址满足 VA 对齐、未申请的
  别名必须为 0——GPU 看不到的块 `gpu_addr=0`，GMU 看不到的块 `gmu_addr=0`；
- 应答保留请求的 flags/kind/align，只把 size 换成真实大小，`host_mem_handle`
  回填请求的 `gmu_mem_handle`；
- 同 key `(kind, gmu_mem_handle)` 复用同一块，但 size/flags/align 不一致时拒绝，
  不做隐式共享；
- 失败路径：reserve 之后校验失败或 publish 失败，只在 release 确认后回收；
  release 不确定就整栈 quarantine；表满在分配前拒绝，不泄漏。

kind 5/19/22/23 需要独立后端（secure、IPC core、memstore、hw-fence），
没有 `reserved` 回调时返回 `GGA_RESERVED`，不会拿普通 RAM 顶替。

配套两层也在本轮落地：

- `giulia_gpu_hfi_alloc_pool.h`：应答器的真实后端。`reserve` 走现有 `ggd_pool`
  的 bump 分配，按有效 flags 选择缓存的 GMU 别名窗口，逐空间 `map()` 后再取
  地址；映射失败会把已经建立的映射退掉；`publish` 走清零/回读/缓存发布；
  `release` 只做确认过的 unmap（bump 池不回收空间，这一点写在文件头）。
- `giulia_gpu_hfi_pump.h`：worker 的每轮策略。只把 `F2H_MSG_MEM_ALLOC` 交给
  应答器，ACK/ERR/DEBUG 只计数不解释；请求被拒立即停并上报（否则固件会一直等
  一个不会来的回包）；回包入队失败则 quarantine——序号已经消耗，丢掉回包就是
  静默挂死。

这三层用同一套 `ggh_io` 串起来测：pump 测试直接跑真实队列表内存
（`ggh_init`），因此队列语义不是模型。

接进服务的 glue 已落成 `ios16/gpu/giulia_gpu_hfi_alloc_impl.h`：`gga_pool_io` 的
`address/map/unmap/publish` 直接读本服务的 `devPool`、调用 `mapDevAllocation()` 和
缓存发布；`hfiAllocInit()` 在 `prepareMemory()` 之后调用一次；worker 每轮在
`hfiPumpOnce(2)` 里推进 pump，前置条件是 `hostPrepared && manualMemoryPinned &&
devMappingsManaged && !platformPoisoned && !gmuFaulted`。`unmap` 明确返回"未确认"——
bump 池在重启前不回收，release 只能让整栈 quarantine，不能假装内存已经自由。

服务侧 glue 由两层验证：

- `tests/ios16_gpu_service.py` 把真实的 `giulia_gpu_hfi_alloc_impl.h` 编译进宿主
  stub 服务，并运行 `hfiAllocInit()`、`hfiPumpOnce()`、`hfiAllocReport()`：
  空池初始化后游标为 4 MiB/0，未 pin 时 pump 返回 `GGPP_ARGUMENT`，
  全零队列表被判为 CORRUPT 而不是"空队列"并锁存 quarantine；
- `tests/ios16_gpu_hfi_wiring.py` 检查成员、初始化顺序、worker 调用点与
  report/stop 语义。

两层都不编译内核；内核链接与实机行为仍以 CI 与设备日志为准。

`GGSK_SHARE` 的第一步也落成可测单元 `giulia_gpu_share_snapshot.h`：
`ggsh_run()` 成功后把 epoch、动态 self/peer VMID、parcel、通道序号一次性冻结；
`ggsn_share()` 每次回答前都用 live `ggsh_state` 复核并比对冻结值，任何偏差都
永久失效（`GGSN_STALE`），冻结副本永不跟随 live 变化。它只读记录字段，
不发 HVC/SCM、不碰 carveout；`ggsn_share_cb` 就是 `ggfn_inputs.share` 需要的回调形状。真正的运行时接入
（worker 里跑 `ggsh_run` 再取快照）由 `giulia_gpu_share_runtime.h` 门控：
通道 → owner live → 动态 identity → **authority**，全部通过才允许 `ggsh_init`；
authority 由平台后端证明 primary 角色与 carveout 独占，当前没有该后端，
所以这条路径会停在 `GGSR_AUTHORITY` 且 `share` 状态保持为零，不会发出任何
RM/SCM 报文。成功后才 `ggsh_run` 并 `ggsn_take`，绑定结构直接交给
`ggfn_inputs.share`。

## 5. 代码与接入点

| 文件 | 作用 |
| --- | --- |
| `ios16/gpu/giulia_gpu_stack.h` | 阶段表、策略、槽位、`ggsk_init/register/probe/probe_all/advance/retreat` |
| `ios16/gpu/giulia_gpu_stack.c` | 内核入口 `giulia_gpu_stack_*` 与每阶段一行有界日志 |
| `tests/ios16_gpu_stack.c` | 宿主 fixture：顺序、门控、缺 provider、probe 失败、enter 失败、leave 不确定、注册冻结、计数器语义 |
| `tests/ios16_gpu_stack_contract.py` | 头文件阶段表与本文表格逐行一致性 |
| `ios16/gpu/giulia_gpu_hfi_alloc.h` | `F2H_MSG_MEM_ALLOC` 应答：解析、flags 语义、块校验、回包、失败隔离 |
| `ios16/gpu/giulia_gpu_hfi_alloc_pool.h` | 应答器后端：`ggd_pool` bump 分配 + GPU/GMU 映射 + 清零回读发布，失败只回收映射 |
| `ios16/gpu/giulia_gpu_hfi_pump.h` | worker 每轮策略：只派发 MEM_ALLOC、其余计数丢弃、拒绝即停、回包失败即 quarantine |
| `tests/ios16_gpu_hfi_alloc.c` | 应答器 21 组宿主用例（真实 START 报文、对齐、复用、隔离、耗尽） |
| `ios16/gpu/giulia_gpu_hfi_alloc_impl.h` | 服务侧 glue：`gga_pool_io` 指向本服务的 `devPool`、`mapDevAllocation`、缓存发布；worker 每轮 `ggpp_pump` |
| `ios16/gpu/giulia_gpu_share_snapshot.h` | `GGSK_SHARE` 的冻结身份：只读地固化 epoch/parcel/动态 VMID/通道序号，并供 `ggfn_inputs.share` 回调 |
| `tests/ios16_gpu_share_snapshot.c` | 快照 10 组宿主用例（一次性取用、未完成/通道/身份/parcel 拒绝、失效永久化、回调参数） |
| `ios16/gpu/giulia_gpu_share_runtime.h` | 一次 share 事务的门控：通道/owner/identity 逐级检查，**authority 必须在 `ggsh_init` 之前**，之后才是 run+快照 |
| `tests/ios16_gpu_share_runtime.c` | 门控 7 组用例（拒绝顺序、无 authority 时 share 状态零污染、可重试、通道/参数拒绝） |
| `ios16/gpu/giulia_gpu_share_impl.h` | 服务侧 share glue：`gpu.share` → owner 通道 + 16 KiB NC 控制页 + `ggsr_prepare/finish`；authority 目前只会拒绝 |
| `tests/ios16_gpu_hfi_wiring.py` | 结构检查：成员、初始化顺序、worker 调用、report/stop 语义（不编译内核） |
| `ios16/gpu/giulia_gpu_stack_impl.h` | 服务侧阶段机：三个只读 probe、`gpu.stack` 计划、`gpu.status` 摘要、stop 时的 retreat |
| `tests/ios16_gpu_service.py` | 宿主服务 harness：编译真实 HFI/阶段机 glue，运行初始化、泵、计划与报告，并保持原有 worker/stop 生命周期检查 |
| `tests/ios16_gpu_hfi_alloc_pool.c` | 应答器 + 池后端联调 13 组用例（映射失败原子性、发布/释放、单 master 限制） |
| `tests/ios16_gpu_hfi_pump.c` | 用真实 `ggh_init` 队列内存驱动 pump 的 8 组用例（预算、丢弃、拒绝、满队、坏队、复用） |

阶段机已注册进服务，但只登记了三个能真正观测的阶段（boot-handoff / firmware /
host-memory）；其余阶段没有 provider，`gpu.stack` 会把链停在那里而不是跳过。
注册契约如下：

1. `GiuliaGPU::start`：`giulia_gpu_stack_init()` 一次，再按层 `giulia_gpu_stack_register()`
   15 次。策略来自 `giulia_gpu` 模式：模式 1/2 全 0，模式 3 允许宿主变更；
   设备写入只在显式调试命令里临时放开。
2. 新终端命令 `gpu.stack`（只读计划）与 `gpu.stack.advance <stage>`：
   `gpu.stack` 只调 `giulia_gpu_stack_plan()`，不写任何寄存器。
3. `gpu.status`：追加 `giulia_gpu_stack_log()` 的两行摘要。
4. `GiuliaGPU::stop`：`giulia_gpu_stack_retreat()`；quarantine 时保留资源并只记录，
   不自动 free。

每个阶段的 `probe/enter/leave` 实现放在各自的现有单元旁边，例如
`giulia_gpu_stack_power.c`、`giulia_gpu_stack_hfi.c`，不塞进框架文件。

## 6. 里程碑与验收证据

| 里程碑 | 内容 | 验收证据 |
| --- | --- | --- |
| M1 | 框架接入服务：`gpu.stack` 只读计划 | 已完成：mode 3 下日志为 `entered=3/15 ... blocked=3(power-lease) device_writes=0`，mode 1 为 `entered=2/15 blocked=2(host-memory)`；`gpu.status` 追加摘要行 |
| M2 | `GGSK_SHARE` 通：share 快照接进 worker | 快照（10 例）+ 门控（7 例）+ `gpu.share` 服务胶水（宿主 harness 断言 `authority=unavailable` 与 `prepare code=no-authority`）完成；M2b 还缺真实 authority 后端与一次成功的 `MEM_LOOKUP`/`ggfn_import` |
| M3 | `GGSK_POWER`+`GGSK_SMMU` 常驻 | 重启后仍 `smmu_active=1`，未映射访问有真实 fault，撤销后能恢复 |
| M4a | SCRATCH 应答器：kind2/14/15/16/1 由自有映射应答 | 已完成宿主侧；`HFI ALLOC FIXTURE: 21 cases passed`、`HFI POOL FIXTURE: 13 cases passed`、`HFI PUMP FIXTURE: 8 cases passed` |
| M4b | `GGSK_HFI` 通：三层接进 worker + START 完成 ACK | glue 已在宿主 harness 编译并运行（空池/未 pin/坏队三个场景），结构检查通过；内核 CI 链接通过后再看 START 完成 ACK（不是分配请求）、17 队列就绪、无 900E |
| M5 | `GGSK_CONTROLLER`+`GGSK_FENCE` 通 | CTX0 client 1 注册成功，队列前后页映射通过审计，fence 可观察完成 |
| M6 | `GGSK_SUBMIT`→`GGSK_SCANOUT`→`GGSK_METAL` | 一条 IB 的 MEM_WRITE 回读一致；GPU 帧像素与 CPU 参考一致；render server 使用原生设备 |

M1 之后每一步都必须同时更新 `hardware/ios16-gpu-*.json` 与对应 `reports/` 目录，
沿用本仓"离线证据与实机证据分开记录"的规则。

## 7. 与现有文档的分工

| 文档 | 负责 |
| --- | --- |
| 本文 | 顺序、门控、证据、回滚和里程碑 |
| `GPU_BRINGUP.md` | 硬件输入、构建与打包流程、历史阶段 |
| `GPU_MAPPING.md` | 地址、权限、固件封存、HFI kind 处理 |
| `GPU_GUNYAH_TRANSPORT.md` | HVC/RM/会话/共享事务的协议细节 |
| `GPU_CPU_PMAP.md` | 队列 CPU 映射与 0086 原生守卫 |
| `GPU_FENCE_PROVIDER.md` | provider 注册、导入与与 fence 生命周期 |
| `GPU_TERMINAL_BRINGUP.md` | 实机实验与故障记录 |

## 8. 验证

```bash
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
   -DGG_HOST_TEST tests/ios16_gpu_stack.c ios16/gpu/giulia_gpu_stack.c -o /tmp/gpu-stack && /tmp/gpu-stack
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
   -DGG_HOST_TEST tests/ios16_gpu_hfi_alloc.c -o /tmp/gpu-hfi-alloc && /tmp/gpu-hfi-alloc
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
   -DGG_HOST_TEST tests/ios16_gpu_hfi_alloc_pool.c -o /tmp/gpu-hfi-pool && /tmp/gpu-hfi-pool
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
   -DGG_HOST_TEST tests/ios16_gpu_hfi_pump.c -o /tmp/gpu-hfi-pump && /tmp/gpu-hfi-pump
python3 tests/ios16_gpu_stack_contract.py
python3 tests/ios16_gpu_hfi_wiring.py
python3 tests/ios16_gpu_service.py     # 编译真实 glue 的宿主 harness
c++ -x c++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only \
   -include ios16/gpu/giulia_gpu_stack.h -include ios16/gpu/giulia_gpu_hfi_alloc.h \
   -include ios16/gpu/giulia_gpu_hfi_alloc_pool.h -include ios16/gpu/giulia_gpu_hfi_pump.h /dev/null
bash scripts/test-ios16-gpu.sh    # 已把上面几项并入统一 runner
```

宿主 fixture 只证明框架会拒绝不安全的转移；它不证明 A750、SMMU、GMU、Gunyah
或 Metal 的任何行为。统一 runner 当前全绿，并会执行到上面这些项：

```text
STACK FIXTURE: 16   HFI ALLOC FIXTURE: 21   HFI POOL FIXTURE: 13
HFI PUMP FIXTURE: 8  SHARE SNAPSHOT FIXTURE: 10  SHARE RUNTIME FIXTURE: 7
STACK CONTRACT: 15 stages   HFI WIRING   STACK WIRING   SHARE WIRING
```

## 9. 本轮边界

- 新增 `giulia_gpu_hfi_alloc.h`、`giulia_gpu_hfi_alloc_pool.h`、`giulia_gpu_hfi_pump.h`、
  `giulia_gpu_hfi_alloc_impl.h` 及三套宿主用例（21 + 13 + 8）与两份结构/编译检查；
  协议与语义取自固定的原厂 `adreno_hfi.h`/`adreno_gen7_hwsched_hfi.c`
  （摘要已在 `hardware/` 记录）；pump 用例直接驱动真实 `ggh_init` 队列表内存；
- `GiuliaGPU` 只增加成员、初始化、worker 调用、report 与 stop 日志，
  没有改动任何既有硬件路径，也没有新增终端命令；
- glue 由 `tests/ios16_gpu_service.py` 编译并运行空池/未 pin/坏队三个场景；
  内核链接与实机行为仍待 CI 与设备日志，本文不声称已通过；
- 顺手对齐了平台改名/目录重排落下的 fixture：`qmp_available` →
  `giulia_platform_qmp_available`（dev/mapping runtime）、`dev_runtime` 的
  provider 区块锚点改为 `giulia_gpu_terminal`、`gunyah_build` 增加
  `ios16/platform` 头文件路径、`gunyah_runtime` 与 `service` fixture 补上
  HFI 分配胶水的桩成员；
- 阶段机接入服务：只注册 boot-handoff/firmware/host-memory 三个只读阶段，
  `gpu.stack` 走 `ggsk_advance()` 并输出 15 行阶段表；策略固定
  `allow_device_writes=0`，mode 3 才允许宿主变更；未注册阶段保持 no-provider；
- 服务侧 share glue：`gpu.share` 取 owner 通道、分配并发布 16 KiB NC 控制页，
  调 `ggsr_prepare`；`shareAuthorize` 只记录 `authority=unavailable` 并拒绝，
  因此这条命令今天停在 `GGSR_AUTHORITY`，`ggsh_state` 保持为零且不发 RM/SCM；
- 新增 `giulia_gpu_share_runtime.h` 与 7 组宿主用例：把"authority 在 `ggsh_init`
  之前"写成顺序约束；拒绝时 `ggsh_state` 保持为零、不产生 RM/SCM 流量，且允许
  带真实授权重试；`ggsr_finish` 的事务路径仍由 share 单元自己的脚本化用例覆盖；
- 新增 `giulia_gpu_share_snapshot.h` 与 10 组宿主用例：冻结已完成 share 的身份，
  每次回答前用 live 状态复核，偏差永久失效；只读记录字段，不发 HVC/SCM；
- 未启动手机、未访问 MMIO/HVC/SMC、未改写任何硬件状态；
- 所有阶段状态来自既有文档与源码，未新增"已验证"结论；
- `GGSK_SUBMIT`/`GGSK_SCANOUT`/`GGSK_METAL` 只有槽位与验收条件，没有实现。
