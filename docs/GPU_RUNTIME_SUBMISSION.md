# GPU 运行期租约与受限命令提交（未启用硬件）

本轮新增 `giulia_gpu_runtime.{h,c}`、`giulia_gpu_submit.{h,c}`。
这是**可编译、可进行宿主故障注入测试的运行库**，不是已经完成的 GPU 驱动。
`GiuliaGPU` 尚未绑定这些接口；`gpu.status` 明确输出两个 `provider_bound=0`。
没有新终端执行命令，没有放宽 host-only 或阶段机的设备写入策略。

后续增加的 [resident power-only coordinator](GPU_POWER_OWNER.md) 通过
`runtime_reserve/cancel` 把软件排他扩展到 take/drop 交接事务两端，原有
非 staged acquire/release 行为保持不变。实际 native source 仍未注册，
提交库也仍未绑定；13 个 C API 链接测试不代表底层硬件后端已实现。

## 电源与资源生命周期

- 电源事务补上独立 `GGR_DVM_CLK`：原有只读观察要求 GCC SNOC DVM vote，
  但运行期 acquire 原来只覆盖 MEMNOC。新 ID 追加，既有资源编号不变。
- 严格区分当前启动的**平台授权**、全局 broker 的**软件排他登记**、provider 的
  **硬件电源租约**。三者缺一不可；UEFI READY/Android DT/软件 claim 都不单独授权 DMA。
- 当前 runtime 使用一个保守 MMIO 排他包络 `0x03d00000/0xe0000`，涵盖捕获的
  GPU/GMU/GPUCC/GPU-SMMU 窗口，避免 GMU/GPUCC 相互重叠导致自冲突。
  **包络中的空洞仍禁止访问**。SID 0、1、5 单独在共享 broker 中登记。
  不安装 SMMU 配置、不激活 SID，也不接管 secure SID 2。
- 平台 `authority` 必须证明本次启动的角色、精确窗口/stream/IRQ 所有权，
  并在同一串行 executor 上处理撤销。软件 claim 不能取代 hypervisor/firmware 交接。
- 每个 power callback（包括 resolve、quiescent、poll/wait 和反序 release）
  前后都重查授权与完整 broker bundle；丢失则停止后续硬件调用并隔离，不能
  继续回退撤票。该防护不能替代 provider 对并发撤销的同步。
- 成功取得全包资源后才运行 `gg_power_ops`；缺少任何 callback 或授权直接拒绝。
  确定未改变硬件的失败才按反序清理；未知电源结果或租约丢失保持资源隔离到 reset。
- 队列索引发布前标记 DMA。命令 fence 完成**不等于 GPU/GMU 全局 DMA 已排空**；
  必须由 provider 实际确认 quiescence，才清 DMA 标记、撤电源及释放 broker 记录。

所有 state 初始必须清零，地址不能移动；broker 使用 state 地址作为 owner。
callbacks、cookie、内存及状态在提交后或结果不确定时必须固定到确认退出或重启。
调用者负责串行化；不允许在 provider callback 中重入该状态机。
kernel wrappers 使用平台唯一全局 broker，不允许创建另一个私有账本绕过其他驱动。

## 原生提交子集

原厂资料固定在 `hardware/gpu-submit-source.json`，来源为 OnePlusOSS 提交
`453d8875f815bf9dfad9035982861226a4823a6c` 的 `adreno_hfi.h` 和
`adreno_gen7_hwsched_hfi.c`；两文件均记录完整 SHA256。

- 实现消息 130 `ISSUE_CMD`，不是曾触发 900e 的消息 131 `ISSUE_CMD_RAW`/CP_INIT。
- 原厂 8 dword submit header，每个 IB 3 dword（64 位地址和**字节数**）。
  当前只接受 32 位 IOVA 表可覆盖的完整范围，最多四个可信 IB，一条 in-flight。
- legacy dispatch queue 3–16，16 字节存储对齐，`0xfafafafa` padding；发布索引
  前后有 barrier。门铃掩码为 `BIT(queue - 1)`，但只能由持有 GMU 所有权的
  provider 实际执行。不会向 control queue 0 误发 dispatch 包。
- `proof` 必须证明电源、IRQ、SMMU、GMU/HFI/CP、实际注册且绑定队列的
  nonsecure context、已禁用 context-queue 模式、真实 fence 后端的当前状态。
- `pin` 必须验证完整 IB/引用缓冲区、翻译和缓存，并阻止 CPU 改写与释放；
  同时建立本次 epoch/context/timestamp 的新鲜未触发 fence。
  尚未实现不可信 PM4 检查，所以不能向应用或任意调试输入开放此接口。
- publication 可先于门铃触发固件消费；一旦发布索引，禁止 cancel/retry。
  正常 send 返回仅表示接受，`poll` 收到匹配的**硬件 fence 成功**才记完成。
  不能使用 HFI ACK、read-index 前进、单独 TS_RETIRE 或人工置位冒充完成。
- 支持超时、单调时钟回退检测、epoch/proof 丢失、pin/门铃/fence 故障隔离；
  不复用环回的 12 位 HFI sequence 或 32 位 timestamp，耗尽需重建受控生命周期。

## 验证边界与后续任务

`tests/ios16_gpu_submission.c` 实际运行上述 C 代码与真实 broker 算法，硬件
provider 是故障注入替身。覆盖别名/带 mask SID 冲突、电源每个阶段失败与未知
结果、所有 dispatch queue/4-dword 环位置/1–4 IB、padding、队列满、DMA 保留、
发布前后丢权、门铃失败、pending/成功/错误 fence、时间回退/超时与编号耗尽。
它不证明 MMIO、固件执行或真实 GPU 完成。

新增 C 单元由既有 GPU patch composer 原生纳入 XNU 构建；post-link checker
要求这 11 个外部函数均在可执行段真实定义，而非只有头文件/日志字符串。

目前仍缺**真实板级电源/clock/IRQ provider**、受权资源交接、常驻 SMMU 与 CP
初始化闭环、context/fence 完成 provider。不得用常量成功补齐这些回调。
先完成这几个 provider 的硬件证据与撤销生命周期，再运行最小可信 IB，观察真实
fence 及输出数据，之后才谈显示合成/Metal 和流畅度。此次没有理由重刷根卷。

当前启动链/资源权属的逐项审计、与 primary fence 共享授权的区别，以及
power-only 测试准入条件见 [GPU_RUNTIME_OWNERSHIP.md](GPU_RUNTIME_OWNERSHIP.md)。
