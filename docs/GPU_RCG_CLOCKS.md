# SM8650 GPUCC RCG 配置后端

`giulia_gpu_rcg.c/.h` 为 GMU、HUB、FF 三个固定根时钟实现有界配置事务。
它不是 PLL、regulator、板级接管服务或完整 GPU power source，没有生产激活调用者。
不添加 MMIO 调试命令、UEFI grant、USB 命令或 boot-arg 自动开启路径。

## 来源与范围

`hardware/gpu-rcg-source.json` 固定公开 SM8650 仓库的 commit、文件大小与 SHA256。
本次用户提供的本地 SM8650/23851 Android17 源码中，GPUCC、RCG2 与 RCG 头文件
与这些公开文件逐字节一致。私有源码、原厂固件及制品访问凭据不随此模块上传。
源码相同不证明 Android16 与 Android17 所有板级配置/固件相同。

GPUCC 基址/范围为 0x03d90000/0x9800，本模块回调仅得到下列 CMD 与 CMD+4：

| 根 | CMD 偏移 | 非停车输出 | 父时钟名义请求 | SRC/HID |
|---|---:|---|---|---|
| GMU | 0x9318 | 260/625 MHz | PLL1 520/1250 MHz | 3 / 3 |
| HUB | 0x93ec | 200/300 MHz | GPLL0 600 MHz | 5 / 5或3 |
| FF | 0x9474 | 200 MHz | GPLL0 600 MHz | 5 / 5 |

三个根均允许 19.2 MHz XO 停车配置，SRC=0/HID=1。这些频率不是 GPU 核心 OPP，
不能拿 Android 的 GPU 核心频率列表替换。parent_hz 是名义请求，不是测量值；
PLL 源另行验证 L/alpha、量化误差与 lock，不能仅凭该数值声称 PLL 已稳定。

CFG 只修改 HID[4:0]、SRC[10:8]、MODE[13:12]、HW_CLK_CTRL[20]。
三个根没有 MND，MODE 清零，保留其他位。UPDATE 自清零，不要求 CMD 等于写值。

## 原生调用前提

调用者必须真实持有本次启动中的稳定 device/uncached mapping、根寄存器排他控制、
旧/新父时钟、CX/MX/访问供电及整个调用期间不会撤销的原生 guard。
`ready`/`quiescent` 是 CPU-only 的已有前提检查，不能在其中偷偷编程硬件。
所有根的全部 dependents 必须静默，所有旧/新有效频率至少 1 MHz，才采用
源代码中最低 500×1us 的等待界限。IOService retain 或本地锁本身不构成上游 guard。

共享 PLL 不可在仍向依赖者供钟时直接改频。真实源应先让根停车 XO，协调其他 PLL
消费者，然后在根外配置/锁定 PLL，保留其 backing，再应用目标 RCG 配置。
本模块不处理该跨模块事务，也不以主机中的恒成功 callback 冒充实际父时钟服务。

## 事务与失败

1. 初始 CMD 有 UPDATE/dirty 时拒绝 BUSY，不接管另一未完成配置。
2. 没有 ROOT_EN 时添加自己的 force vote；继承的 ROOT_EN 永不清除。
3. 有界等待 ROOT_OFF 清零，再配置 CFG 并核对有效字段。
4. 设置 UPDATE，等待其自清零，再次核对 CFG。
5. 只清除自己添加的 force vote，读回确认。下游门控时 ROOT_OFF 可以重新置位。

成功只表示配置握手完成，不表示测量频率、分支开启、GPU 上电或渲染成功。
首次 I/O 错误、live 丢失、异常读回、超时或恢复失败立即隔离；不回滚、不自动重试，
不释放映射、票据或供电 backing。invalidate 也不进行硬件清理。
调用者串行化 state 和 invalidate，回调不可重入。接口不能恢复不可恢复的外部 abort。

## 构建与测试边界

`python3 tests/ios16_gpu_rcg.py` 从 hash-pinned 公开源码独立提取八组计划，编译实际 C
执行器，在普通 RAM 中模拟 CMD、CFG、自清零、强制使能和错误注入。
同时检查 C++ 调用链接和五个 ARM64 Mach-O 可执行定义。测试源/类型替身不证明
原生 XNU ABI，更不证明真机运行；完整 XNU 链接和硬件测试必须另行记录。

现有 GPU patch 自动收集本单元，回归脚本增加该测试；后链接检查新增 marker 和
五个真实定义，总计要求 115 个 GPU 可执行定义。旧的 110 定义内核不是本次产物。

下一步仍需原生 PLL1、CX/GDSC、ARC/带宽与 current-boot 板级接管服务，随后才能
绑定完整 power source。完成之前，不生成或试启动所谓 GPU 加速镜像。
