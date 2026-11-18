# Restricted RPMh ACTIVE TCS transport (not GPU acceleration)

`ios16/gpu/giulia_gpu_rsc.c/.h` implements a register transaction algorithm
for one ARC write on an **already owned dedicated ACTIVE TCS**. The existing
GPU patch composer stages it automatically. It does not install a native RSC
owner, map MMIO, acquire a voltage lease, register a power source, or enable
a GPU service. Its only permitted production-code caller is the separate
[guarded native executor](GPU_RSC_NATIVE.md), which itself has no activation
caller or real board authority source.

This is a transport prerequisite after the [Command DB decoder](GPU_ARC_LEVELS.md),
not a replacement for [current-boot ownership](GPU_RUNTIME_OWNERSHIP.md).
**It is not yet admissible for a phone power-only or GPU command test.**

## Public source and supported subset

`hardware/gpu-rsc-source.json` pins four public files to OnePlusOSS commit
`d763ed47deec55451366c1187f813f2f309d88a7`, with exact size and SHA256.
In `drivers/soc/qcom/rpmh-rsc.c`:

- `rpmh_rsc_reg_offsets_ver_2_7` and `...ver_3_0` define explicit **legacy**
  layouts. TCS stride is 672 bytes. Only command slot 0 is used; command stride
  differences are not silently generalized to multiple commands.
- `__tcs_buffer_write` defines the 8-byte WRITE message with RESP_REQ, address,
  selector, and command WAIT/ENABLE fields.
- `__tcs_set_trigger` defines clear-trigger, clear-mode, enable-mode, trigger.
- `tcs_tx_done` defines per-TCS IRQ completion and own-bit W1C reclamation.
- Public `CMD_STATUS_ISSUED`/`CMD_STATUS_COMPL` are additionally required;
  unknown status bits are conservatively refused, not diagnosed as a specific
  hardware fault.

No board RSC address, TCS assignment, actual register version or resource
selector is guessed. HW-channel mode, borrowed sleep/wake TCS, solver,
fast-path, multiple commands, multi-client aggregation and sleep/resume are
**not supported**. The future source must verify that this subset fits the
current boot. A matching source hash or DT is not that verification.

## Native authority obligations still unimplemented

The caller must provide a real kernel-native guard covering the entire
binding lifetime and every callback: assigned ACTIVE TCS, access and IRQ
routing, retained supplies/clocks, exact ARC binding from an authorized stable
Command DB, and serialized aggregation including other clients. The target IRQ
bit must already be enabled and its delivery cannot race an unrelated handler;
this module never writes IRQ_ENABLE. Teardown must honor retained/uncertain
state. A software broker cannot exclude firmware or hypervisor actors.

`live(epoch)` is a CPU-only check **under** that guard, not a self-issued grant.
A constant success callback is not a native backend. IO callbacks must perform
genuine bounded ordered 32-bit accesses. This caller-serialized C core is not
an IRQ handler, concurrent executor or native mapping layer. A separate native
mapping/executor now exists, but upstream resource ownership, aggregation and
revocation backing still must be supplied by the future source.

## Transactions and failures

1. Validate configuration before any callback. Require idle/disabled/clean TCS
   and no target IRQ; never adopt or erase inherited activity.
2. Allocate a non-wrapping serial before any possible write. Send only an
   explicitly permitted 0..15 selector to the source-bound ARC address. One
   in-flight request; no arbitrary RPMh address or untrusted application input.
3. Read back preparation writes with at most 16 attempts and 1-us delays.
   Reject a stale IRQ immediately before trigger. Do not retry the final
   trigger, whose register may be hardware-changing.
4. Poll monotonic time with a timeout and independent 4096-call limit for a
   frozen clock. A new target IRQ, exact issued/completed command status, and
   idle TCS are all necessary. A write readback alone is not completion.
5. Clear trigger then mode, disable command/wait, acknowledge **only our bit**,
   and verify clean state. Foreign IRQ bits and IRQ_ENABLE are preserved.

Authority is checked before/after every IO/clock/delay callback. Failed writes
may already have happened. Loss, timeout, uncertain IO or failed cleanup
quarantines and retains state, serial and in-flight request. There is no
automatic rollback, cancel, retry or reopen. Invalidate makes no IO.

**TCS completion is not ARC vote withdrawal or measured voltage.** The request
remains active after transport close. Native aggregation/withdrawal and its
leases remain the source's responsibility; transport close does not authorize
dropping backing. A power request completion is not a GPU render fence.

## Tests and build boundaries

`tests/ios16_gpu_rsc.py` compiles the actual C core under ASan/UBSan and derives
an independent layout/bit oracle from hash-verified public Linux. GPL reference
fixtures stay host-only, outside the BSD production module. Tests cover both
layouts, all 32 TCS IDs/16 selectors, write order, foreign IRQ preservation,
busy/stale serials, inherited activity, every callback loss/error boundary,
all-ones reads, bounded stuck readback, bad completion, clock reversal,
deadlines, frozen clocks, exhaustion and retained uncertainty. This is
**ordinary RAM, not hardware**.

All six C exports are called from C++ and verified in executable ARM64 Mach-O
sections. The linked-kernel checker retains the previous 83 required functions
and adds six (89 at that milestone) plus an unbound marker. The separate native
executor adds eight more definitions (97 total). This requirement does not mean
a new full kernel was built. The separate Nov11 native XNU22-header object
compile is not a full kernel link or on-phone validation.

## What remains before SpringBoard acceleration

Real board RSC/clock/power ownership and physical GPU handoff, then authorized
SMMU/IRQ/GMU/HFI/CP/context/fence providers and a minimal trusted render with
real output and fence completion. Afterwards, Darwin22/iOS16 Metal, IOSurface
and display integration must actually use the driver. Firmware embedding,
this transport, or low-level GPU submission alone does not accelerate the
existing software-rendered desktop.
