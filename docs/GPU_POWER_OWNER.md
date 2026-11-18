# XNU 常驻 power-only 交接控制器（原厂硬件 source 尚缺）

## 已实现和未实现

`giulia_gpu_power_owner.cpp` 是实际纳入 XNU 的常驻控制器，不是另一个文档
状态机。它调用已有的真实 runtime/power 事务，保留 IOService 提供者和消费者，
复制 callback 表，并用一个永久 IOLock 串行化整个交接、上电、撤票、退出过程。

**它不是 SM8650 的底层电源/时钟驱动。当前仓库没有生产调用者注册
`ggpo_source`，所以没有真实原厂交接 source、没有启用 GPU 上电或加速。**
take/live/drop 和 power callbacks 在宿主测试中是标注的替身；测试通过不能
当作手机权属、CX/clock 操作、GMU 执行或性能改善。

## 内核内部 source 契约

原厂 source 必须是本次启动中真实的、常驻的 IOService。注册 API 不对 USB、
用户客户端、ADT、UEFI function pointer 或可序列化 blob 开放。

- `take(epoch, scope, grant)` 真正执行 firmware/VM 资源交接，固定存储 grant。
  `UNCHANGED` 必须保证没有任何变化并返回全零 grant。未知结果、错误 grant、
  变更后声称 UNCHANGED 都进入隔离，不能乐观地调用 drop 或释放引用。
- `live(grant)` 只读验证本次 boot、native serial、self VMID、所有窗口、SID、
  中断和前置租约，必须恰好返回 1。callback 不能用上述字段自我认证。
- `drop(grant)` 只在全部 power tokens 已安全撤销后释放交接和前置租约；
  非零或同时发生本地失权都保持 source/consumer/grant 固定到 reset。
- `power` 是 `gg_power_ops`：原厂板级父时钟、供电、电压角、总线聚合、
  refcounted votes、失败返回语义及安全状态观测都由实际 native source 实现。
  不能把寄存器 enable 位、UEFI PREPARED 或回调返回常量 1 当成独立租约。
- 全部回调同步且非重入；callback/cookie、模块代码及原生资源必须常驻。
  不能在 callback 中重入控制器。只在进程上下文调用 API，不能在 IRQ 里
  等待 IOLock；IRQ 通知应排入提供者自己的常驻进程 worker。
- retain 只保证 IOService 对象存活，不会自动阻止其 stop 释放 cookie、映射或
  时钟引用。源的 stop/free 必须遵守 unregister 的 BUSY 和隔离状态；只有
  unregister 成功后才可销毁 backing。对不满足该约束的源不能进行注册。

scope 严格匹配已捕获的 GPU、GMU、GMU AO、GPUCC、GPU SMMU、GCC GPU
clock 页。其非安全 SIDs 仅 0/1/5；INTIDs 为 SPI+32，即 332/336/337。
资源范围描述不代表授予了其中空洞的访问，更不包含 PDC/QDSS、任意 GCC、
安全 SID 2、AOP 或全局 PLL 控制权。CFG/ACD/parent/voltage/bandwidth/reset
标志是源的验证义务，不是权限位；实际共享平台依赖必须由源独立持票。
测试对照 `hardware/sm8650-gpu.json` 核对六个范围和中断/SID，避免手抄漂移。
runtime 的公共排他 bundle 只覆盖 GPU/GMU/GPUCC/SMMU 包络和三个非安全
SID，不是整个 scope 的权限管理器。源还必须在真正交接前通过同一个平台
broker 协调 GCC 页及共享依赖，并通过原生中断管理器持有三个 INTID；这些
额外租约须保留至 drop 完成。不能把 runtime reserve 当作 GCC/IRQ 所有权。

## 生命周期与撤权

1. `initialize` 在进程上下文只分配一个永久 IOLock；status/拒绝路径不分配。
2. register 复制 ops 并 retain source。每次启动只允许注册一次，不热替换源。
3. acquire 保留 consumer，先用 `giulia_gpu_runtime_reserve` 在全局 broker
   排除冲突，再调用真实 take，然后验证 grant/live，再调用
   `giulia_gpu_runtime_acquire`。reserve 只登记软件排他，不调用硬件。
   runtime 在每个 power callback 前后重查权限
   和平台 broker。全局 broker 仍是唯一账本，不创建私有的第二个账本。
4. release 持同一 executor，确认静默、反序撤销自身票，再 drop，最后通过
   `runtime_cancel` 撤销软件排他并 release consumer。staged 运行期在电源
   失败回退或撤电后仍保留 broker bundle，避免交接前和 drop 期间出现窗口。
   IOService release 在解锁之后执行，避免析构回调死锁。
5. invalidate 先原子 latch，再等待 executor；返回时已无本地 callback 执行。
   正在执行的 callback 返回后不会再进入下一次硬件回调，也不进行失权后的
   rollback。未知资源和引用一直保留。unregister 对这种状态返回 BUSY。

**本地锁无法阻止 hypervisor 在 MMIO 中途强制撤权。** 原厂 source 必须在
上游撤权前完成此协调，或提供覆盖硬件访问的原生 guard；无法保证时不能注册
为可用源。本接口不发明 hypercall，也不把 Gunyah RM 通道 owner 当 GPU owner。

status 为 CPU cached snapshot，不调用 live 或硬件；`registered` 不等于取得
grant，`ready_cached` 也不是新的硬件观测。只有完成 take/live 和原厂 power
事务才可置 ready。任一不确定结果不允许重试；当前控制器为单次 power-only
生命周期，不是通用 suspend/resume 或热重连实现。

## GiuliaGPU 接入与测试隔离

默认启动不改变行为。独立入口只在以下条件同时满足时被接受：

```
giulia_gpu=3 giulia_debug=1 giulia_gpu_hostonly=0 giulia_gpu_power_test=1
```

**这些参数不是推荐现在刷机的命令；原厂 source 尚未实现，仍会拒绝上电。**
启用此入口时，现有 GiuliaGPU worker 只调用 native power coordinator 和
status/stack，屏蔽 legacy DEV、observe、HFI、GMU、Gunyah periodic 和内存请求
pump，不因没有 source 或失败而回落到旧硬件路径。stop 在 worker 内结束
power 生命周期后再唤醒 IOService stop；隔离时保留 host backing。

独立的 raw MMIO 终端也检查该启动标志：非零时全部 `reg.*` 在映射、读写或
解除映射之前拒绝，即使原启动参数还保留 `giulia_mmio=1`，或 GPU service
匹配失败，也不能从终端绕过隔离。已有映射同样不能被使用。

完整 15 阶段 stack 仍是独立的只读规划；power-only 入口不把后续 provider
自动注册成成功，也不启动 SMMU/context、固件/CP、IB、fence 或 Metal。
`gpu.status` 为此入口单独报告真实的 coordinator cached state；旧默认路径仍
报告未绑定。没有添加可从终端注入 grant 的新命令。

## 验证

- `tests/ios16_gpu_power_owner.py` 编译并执行实际 `.cpp` 和真实 C runtime。
  只有 IOLock/IOService、原厂 source 和硬件 callbacks 被替换为 CPU doubles。
- 覆盖完整成功路径、11 资源顺序、错误 grant、清洁/不确定失败、错误消费者、
  status 无 I/O、单次注册、引用保留、反序撤票和并发撤权等待。
- `tests/ios16_gpu_service.py` 执行实际 worker/stop 和接入方法，另行验证
  无 source 拒绝、legacy 路径隔离和失败 stop 的内存保留。
- GPU post-link checker 追加 7 个真实 C-linkage coordinator 定义和 2 个
  staged runtime 定义要求，不能仅凭日志字符串通过；旧 59 个定义要求完整
  保留，此 coordinator 版本总计 68。后续
  [GCC 父分支子集](GPU_GCC_PARENTS.md) 再追加 12 个定义，当前总计 80；
  该子集没有注册到本 coordinator，不代表完整 source 已存在。

下一步仍需实现并证实原厂 source：真正的 firmware/VM 交接路径、常驻
clock/regulator/带宽策略及配置可访问性。完成前不安排 power-only 真机测试。
