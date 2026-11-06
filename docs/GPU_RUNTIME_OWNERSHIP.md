# GPU 运行期接管审计与 power-only 准入条件

## 结论与范围

目标是 giulia / SM8650 / A750。审计基线为提交
`23eb8e624dfe86d2a4b82ac5f619cd5c15ba5b29`，不是手机当前硬件状态的证明。
原厂资料及摘要见 [机器可读清单](../hardware/gpu-runtime-ownership-audit.json)。

**现有启动链没有给 `GiuliaGPU` 一个实际注册的原厂运行期电源 source，也没有
完成 GPU 窗口、stream、中断的当前启动授权交接。现在不能进行 power-only
硬件测试，更不能直接绑定提交库。** 本次只修复已有运行库的逐回调丢权防护，
不把缺失的硬件能力包装成成功，不增加终端硬件操作。

后续已加入 [XNU 常驻 power-only 交接控制器](GPU_POWER_OWNER.md)，连接
IOService 的独立测试入口、staged broker 保留和撤权等待。它仍没有实际注册
的原厂 source，不能据此勾选下方硬件准入条件。本文表格描述默认启动路径；
该新增入口不会启动提交或改变默认 host-only 行为。

## 交接路径逐项审计

| 路径 | 代码证据 | 实际证明 | 不证明什么 |
|---|---|---|---|
| UEFI 电源准备 | `GiuliaInitrdDxe/GpuPower.c` 的 `Resolve/Resource/Prepare` | Boot Services 内调用原厂 ClockDxe，输出并封存 `gpp_report` | XNU 可以调用 ClockDxe、独立电源票据、运行期所有权 |
| UEFI 模块生命周期 | `GiuliaInitrdDxe.inf` 的 `MODULE_TYPE = DXE_DRIVER`；`GpuPower.c` 使用 `gBS/gDS` | 当前实现属于启动环境 | 引入 `UefiRuntimeServicesTableLib` 不会自动把 Clock 协议变成 Runtime Service |
| XNU 消费电源报告 | `ios16/platform/giulia_platform_power.h` 的 `ggph_decode` | 检查记录结构及准备结果 | `GGPH_PREPARED` 不是可撤销的 runtime lease |
| Gunyah 交接 | `GpuGunyahReport.h`、`giulia_gpu_gunyah_boot.h` | 本次 FDT 来源、报告 CRC、RM capability 和原始 SPI 描述 | 本机 hypervisor ABI、GPU MMIO/SID/IRQ 所有权、primary 角色 |
| Gunyah owner | `giulia_gpu_gunyah_owner.{h,c}` | 独立建立 RM 通道、RM 中断保留及查询生命周期 | RM 通道的 owner 不是 GPU 的 owner；查询得到 VMID/READY 不授予 GPU 资源 |
| fence 内存共享 | `giulia_gpu_share_impl.h::shareAuthorize` | 明确拒绝缺少 primary/carveout 证明的操作 | 不能用固定 self/peer VMID 或 `MEM_LOOKUP` 成功替代源内存所有权 |
| 平台 broker | `GiuliaPlatformResources.cpp` | 本 XNU 内参与者间的排他登记及代际检查 | 固件、远端 VM 或安全世界的交接授权 |
| IOService 阶段机 | `giulia_gpu_stack_impl.h` | 只注册 boot、firmware、host-memory 三项只读 provider | 没有 runtime power provider；`allow_device_writes=0` 保持不变 |

`shareAuthorize` 与 `ggrt_ops.authority` 是**不同的授权范围**：前者授权固定
fence carveout 的 primary 共享，后者授权 GPU 的运行期资源。
不能把前者改成返回成功来解决上电问题；也不要求 power-only 测试先执行
fence 内存共享。两个阶段分别取得需要的真实授权。

UEFI 规范明确终止 Boot Services 后不能继续调用它们。当前 provider 既没有
运行期调用 ABI，也没有其代码/数据/页表/锁/回调的常驻与地址转换契约。
**保留一个 `mClock` 指针或复制 DXE 函数指针到 ADT 不是可行接法。**
参见 [UEFI 2.10 Overview](https://uefi.org/specs/UEFI/2.10/02_Overview.html)。

## 需要独立证明的板级条件

1. **当前启动权属和撤销同步**：可核验的 firmware/hypervisor 交接路径，明确
   MMIO、非安全 SID、IRQ 的授权对象、范围、代际和丢权处理；撤销必须与
   唯一 provider executor 同步。DT、启动参数和可重复的 boot epoch 不单独授权。
2. **精确访问范围**：runtime broker 的 `0x03d00000/0xe0000` 只是保守排他
   包络，空洞不能访问。不能据此扩大到 GCC、AOP、PDC、IPC_CORE 或 QDSS；
   这些另需经过共享平台后端自己的范围/票据检查。SID 0/1/5 的登记不激活
   context，secure SID 2 不在本阶段接管范围。
3. **配置访问和 AOP 前置条件**：现有 `gg_power_ops.resolve` 是不改变硬件的
   解析操作，不能偷偷开启 CFG 或发送 AOP 请求。首次 CX 操作前必须已有
   可证明、保留到退出的 config-access 和匹配 ACD 策略；若没有，要先设计
   独立的有界事务及共享仲裁，不能把它们塞进只读 resolve。
4. **运行期电源/时钟后端**：CX、GMU 625 MHz、hub 200 MHz 与各分支的
   原厂父时钟、电压角、总线依赖、请求聚合和撤票语义。共享/继承票不能随意
   关闭；`IsClockEnabled`/CBCR 读回不等于取得了独立引用。
5. **停机证明**：保持 CM3 reset，不启动 SMMU/固件/CP；确认没有其他 master
   在访问相同资源。不能以一个 idle bit、fence 完成或 host-only 标志代替全局
   quiescence。不能为了读取 idle 去访问尚未授权或可能断电的窗口。
6. **IRQ 生命周期**：RM 的 TX/RX SPI 与 GPU、GMU SPI 不是同一组。
   后续 IRQ provider 必须绑定当前实际中断控制器和注册生命周期，不能把
   DT 中断编号、RM IRQ 保留或没有中断输出当成 GPU IRQ 租约。

原厂 `gen7_hwsched_gmu_first_boot` 经过 CX/时钟、固件、SMMU、IRQ 和 DDR
带宽等配套路径，不是往几个 CBCR 写 1。对应的 pin 和 SHA256 在清单中；
这些源码只能给出协议/依赖依据，不能证明正在运行的设备已交出资源。

## 本次修复：逐 provider 回调边界检查

旧 `giulia_gpu_runtime_acquire` 只在整批 `gg_power_acquire` 前后检查平台授权。
如果一个资源 acquire、observe 或 wait 返回时丢权，后面的资源操作乃至错误
回退仍可能先执行。release 也不能仅靠进入时的一次 live 检查。

现有真实 C 运行库现改为：

- 在 resolve、quiescent、每次 acquire/release、observe、wait **前后**验证
  同一 epoch 的平台授权和完整 broker bundle；不依赖尚未设置的 `ready`。
- 丢权即隔离，清除两个 ready 标志，不调用下一个硬件 callback，禁止未经
  授权的 rollback；保留 epoch、已得 token、映射及任何 DMA 标记到 reset。
- callback 返回的 token 不因随后丢权而丢弃；返回成功也不能覆盖丢权结论。
- drain 的 quiescent 也走相同检查，丢权时不清 DMA 标记。
- 保持外部 C ABI、资源 ID、原有 host-only/写入策略不变。trace 仅做 CPU
  日志，用原 provider cookie，允许在隔离后记录，不能执行硬件访问或重入。

这些前后检查**不能消除检查与硬件访问之间的并发撤销竞争**；真实 provider
仍须提供完整调用期间的授权与撤销同步。不能将此补丁当成所有权后端。

故障注入覆盖每个回调边界的平台丢权、四个 broker token 分别失效、资源失败
与同时丢权、反序 release 及 drain 中丢权。执行真实 runtime/submission C
代码和真实 broker 算法，硬件 callback 是替身；不是 MMIO 或 GPU 执行测试。

GCC 两路 GPU 父时钟分支已新增独立的固定寄存器事务和 native executor，
详见 [GPU_GCC_PARENTS.md](GPU_GCC_PARENTS.md)。它仍无真实平台授权源或
生产调用者，也不覆盖 PLL、供电和 GPUCC；下列准入项不能据此勾选。

ARC 逻辑等级／Command DB 的有界解析与离线规划见
[GPU_ARC_LEVELS.md](GPU_ARC_LEVELS.md)。它没有 RPMh transport 或当前启动
授权，不代表电压 provider 已实现，也不能据此放宽真机准入。

## power-only 测试的准入清单

下面全部获得证据之前，不打包启用硬件访问的测试镜像：

- [ ] 当前启动 GPU 权属、范围和同步撤销证据。
- [ ] CFG/ACD 前置条件及独立平台租约；没有继承票被错误撤销。
- [ ] XNU 常驻电源/时钟 provider；父时钟、电压角、带宽策略与失败回退有依据。
- [ ] 安全可访问的状态观测与保持 reset/全局静默证明。
- [ ] 串行 worker、stop/fault/丢权、资源保留生命周期完整接入 IOService。
- [ ] 宿主故障注入与原生 XNU 编译/链接检查通过。

全部通过后，测试仍仅使用目标设备的临时启动；SMMU context、GMU firmware
执行、CP_INIT、IB 和用户提交均关闭，不改 userdata，不重做根卷。
首先验证设备持续响应、真实状态和可证明的退出路径，再进入后续阶段。
