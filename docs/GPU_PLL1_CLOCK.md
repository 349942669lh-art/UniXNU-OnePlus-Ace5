# Ace5 SM8650 GPU PLL1：已校准 standby → run 执行器

`ios16/gpu/giulia_gpu_pll1.c/.h` 仅实现两档固定 PLL1 的配频、latch 和 lock 事务。
**不是完整原生电源服务，不执行首次校准，不自动接入生产启动流程，不代表 GPU 加速。**
与 GPUCC 分支及 RCG 后端一样，必须先由真实本次 XNU 启动的资源所有者建立原生 guard。

## 来源及频率语义

`hardware/gpu-pll1-source.json` 固定公开 SM8650 仓库 commit 和三个文件的 SHA256。
用户本地 `23851_17.0.0.2_2026111602560461_root` 的对应文件与该公开版本逐字节一致。
私有源码参考不放进 repo；测试使用公开的 hash-pinned cache，不携带原厂固件或认证资料。

PLL1 在 GPUCC `0x03d90000/0x9800` 内偏移 `0x1000`，布局为 Lucid OLE，
而头文件把其 ops 和 configure 映射至 Lucid EVO。不能套用旧版 alpha PLL 的布局。
本模块只使用 OLE 的 MODE、OPMODE、L、ALPHA、USER 和固定模拟配置寄存器。

| 名义父时钟请求 | L | ALPHA（16 位） | 编码频率的整数下取整 | 源驱动取整报告 |
|---|---:|---:|---:|---:|
| 520 MHz | 27 | 5461 (`0x1555`) | 519,999,902 Hz | 520,000,000 Hz |
| 1250 MHz | 65 | 6826 (`0x1aaa`) | 1,249,999,804 Hz | 1,250,000,000 Hz |

XO 固定为 19.2 MHz。按源规则向下截取 fractional alpha，再把对外报告向上取整到
1 kHz。`floor_hz` 只是编码有理数的整数下取整，`reported_hz` 是报告惯例；均非测量。
RCG 的 GMU 260/625 MHz 请求由此分频，实际编码输出不能被当作数学上精确的目标值。
本模块不是 GPU 核心 OPP 表，也不进行动态调频。

## 原生前提：不满足即不写

`ready`/`parked` 回调仅检查 CPU 中已有的事实，不得偷偷取得授权或改变硬件。
真实 source 必须持有 GPUCC device/uncached mapping、PLL1 和所有写入者排他控制，
固定 XO、CX/MX/访问供电，以及覆盖旧/新频率的电压和带宽 backing。
所有 PLL1 消费者必须已经切离 PLL1 并保持停车；仅仅“没有 GPU 作业”并不充分。
IOService retain、本地锁、DT 描述和 Linux 的历史运行状态不能替代资源交接。

执行器额外检查硬件必须处于：

- OPMODE=STANDBY、RESET_N=1、OUTCTRL=0；无 FSM/vote-run、pending latch 或 ACK。
- PCAL_NOT_DONE=0，L 的两段 calibration 值均为 `0x44`，L 本身非零。
- 三路 USER 输出位已配置，固定模拟配置和 USER_HI 与来源匹配。

运行中/继承输出/待处理 latch 返回 BUSY；未初始化、需校准或配置不同返回
UNAVAILABLE。两者均不写寄存器，不自动接管或修复。
模拟配置检查采用严格支持子集，不能据此认定其他合法配置一定错误。

首次自校准可能需要高于目标运行档的临时电压，本模块没有相应原生投票能力，
因此明确拒绝冷启动状态；不能伪造 PCAL 已完成或恒成功 ready 来绕过。

## 事务与隔离

1. 仅修改 L 低 16 位，保留 process/ringosc calibration；写入目标 ALPHA 并读回。
2. 设置 LATCH_INPUT，等待 1 us（覆盖两个参考周期），检查 ACK 已置位。
3. 清除自身 LATCH_INPUT，有界等待 ACK 撤销。
4. 切到 RUN，最多 200 次、间隔 1 us 等待 LOCK；再次核对编码。
5. 开启 OUTCTRL 并核对 lock、输出、编码、OPMODE 和 USER。

ACK 撤销检查比参考驱动更保守；尚未验证实际芯片行为，不把 RAM 模型当成硬件事实。
MODE 含硬件状态位，不对整个寄存器采用“写入值必须等于读回值”的错误规则。
不写 STATUS/STATE，不提供任意地址或任意频率的调试入口。

成功后不自动切换 RCG，不释放 backing；每个 state 仅允许一次成功 start，
不支持运行中 retune 或 shutdown。调用者串行化 state/invalidate，回调不可重入。
任何 I/O 不确定、guard 丢失、异常读回或超时均永久隔离，不回滚、不重试、
不做清理写入。输出已打开后发生错误，也必须继续保留电源/映射等 backing。
该 C 执行器无法捕获错误映射导致的 synchronous external abort。

## 验证及复现

将 pin 中的公开 URL 按 `path` 下载到 `.cache/gpu-pll1-source/`，测试首先验证
长度和 SHA256，再独立提取寄存器、配置与频率计划。不得用未核验的本地私有文件替代。

```sh
ASAN_OPTIONS=abort_on_error=1:handle_segv=0 python3 tests/ios16_gpu_pll1.py --out /path/on/F/pll1-host
```

测试用普通 RAM 模拟硬件，覆盖两档计划、所有成功路径回调错误/失活边界、
ACK/LOCK 超时、错误读回、继承状态拒绝、配置不匹配与输出开启后的隔离。
另检查 C++ 链接和五个 ARM64 Mach-O 定义。这不证明原生 XNU ABI 或真实硬件。

GPU patch composer 自动收集此编译单元；后链接检查增加 marker 和五个真实定义，
总计要求 120 个 GPU 可执行定义。因此旧的 115 定义内核不能冒充本次链接产物。

## 下一步

完整电源 source 仍未绑定。还需冷初始化/校准所需电压协调、CX/GDSC、共享 ARC、
CFG/ACD/带宽、本次启动的 Command DB 与资源交接，再把这些执行器接入真实 owner。
通过准入检查后才可单独授权 power-only 硬件测试；GMU/HFI17、GPU 命令/写回、
像素验证及 Metal/IOSurface/合成器接入均是后续阶段，不能由编译或 LOCK 推断完成。
