# Ace5 CX/GDSC 固定开启事务

`giulia_gpu_cx.c/.h` 提供 `ggcx_identity/describe/open/enable/invalidate`。
板级组合的 CX 资源调用该核心。`open` 只初始化 CPU 状态，`enable` 执行一次固定事务。

## 源码依据与步骤

公开 OnePlusOSS 内核提交 `d763ed47deec55451366c1187f813f2f309d88a7` 的
`gdsc-regulator.c` 及设备树提交 `453d8875f815bf9dfad9035982861226a4823a6c`
的 `pineapple-gdsc.dtsi`、`pineapple.dtsi` 提供固定策略，摘要记录于
`hardware/gpu-cx-source.json`。

1. 检查真实 ready 和全局 quiet。
2. 对 GPUCC `0x9108` 做新鲜 RMW，清 HW_CONTROL/SW_OVERRIDE，设置 CLK_DIS_WAIT=8。
3. 读回配置。继承 HW_CONTROL 时等待 1 µs，随后清 SW_COLLAPSE，读回并等待 1 µs。
4. 只从独立只读状态 `0x9168` 的 bit31 判断 power-on。最多两轮、每轮1500次读取，
   未成功读取之间等待1 µs，耗尽一轮后保留诊断读值。
5. 新鲜 RMW 设置 retain FF bit11，读回，两次1 µs稳定等待，最终检查状态与控制位。

Linux probe 初始化和 NORMAL 模式的硬件转软件交接被组合成固定策略。
其适用性要求当前启动允许软件接管，真实上游持续提供排他和供电条件。
核心保留无关寄存器位，仅允许写 `0x9108`，状态地址保持只读。

## 生命周期

每个 ready/quiet/read/write/wait 回调前后检查 live。实际写入前记录可能变更，
失败或失权后锁存首个错误并隔离。板级组合在首个 CX 写入前发布 token、held 和
retained。固定路径只有一次开启尝试，后续调用返回 USED。

真实上游必须提供当前启动授权、稳定设备映射、撤权同步、全局静默、CFG/XO、
ARC/供电及 ACD 支撑，并持有至可验证的 reset。该核心采用保留策略，没有关闭事务。
当前生产代码尚未初始化并注册完整 source。

## 验证

`tests/ios16_gpu_cx.py` 执行真实 C，使用公开固定策略生成 HOST oracle。
覆盖16种初始控制位组合、最长3000次轮询的全部回调故障/失权边界、独立状态位、
新鲜 RMW、模式交接与稳定等待顺序、retain 后失电及禁止重放。
组合测试执行真实 board/CX/PLL1/RCG/GPUCC/GCC 核心。

已进行 C++ 头文件与真实 C 对象链接、5个隔离 ARM64 Mach-O 定义检查。
完整内核检查要求136个定义，尚未有本轮整内核构建通过记录。
当前 `power_only_test_admissible` 与 `gpu_acceleration_enabled` 均为 false。
