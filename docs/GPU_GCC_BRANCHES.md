# SM8650 GPU GCC 配置访问与总线分支

`giulia_gpu_gcc.c/.h` 实现固定 GCC 页 `0x00171000/0x1000` 内的四项开启事务。
板级组合层已实际调用 AXI、MEMNOC、DVM 核心；CFG 供原生上游在独立准备阶段使用，
没有放入只读 `resolve`，也没有被组合层偷偷开启。

这是有界寄存器算法及组合接线。当前启动授权、真实设备映射、供电/父时钟/带宽、
共享仲裁和注册 source 仍需上游实现；没有自动启动或 USB 硬件入口。

## 固定策略

来源为 OnePlusOSS `d763ed47deec55451366c1187f813f2f309d88a7` 的
`gcc-pineapple.c`、`clk-branch.c`，完整摘要见 `hardware/gpu-gcc-source.json`。
代码独立实现，HOST oracle 只提取固定数据与分支语义，不把 Linux 驱动编进 XNU。

| 操作 | 页内偏移 | 等待策略 |
|---|---:|---|
| CFG | `0x4` | 源码在 GCC probe 中直接常开；确认 enable 位，不等待 halt |
| AXI | `0x154` | `BRANCH_HALT_SKIP`；记录读值，不以原始 enable/halt 位拒绝已接受写入 |
| MEMNOC | `0x10` | `BRANCH_HALT_VOTED`；HWCG bit1 生效时跳过 halt，否则最多 200 次轮询 |
| DVM | `0x18` | `BRANCH_HALT_DELAY`；等待 10 微秒，不等待 halt |

DVM 在此固定版本中是 DELAY。此前 UEFI/观察代码把它描述为 VOTED 的注释不能
替代本后端的版本依据；本次没有顺带修改历史 UEFI 候选。各版本仍需单独验证。
CFG、MEMNOC、DVM 对 enable 位做保守读回检查，AXI 沿用已有板级观察中的 SKIP
边界。写入成功、寄存器读回和 HWCG 都不等于测得频率或取得独立租约。

## 前提及失败保留

调用者必须持续持有真实当前启动的访问和撤权同步 guard。`ready` 是无 MMIO 的
CPU 状态检查：映射、分支排他、父时钟、供电均已保留；有效时钟至少 1 MHz，
使固定轮询界限适用。没有这些条件应返回不可用，不能使用恒真替身。

每个 read/write/wait 和 ready 回调前后检查 live。实际写入前记录 touched；
即使写回调返回失败也按已经可能生效处理。丢权、I/O 错误、读回矛盾或超时后
永久隔离该状态，保持首次原因，不重试、不回滚、不再执行 I/O。

该初期路径只开启并保留到 reset，不提供 disable 或共享票据撤销。继承的位
只记为 inherited，不生成独立硬件 ownership。原生上游必须确保整个 backing
在失败、stop 和失权后仍可保留；库内标志本身不能做到这一点。

## 接入和验证

`ggbs_io` 新增独立 `gcc_ready/gcc_read32/gcc_write32`，与 GPUCC I/O 分开。
板级组合的 11 项资源顺序和外部资源 ID 保持不变。CX 已接入本地 GDSC 核心，
11项固定资源的正常路径均调用本地执行器。供电/父时钟/带宽条件继续由 upstream 保证。

打包补丁生成器自动收集新增 C/头文件。内核检查新增 5 个 `gggc_*` 可执行定义
及标识字符串，GCC 增量使完整 GPU 检查由 126 个定义增至 131 个，随后 CX 增量增至 136 个。旧内核不能据此当作
更新后的候选。

`tests/ios16_gpu_gcc.py` 对实际 C 运行 source-derived 策略、所有正常及最长轮询
回调的故障/失权注入、HWCG、AXI 不锁存、继承状态、禁止重写丢失票据等测试；
还检查 C++/C 链接及 5 个隔离 ARM64 Mach-O 定义。组合回归运行真实
board/CX/PLL1/RCG/GPUCC/GCC 代码；硬件 I/O 和授权仅在 HOST fixture 中替换。

上述测试不是完整原生 XNU 编译链接，也不是手机上电、持续持有或 reset 退出证明。
`power_only_test_admissible` 保持 false。
