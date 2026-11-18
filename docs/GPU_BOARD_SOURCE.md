# Ace5 最小板级 source 与固定 CX/GDSC、时钟执行器

## 当前范围

`giulia_gpu_board_source.c/.h` 将真实 CX/GDSC、PLL1、RCG、GPUCC、GCC C 执行器接到
`ggpo_source.power`。这是一个可被原生调用者使用的**固定、保留到reset的组合层**，
电压、GCC父时钟、共享ARC、带宽及当前启动交接仍由真实上游提供。没有自动初始化/注册调用者，没有更改默认host-only启动。

不能把它称为已完成的原生电源接管。没有上游就不能注册为可用源，更不能打包
声称已经可以上电的镜像。主机模型、ARM64对象和注册函数存在都不是硬件授权。

## 固定资源执行

沿用 `gg_power_acquire` 的11项顺序和外部资源ID：

| 资源 | 本组合层的实际执行 |
|---|---|
| CX | 执行固定 GDSC 软件控制、独立状态轮询和 retain FF 事务；供电/CFG/ACD 由上游持续持有 |
| GMU rate | RCG停车XO → 已校准PLL1启动1250MHz → RCG切625MHz |
| HUB rate | RCG切200MHz，GPLL0等旧/新父时钟由上游持有 |
| GMU/CXO/AHB/HUB_AON/SMMU clocks | 调用真实GPUCC分支执行器 |
| AXI/MEMNOC/DVM | 调用固定GCC分支执行器，独立GCC页I/O仍由真实上游提供 |

本地器件访问额外限制为固定GPUCC寄存器白名单；不提供任意地址、任意频率接口。
只读模拟配置不允许写入。HUB divider、全部PLL1消费者停车、校准、CX和安全访问
等前提仍必须由真实source在整个会话中保障；不是读一个bit或local lock就成立。

当前FDT的GMU `axi_clk`指向GCC clock ID14。原生上游应按实际绑定和公开GCC源
核对父时钟路线，不得未经核对套用UEFI ClockDxe的`gcc_ddrss_gpu_axi_clk`名字。
这里不改已知UEFI启动基线，也不声称两个provider的名称空间等价。

不做QDSS跟踪，所以不增加其调试`apb_pclk`操作；实际CFG访问保障仍必需。
四项 GCC 核心与版本策略见 [GPU_GCC_BRANCHES.md](GPU_GCC_BRANCHES.md)。
CFG 仍由上游在明确准备阶段建立并保留，不放入只读 resolve 或本组合层的 acquire。
不做DVFS、低功耗、休眠、多队列、GMU固件启动、SMMU context启用或其他外设。

## 交接与生命周期

1. `ggbs_init`仅复制真实上游和I/O表；`ggbs_export`导出组合后的`ggpo_source`。
2. 原生服务可调用`giulia_gpu_board_source_register`接入既有owner。
   实际IOService、回调、cookie、映射和供电backing的寿命必须满足owner约束。
   代码没有这个实际调用者，不能把“新增注册函数”写成“实际已注册”。
3. `take`先调用上游真正的交接；原样传递grant，不从DT/Android报告生成授权。
   不完整/矛盾/不确定的grant保持保留并隔离。只有成功后才打开子执行器。
4. 每个实际I/O/委托回调前后检查上游live；上游还必须提供覆盖整个会话的
   持久guard，阻止访问期间被撤权。反复检查不能消除TOCTOU或捕获external abort。
5. 每个资源有单独的本地客户端token和原始上游token，不假设上游token的高位可用。
   发起不确定操作前发布并保留token；不会因随后失权而抹掉。
6. `invalidate`仅原子锁存丢权。上游撤权/销毁前仍须调用owner invalidate并等待
   executor退出；不能仅retain一个IOService就释放其映射。

上游可以来自经核验的当前启动静态资源分配，也可以是平台实际支持的交接协议；
不强行要求额外发明hypercall。但当前代码没有任何一条已证明的途径。
RM通道、VMID/READY查询、UEFI Boot Services报告和软件broker均不能替代它。

## 保留型测试，不伪装成可正常撤电

PLL1执行器不支持shutdown，AON分支也不能按普通CBCR撤销。所以本组合层在第一次
本地 CX/时钟事务前锁存`retained`，此后任何release/drop都在实际I/O/上游撤票前拒绝。
失败或stop时保留整组backing直到reset，不存在乐观回滚或返回虚假的清洁释放成功。
这不是内存泄漏可忽略的通用运行期驱动；源/映射/票据必须确实能保留直到reset。

若要进行这种一次性上电测试，真实上游必须明确支持并实测保留策略及reset退出。
若要求清洁stop，则必须先补PLL/AON等可证明的完整关闭路径。两者目前都未在真机
验收，本组合层不会自行授予镜像生成或启动许可。

## 源码接口修复与验证边界

此前RCG的`GGRC_OK`与power接口的同名enum冲突，只分别编译时不会暴露。
RCG常量改为`GGRCLK_*`，函数名和enum数值不变；不要加回导致冲突的旧名别名。
本测试同时包含全部实际头文件，并把真实C对象链接到C++调用者，防止该问题复发。

`tests/ios16_gpu_board_source.py`执行真实board/CX/PLL1/RCG/GPUCC/GCC代码，只有原生
授权、I/O和注册对象是HOST doubles。覆盖11项执行顺序、每个I/O/wait及live失效
边界、不完整grant、需校准拒绝、超时、token保留和禁止撤票。
ARM64对象检查不等于原生XNU头文件ABI、完整内核链接或真机运行。

旧的“无任何子执行器调用者”检查仅允许本组合文件这一明确调用者，同时新增
检查禁止生产代码在未实现上游的情况下自动init/register本组合层。
后链接检查包含6个组合层定义和5个新增GCC定义，加上5个CX定义后现要求136个；旧内核不是本轮新内核。

`scripts/check-ios16-gpu-board-source.py`仅输出离线缺口清单并返回非零，不能
仅凭调用者/marker/JSON true授权上电。本轮在真实上游未具备时应停止镜像打包。

CX 固定步骤、版本依据和测试见 [GPU_CX_GDSC.md](GPU_CX_GDSC.md)。
11项固定资源均有本地执行路径，原生上游仍须提供真实供电、映射、授权、持有及退出服务。
