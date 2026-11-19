# GPU Gunyah 传输与查询会话

当前代码已连接内核 HVC、RM 分片传输、七步只读查询、空闲通知收包和
primary 共享内存事务。本次又接通了真实启动 FDT 的资源交接、独立
hypervisor 兼容性 probe 和单 CPU GIC 轮询租约。这些环节现已接到
`GiuliaGPU` 的同一运行时 worker，查询或共享结果尚未转换为 fence 提供者。默认
kind23 仍要求另一个真实平台提供者注册，不会自动执行 HVC。

固定原厂版本与逐文件摘要见
[Gunyah 源码记录](../hardware/gpu-gunyah-source.json)。协议实现位于
`ios16/giulia_gpu_gunyah_{hvc,rm,queries,session,memory,share}.{h,c}`；这些文件由 GPU
补丁生成器带入内核，三个 C 单元按照内核的 iokitcpp 配置编译为 C++，
对外接口保留 C linkage。内核产物检查要求这些实现的可执行符号存在。

## 本次启动资源与平台所有权

UEFI `GiuliaInitrdDxe` 从实际 entry HOB 的原始 FDT 提取唯一 RM 节点。
它先以 UEFI memory map 证明 header 和完整 FDT 属于可读普通内存，再
使用真实 FdtLib 验证树、重复属性、继承 interrupt-parent、GIC 控制器、
64 位 capability 和 SPI spec。本板 RM 实际位于 `/hypervisor` 的 2/0
capability 总线；这些 reg 值不能解释成 MMIO 地址。冻结报告包含实际
FDT CRC/长度、节点偏移、本次 monotonic epoch、两枚 capability 和原始
IRQ spec，无 READY、IRQ 租约或 CPU 指针。
DXE 依赖 monotonic-counter architectural protocol 完成注册；本板计数器
首个成功值可以为 0，提取器最多读取两次以获得实际非零本地 generation。
该计数器冷启动可重复，不是跨启动认证或可单独授予 runtime lease 的 nonce。

GPU 打包流程为 ADT `/chosen/giulia-gpu-gunyah` 预留 256 字节请求，
DarwinBoot 在最终 ADT copy 后、GetMap/EBS 前通过 BootServices 协议
填入报告。只有 exact GPU mode1/2/3 token 和 pristine slot 可调用；缺失
provider、畸形结果或旧 CRC-valid 结果都不能留下可接受的旧报告。XNU
实际 `GiuliaGPU::start` 读取该属性并记录结构校验结果，绝不将它直接
用作 HVC 的 live 回调。主机 payload 和最终 firmware 检查同时核对报告
槽、kernel consumer 及 provider/loader 能力，旧产物不能混装。

`ggh_hvc_probe_compatibility` 在独立且真实的 boot probe lease 下最多
执行 UID `0x8600ff01` 和 IDENTIFY `0xc6008000` 两次 canonical HVC。
按固定源码字节序匹配 Gunyah UUID，要求 API1/LE/64-bit ABI，完整保留
x0–x7，失租/ABI 错误后隔离。它不自动执行 MSGQ 或取得 capability 租约。

`0085-giulia-gunyah-irq.patch` 把实际平台钩子装入 `pe_fiq.c`、`sleh.c`
和 `ml_install_interrupt_handler`。claim 使用本次报告转换的动态 INTID，
检查单 CPU/boot CPU/线程上下文/真实 GICD 窗口，拒绝已有 generic handler。
在本地 IRQ 关闭时仅对两枚 SPI 写 ICENABLER，等待有界 RWP，再验证
ISENABLER 和 ISACTIVER；pending 只观察，不 ACK/清除。失败保留 mask，
新 generic handler、控制器重初始化或 reserved IRQ 误入会撤销租约。
`guard_begin/end` 可在一次不睡眠的实际 HVC 前后保持 IRQ 关闭并复核。
当前不支持与已注册 generic interrupt controller 共存，也不支持多 CPU；
这些检查不能用 boot FDT 数据或 EBS 默认状态代替。

静态平台补丁来自固定 XNU `1b191cb58250d0705d8a51287127505aa4bc0789`
加本工程 Sweet 平台补丁，由 `ios16_gpu_gunyah_irq_patch.py` 精确生成，
有实际源码 `git apply --check` 和 AArch64 glue 编译验证。完整 XNU 链接
仍须另行完成；当前没有运行这些 native GIC/HVC 操作。

## 调用与生命周期

`ggown_state` 现在由实际 GPU worker 的 OP22 显式创建并保持到重启。
策略要求 worker 入口记录的线程身份、已 pin 的资源、当前启动报告、
未隔离平台和原子 stop latch；命令参数不能指定 capability 或 IRQ。
bootstrap 执行真实 GIC claim 和 UUID/API probe；每次实际 HVC 都在
native IRQ guard 中，输出只在 guard_end 成功后交给 RM。等待/通知处理
在 guard 外。stop 在取得 worker 锁以前先撤销策略和 owner。

OP22 action0 只读历史状态；action1 以 b 毫秒执行完整查询；action2 使用
b 帧、c 毫秒预算接收通知；action3 仅 bootstrap；action4 撤销。超时范围
为 1–3000 ms。查询成功后 worker 周期性 poll 生命周期；失效后阻止后续
修改和 HFI。bootstrap-only 可借用同一 RM 序号空间供显式共享事务使用，
不会把 MEM_LOOKUP 不存在当作隐式赋权理由，也不自动注册 fence。

真正拥有通道的内核 worker 在堆上分配、清零并固定 `ggrs_session`，然后
使用 `ggrs_init_hvc` 复制 HVC 描述符、时钟/延迟/通知回调。测试或已有
传输所有者也可用 `ggrs_init` 导入相同语义的消息队列接口。初始化回调
不能重入发布对象。没有终端注册入口或设备树 capability 默认值。

`live` 必须核对当次启动的独占 TX/RX capability、epoch、已确认的
hypervisor ABI 以及 IRQ/轮询所有权；所有回调和 backing 必须保持有效。
通道的其他消费者不能同时收包。capability 是 64 位句柄，不能当作 MMIO
物理地址；0 是允许值，`U64_MAX` 为固定源码的 invalid 值。

`ggrs_probe` 在整个查询尝试的单一截止时间内依次执行：

1. 查询本 VMID。
2. 按完整 `qcom,cpusysvm` 名称查询动态 peer VMID。
3. 核对 peer 的名称身份。
4. 查询 peer READY。
5. 按准确 ACL、label5、物理范围查询 parcel 句柄。
6. 从本 VM 资源表找到 peer 对应 label6 的 RX doorbell。
7. 再查询 peer READY。

每一步匹配 RM reply 的 sequence/message ID/error，再解析完整 payload。
RM 初始 reply 为 12 字节，普通头为 8 字节，最大帧为 240 字节；分片
计数表示 continuation 数量。发送只在最后一帧设置 PUSH。每个 epoch
允许 sequence 0–65535 各使用一次，耗尽后拒绝复用。未完成分片被其他
初始消息打断时隔离，避免静默丢弃回复或通知。

仅明确的 FULL/EMPTY 可在时间和轮询上限内重试。成功 send 后 ready 为
false 仍表示已经接受，不可重发。HVC 状态和 RM error 分开记录，保留
完整 64 位 HVC 诊断。回调或 HVC 自身不返回时，软件截止时间无法抢占。

超时、时钟倒退、畸形包、未知通知、断流或租约丢失会撤销 transcript 并
隔离。显式 `ggrs_invalidate` 即使遇到活动 worker 也会锁存失效；活动操作
在发布结果前处理。一个会话只允许一次查询尝试，失败后不能在同一对象
上重试。初始化、查询和输出复制均防止回调重入。

已完成查询以后，通道所有者仍须持续接收并交付 lifecycle 通知。
`ggrs_notification` 同时调用原所有者的通知 handler；peer/self 的任何
状态事件，包括再次 READY，都会撤销查询结果。不相关且格式正确的 VM
状态通知可保留当前 transcript；不能把空闲期间未消费的通知当作不存在。

`ggrs_poll(session,frame_budget,timeout_us)` 在同一 worker 上仅接收通知，
不发请求或分配 sequence。实际观察到 EMPTY 且没有半条消息时返回
`GGRS_IDLE`；处理完预算但尚未观察 EMPTY 时返回 `GGRS_BUDGET`。两者仅
表示本地进度。分片通知必须在本次预算/截止时间内完整收取；半条消息、
迟到 reply/request、未知通知、畸形包或租约失效会撤销 transcript 并隔离。
IDLE 不证明远端仍 READY，也不允许省略真实 owner 的通知/IRQ 生命周期。

`ggrs_copy_snapshot` 只导出查询记录。它不证明内存 share/accept、CPU
Normal NC 映射、控制器页 ABI、CTX0 注册、IRQ 绑定或 fence 完成，不能
直接产生 `GGFP_REQUIRED`，也不调用 `giulia_gpu_fence_register`。

## Primary 共享内存事务

固定 fence primary 路径先 SCM assignment，再 `MEM_SHARE`，没有
`MEM_ACCEPT` 或 `MEM_NOTIFY`。`ggsh_init` / `ggsh_run` 实现此顺序；其
owner 必须持有覆盖整个事务的 RM 通道、全局 SCM、carveout 和控制内存
独占租约，实时证明 primary 身份及精确 carveout 的初始独占所有权。
它不获取或假定这些权限，不与其他 cpusys sharing driver 并发执行。

事务先发四次动态 VM 身份/READY 查询，再确认 SCM64 和 assignment
服务可用。独立且保持至重启的 16 KiB Normal NC 控制分配中放置 16 字节
region、一个 u32 source VMID、两个 24 字节 destination permission。
region 始终为 `0xd4e23000 / 0x2dd000`；destination 为动态 self 和 peer
各 RW6。控制区 PA/CPU 指针、范围、对齐和 carveout 交叠均检查。
该 carveout 按原厂 4 KiB 几何使用，不套用普通 secure 池的 16 KiB 限制。
VMID 范围限制为固定原厂 u64 bitmap 可表示的 1–63，不能将 enum3 当 peer。

SCM function 为 `0x42000c16`、arginfo 为 `0x1117`；BUSY/INTERRUPTED
复用已有有界 SCM exchange，每次进入赋权前重新核对授权、租约和完整
控制描述符。原始返回、transport status 与 RM error 分开保留。状态在
首次可能赋权前标记 retained，赋权成功后构建准确 44 字节 NORMAL/flags0/
label5 的 `MEM_SHARE`（self/peer RW ACL、一个精确 SGL、空属性表）。
随后 exact `MEM_LOOKUP` 必须返回相同 parcel，再重查 peer READY。

该事务沿用真实 owner 的 `ggrm_state` 和 sequence，不另建同 epoch 的
序号空间；最多七个 RPC，检查其他消费者导致的 epoch/quarantine/sequence
变化。整体单调截止时间覆盖所有 RPC、SCM、授权和发布回调。任何失败后
隔离；赋权后失败保留所有资源，不自动 rollback/reclaim/unshare 或重放。
成功后的显式失效立即清除共享结果；忙时先原子锁存，再由 worker 清除。

事务不读取、清零或初始化 carveout/queue，不建立 CPU/GPU/GMU 映射，
也不代表 peer acceptance、控制器页 ABI 或 CTX0 就绪。成功记录仍不能
注册 fence 提供者。独立 memory codec 同时提供通知编解码，但没有把
`MEM_NOTIFY` 加入 primary 路径。固定源码只给出 `MEM_SHARED` 的 20 字节
前缀，变长尾部排列仍为 TODO；完整解析返回 unsupported，绝不推导共享
范围或接受证明。`ACCEPTED/RELEASED` 的已知 12 字节格式则严格核对
parcel、动态 participant 和显式 mem_info_tag。

## HVC 与 doorbell

### 独立查询本次启动的 VM 身份

`ggown_query_self` 在现有独占通道和 owning worker 上执行一次固定
`VM_GET_VMID`。该入口复用 UID/API 检查、IRQ guard、RM 接收器和序号空间。
需要时先执行 bootstrap。整个调用使用一个单调截止时间。

该查询独立于 peer、parcel 和 fence 的七步 discovery，允许在供电接管前
取得候选启动的真实 self VMID。后续完整 discovery 继续使用递增序号。
查询失败后保留通道并使 owner 失效至 reset，禁止重试。

已有 DEV 调度中的 action 5 接入该入口，参数 b 为 1 至 3000 毫秒。
`gpu.dev 1 22 5 1000 0 0` 请求查询，action 0 报告 `self_attempted`、
`self_observed`、`self_vmid`、RPC 结果和远端错误。结果保留历史属性，
需要结合 owner 的 invalid 状态理解。GPU 资源授权仍由平台 source 提供。

该入口沿用已有 worker、内存固定及启动模式条件。host-only 和 power-only
入口继续拒绝全部 DEV 调度。状态命令只读取 CPU 缓存结果。

`make-ios16-payload.py --gpu-mode 3 --gpu-rm-diagnostic --debug-terminal`
支持无根卷的 RM 诊断打包。该配置要求新内核中的独立 self 查询，以及
本次启动的 Gunyah、ARC 状态和供电结果请求。打包器核对 UEFI 的匹配 GPU
供电准备功能不可用。普通 mode 3 的固件准备检查继续执行原有条件。

诊断配置使用 `giulia_gpu_rm_diagnostic=1` 标记和已有 mode-3 owning worker。
实验步骤为固定 RAM backing、单次 action 5 查询和 action 0 状态读取。
该标记把 DEV 调度限制为这两个 RM action，保留 CPU 状态、stack 和 RAM pin。
GPU 观察、HFI pump、GMU、共享和原始 MMIO 调度均在执行前拒绝。

完成 UFS 只读探测后，该明确配置使用现有 SPMI restart reason 与 PSCI
SYSTEM_RESET 退出至 Fastboot。可写存储、MMIO、power-only、混合或未知配置
仍由退出检查拒绝。RM 状态与 backing 保留到整机 reset。主机回归测试编译
实际启动参数判定函数，覆盖 1152 个参数组合。真实退出结果按候选启动验收。

canonical MSGQ 使用 `hvc #0`，固定 function ID 为 `0xc600801b/c`。
发送和接收均使用私有 scratch buffer，接收仅在成功且租约仍有效时
发布输出，失败保留调用者数据。

固定 fence driver 的 doorbell 是另一个 legacy ABI：`hvc #0x6013`。
`ggh_hvc_doorbell_read_clean` 使用独立描述符和 `live` 回调，后者必须
证明该 legacy ABI、实际 RX capability 和独占 IRQ 所有权。canonical
MSGQ 可用不代表 doorbell 可用，不按编号推导 `0xc6008013`。read-and-clean
可能已经清除了标志；不确定失败不得自动重放。两个 native primitive
均保存、恢复 Darwin 的 x18，并清零临时保存槽。

## 验证

`bash scripts/test-ios16-gpu.sh` 包含 query、RM、HVC、session 和 fence
queue、memory 和 share 的 ASan/UBSan 测试；session 测试连接真实 HVC adapter、RM 和 query
实现，仅将 HVC 指令替换为明确标记的主机 fixture。覆盖动态身份、冻结
回调、故障/通知、回调内失效、总截止时间和时钟倒退。

`python3 tests/ios16_gpu_gunyah_build.py` 从实际生成的内核补丁取源，交叉
编译 Darwin arm64 C++ 和 AArch64 C，核对 HVC 指令、x18 保存及外部符号。
平台头文件为测试 stub；这不等于完整 XNU 编译或内核链接。所有验证均为
离线检查，没有执行目标 HVC、SMC、MMIO、RAW 或 fence 提交。

2026-10-23 后续验证日志为 `reports/gpu-gunyah-share-20261023/`：完整 GPU
runner（含 890 控制场景、新共享事务与 idle pump）通过，Python 271 项中
268 项通过、3 项条件跳过，八种配置补丁应用及三单元交叉编译通过。
22 份固定原厂源码长度、SHA-256、Git blob 已逐字节核验。

启动资源接线的后续日志在 `reports/gpu-gunyah-boot-20261023/`：完整 GPU
runner 通过；当前全仓 281 项中 278 项通过、3 项条件跳过；GPU/Wi-Fi/Audio
八种配置及九项打包测试通过。真实 vendored libfdt 成功提取 898,526 字节
完整板级 DTB；398 项 loader/decode 场景、native GIC glue RAM shim、
真实源码静态补丁核验通过。UEFI provider/loader 使用实际 Mu 头文件生成
ARM64 COFF，kernel transport/glue 也通过 AArch64 交叉编译。计数器合法
首值 0 的修复另跑完整 FDT、loader 和 native 编译验证。
这些证据仍不包括完整固件/内核链接、实机 GIC 租约或 fence 完成。

下一步仍需共享内存事务的运行时接入、控制器相邻页授权，以及 fence 表句柄、
跨 VM 锁、错误和复位处理；CPU Normal NC 映射与 pmap 审计本身已实现为
[CPU pmap 门控](GPU_CPU_PMAP.md)，但没有真实 share 快照时不会发起导入。
此前 HFI kind11–13/20 的排除已依据固定 gen7 的真实 SYSRAM
分支纠正，详见 [映射记录](GPU_MAPPING.md)。
