# SM8650 GPUCC 关键分支时钟后端

本模块实现固定六个CBCR分支的有界寄存器事务，不是完整GPU power source。
**没有自动生产调用者，不映射任意地址，不注册GPU/Metal，不授予硬件控制权。**

## 本轮两个实际改动

1. `giulia_gpu_gpucc.c/.h`：独立实现AHB、CX GMU、CXO、GPU SMMU、HUB AON和
   HUB CX INT分支的RMW、厂商HALT规则、继承投票保护与失败保留。
2. 修正`giulia_gpu_gmu_boot.h`中AHB fence范围编码：原0x08008a32，与固定厂商
   `BIT(31) | FIELD_PREP(GENMASK(30,18),0x32) | FIELD_PREP(GENMASK(17,0),0x8a0)`
   不符。现按字段表达式编码，结果0x80c808a0。原测试将同一常量作为期望值，
   无法捕获此错误；新增独立的hash-pinned源字段oracle，修复前失败、修复后通过。

第二项是已确认的编码错误，但没有真机证据证明它就是此前启动或卡顿的根因。
oracle只验证本次检查的14个寄存器偏移和AHB字段/实际RAM写入，不宣称整个厂商
GMU初始化序列已完整移植。

## 固定硬件范围

GPUCC窗口0x03d90000/0x9800，所有偏移为字节；仅允许以下六项：

| ID | 名称 | 偏移 | 等待规则 | 允许关闭 |
|---|---|---|---|---|
| AHB | gpu_cc_ahb_clk | 0x911c | HALT_DELAY，10us | 是，仅自己改变的请求 |
| GMU | gpu_cc_cx_gmu_clk | 0x913c | VOTED，开启轮询 | 否，原厂AON ops无disable |
| CXO | gpu_cc_cxo_clk | 0x9144 | 普通HALT，开关均轮询 | 是，仅自己改变的请求 |
| SMMU | gpu_cc_hlos1_vote_gpu_smmu_clk | 0x7000 | 开启轮询；关闭延时10us | 是，但不宣称整个clock已OFF |
| HUB_AON | gpu_cc_hub_aon_clk | 0x93e8 | VOTED，开启轮询 | 否 |
| HUB_CX_INT | gpu_cc_hub_cx_int_clk | 0x9148 | VOTED，开启轮询 | 否 |

`hardware/gpu-gpucc-source.json`固定公开source commit、长度和SHA256。
RMW只改变bit0，保留其他字段；状态位可能异步变化，不要求整个CBCR等于写入值。
AHB延时策略允许enable位未锁存的历史设备表现，不将其误判为普通HALT分支。

## 前提与生命周期

- 调用者以真实XNU常驻source持有mapping、分支排他控制权、正确父频率/PLL、
  CX/电压/访问backing；有效分支频率至少1MHz，才使用200×1us的有界HALT轮询。
- `ready`只验证已有前提，不是“先写一遍看看”；本模块不编程rate、PLL、GDSC、
  RPMh、ACD、CFG或interconnect，不能单独组成`gg_power_ops`。
- 调用者串行化整个state，包括invalidate；回调不能重入。真实source可以通过
  原子live标志撤销访问，I/O前后都会检查它。接口不能恢复不可恢复的external abort。
- open只记录回调，无MMIO。描述值、回调返回值、DT和寄存器读数不产生授权。
- apply是单分支幂等请求，不是客户端引用计数或共享票据聚合器；这些仍由source承担。
- 不清除继承投票；AON关闭返回PINNED，不伪造关闭成功，所有相关backing必须保留。
- 首次I/O错误、位读回异常、超时或控制权丢失会quarantine。写前记录touched，
  不自动回滚、重试或释放可能仍依赖的电源/内存。隔离之后不再进行I/O。
- VOTED关闭只撤回自己设置的vote；其他voter保持clock运行是合法情况。

## 编译接入与测试

现有GPU patch生成器自动纳入`giulia_gpu_*.c`，新增后端随GPU配置加入XNU。
后链接检查新增五个外部可执行定义和identity marker，不能拿旧内核当新构建。
此检查仍明确报告`hardware_verified=false`和`acceleration=false`。

```
python3 tests/ios16_gpu_gpucc.py
python3 tests/ios16_gpu_gmu_source.py
```

前者从固定公开GPUCC源提取branch描述，核对六项offset/policy/AON，在普通RAM
中运行真实C实现。覆盖所有成功路径、继承、AON、HALT_DELAY、VOTED、所有成功
读写/等待位置的错误注入、live丢失、非法参数和有界超时，并验证C++链接和ARM64
Mach-O五个真实定义。后者从厂商GMU/寄存器头独立生成oracle。

这些是主机测试；Mach-O对象检查使用暂存类型，不是单独证明原生XNU ABI。
只有另行完成真实XNU完整链接，才能报告完整内核构建成功；仍不等同于GPU硬件运行。

## 下一步

需要真实GPUCC PLL1/RCG速率与供电依赖、CX/GDSC、共享ARC及板级接管source，
才能将此后端接到常驻电源服务。之后才安排Ace5专属power-only和GMU/HFI测试，
再做真实写回、像素与iOS16用户态。禁止在一加13T上测试这些SM8650地址。
