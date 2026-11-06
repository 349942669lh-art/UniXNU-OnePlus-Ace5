# Command DB／ARC 逻辑等级前置实现（不是电源驱动）

此模块补齐未来 RPMh 电源后端需要的 **资源地址解析与逻辑等级换算**。
它仅处理调用者持有的稳定、私有、普通 RAM 快照；不映射硬件、读取共享
固件内存、发送 RPMh／SCM／HVC 请求，也不注册为 GPU power provider。

**不能把解析成功、地址、selector 或表内容当成当前启动的电源授权。**
本模块不产生真实电压、不使能电源、不确认完成、不改变 GPU readiness。
没有当前设备 Command DB 快照被获取或验证。测试中的名字与地址是人工
构造数据，不是 f735dd70 的资源地址证据。

## 为什么需要它

`VDD_*` / `RPMH_REGULATOR_LEVEL_*` 是 ARC 逻辑等级，不是微伏数值。
发送给 ARC 的硬件档位（selector，最多 16 档）由 Command DB 的资源辅助
数据定义；逻辑等级与 selector 不能互相直接替代，更不能照搬另一台设备
的地址或档位。原生 regulator 源还需要处理真实请求聚合和 RSC 完成。

公开固定版本中的两个驱动用途也不同：`qcom-rpmh-regulator.c` 的 PMIC
VRM 路径不能代替 `rpmh-regulator.c` 的 ARC level 映射。
后者明确使用 `cmd_db_read_addr`、`cmd_db_read_slave_id` 和
`rpmh_regulator_load_arc_level_mapping`。此实现选择后者的 ARC 数据格式，
没有根据先前的 PMIC 参考路径猜测电压命令。

## 固定版本依据

`hardware/gpu-arc-source.json` 记录公开源码的完整 commit、字节数和 SHA256：

- [cmd-db.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/soc/qcom/cmd-db.c)：
  144 字节主头、8 个 16 字节 slave header、24 字节 entry、8 字节资源 ID；
  header_offset/data_offset 均相对于主头末尾，entry 的 aux offset 再叠加。
- [rpmh-regulator.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/regulator/rpmh-regulator.c)：
  `rpmh_regulator_load_arc_level_mapping` 使用 1..16 个小端 u16，忽略尾部零；
  `rpmh_regulator_load_default_parameters` 查找首个不低于请求的逻辑等级。
- 同版本 `include/soc/qcom/cmd-db.h`：ARC slave ID 为 3。
- 同版本 `qcom,rpmh-regulator-levels.h`：定义逻辑等级，不是硬件 selector
  或物理微伏值。

新生产代码是独立编写的有界 wire decoder。原 Linux 函数仅提取到临时
GPL 宿主 oracle；不并入 XNU 或固件，也不上传私有 Z: 原厂源码。

## API 与支持范围

`ios16/gpu/giulia_gpu_arc.c/.h` 导出：

- `ggarc_identity`：表明它是 snapshot-only / no-hardware-authority。
- `ggarc_resolve`：验证整个描述区边界与所选 ARC 表，复制为无指针结果。
- `ggarc_select`：在 `[min,max]` 逻辑等级范围中选择最低可用 selector。

版本必须由可信调用方显式指定，不从目标设备型号或文件名推断。版本与
magic 相符只说明格式匹配，不认证来源。上游未校验的 checksum 在此也
不作为完整性或真实性证据；优先级字段不用于授权。

有意限制的保守子集，不声称所有 Linux 可读数据库都被支持：

- 快照 144..262144 字节，最多 1024 个 entry；无堆分配、递归或外部回调。
- header 连续、slave ID 唯一，保留字段为零；未知 flags、standalone 模式
  或版本不符拒绝，不为“先能启动”放宽。
- 所有 entry table 和 aux 的长度／偏移先检查；entry table 不能互相重叠，
  aux 不能覆盖 entry table。只读 aux 之间允许共享；不制造独立硬件票。
- 精确的 1..8 字节 ASCII key；不截断过长名字，不接受嵌入 NUL。
  同名资源有歧义时拒绝，不取第一项冒充可靠解析。
- 所选资源必须属于 ARC，地址是四字节对齐的 20 位 ARC 地址子集。
  这是 RPMh command address，**不是允许 CPU 映射的 MMIO 地址**。
- ARC aux 必须为 2..32 字节、偶数长度。逻辑等级非递减，允许等值档位；
  第 1 项之后出现零表示尾部 padding，之后不能重新出现非零。
- 第 0 档可能是非零逻辑等级。`selector=0` **不等于 OFF**；只有显式
  请求 `[0,0]` 且表确实包含零，才能生成零级别规划，仍不会发送命令。
- 逻辑范围限于 0..65535；无可用档位返回 RANGE，不向下钳位，不把
  `600000` 之类微伏数值静默截断为 u16。

失败输出清零。输出与 image/name/policy 必须不重叠，输入在整个调用期间
保持稳定；这些是可信内核调用方的普通内存契约，不是处理用户地址的 API。
数据拷出后不保留输入指针；输入存储期结束不会使结果成为悬空引用。
可编辑的 table/plan 本身不是能力票据，也不能传给未授权的硬件路径。

## 离线检查工具

`scripts/inspect-ios16-gpu-arc.py` 编译并调用实际 C decoder，不另写一套
Python 解析算法。只接收普通快照文件；拒绝设备节点、管道、目录、符号
链接和 `/dev`、`/sys`、`/proc` 伪文件。显式给定版本、资源名和逻辑范围：

```sh
# 仅为已有合法快照的使用格式；版本、资源和范围应先取得实际依据。
# 不要为此去读 /dev/mem，或试探尚未授权的共享固件区域。
export TMPDIR=/mnt/f/ace5-port/gpu-adaptation-v1/arc-levels-20261106/tmp
python3 scripts/inspect-ios16-gpu-arc.py \
  --image /mnt/f/path/to/verified-private-snapshot.bin \
  --resource RESOURCE --db-version 0xVERSION --arc-version 0xVERSION \
  --min-level LOGICAL_MIN --max-level LOGICAL_MAX
```

上例使用占位参数，不能原样运行。输出始终保留
`hardware_authority=false`、`current_boot_verified=false`、
`rpmh_submitted=false`、`safe_for_flash=false`；SHA256 只标识所读文件，
不认证固件或当前启动。此工具不自动寻找或从手机提取快照。

## 测试与下一步

`tests/ios16_gpu_arc.py` 编译实际 C 模块，执行 ASan/UBSan RAM 故障用例，
并对 4096 组已知合法且对齐的人工镜像调用固定版本原 Linux 查找／ARC
映射函数作差分对照。原查找函数没有边界保护，**畸形、截断、非对齐和
变异样本只交给新 decoder**。另验证 C++ 调用 C exports、生成的 ARM64
Mach-O 定义以及离线工具的安全输出。这些都不是设备电源或硬件测试。

GPU post-link checker 保留原 80 个定义，新增上述 3 个，要求 **83** 个
可执行定义及 snapshot-only marker。编译和链接不会执行解析函数或 MMIO。

真实平台源仍需证明快照来源／当前 epoch、ARC resource 的原厂对应关系、
RSC/TCS 所有权、共享请求聚合、完成与不确定失败保留，再接通实际 regulator
与上游 PLL。Command DB 数据不是这些权属的替代品。尚不满足 power-only
真机测试准入条件，不应为本次数据解析模块刷机或格式化设备。
