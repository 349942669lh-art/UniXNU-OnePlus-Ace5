# GCC GPU parent-branch subset (not acceleration)

This module contains **actual fixed-register 32-bit read/modify/write code**,
plus a native XNU mapping/executor and bounded reference tickets. It is not
just a desired-state ledger. It is also **not a complete native power source**,
proof of ownership, a PLL/regulator driver, or an enabled GPU path.

No production caller opens it. There is no boot argument, automatic IOKit
registration, terminal command or USB request which can activate it. Compiling
it into a kernel does not perform its MMIO. Do not flash a phone just to test
this currently unbound subset; the existing software-rendered baseline stays
unchanged.

## Public primary evidence

See `hardware/gpu-gcc-parents-source.json` for fixed commit/file SHA256s.
Both files are publicly available in OnePlusOSS at commit
`d763ed47deec55451366c1187f813f2f309d88a7`:

- [gcc-pineapple.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/clk/qcom/gcc-pineapple.c),
  lines 2364–2396: branches share offset `0x52000`, using bits 15 and 16.
  GPLL0 OUT_EVEN uses the post-divider table near lines 117–136 (divide by 2).
- [clk-branch.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/d763ed47deec55451366c1187f813f2f309d88a7/drivers/clk/qcom/clk-branch.c),
  lines 103–104: `BRANCH_HALT_DELAY` uses `udelay(10)`, not a halt-bit poll.

With GCC base `0x100000`, the transaction page is **`0x152000`, 4 KiB**;
this is not the existing GPU GCC page at `0x171000`. The new page requires its
own native grant. The host-audited board clock plan requires stable 600 MHz
GPLL0 and 300 MHz OUT_EVEN. The module does not program or measure either
rate; it requires a real source to establish and retain those prerequisites.
A matching file hash or DT address never establishes present hardware state.
No private system source or device firmware is included in this change.

## Guard and source contract

`ggcp_authority` is an in-kernel, copied callback table, not a userspace proof.
The future real IOService source must independently establish current-boot
CPU access rights, stable upstream supplies/PLL/post-divider, dependent
quiescence, and a guard that serializes **all** writers of the shared register.
The global broker reservation only excludes cooperating XNU clients; it does
not stop firmware, a hypervisor, or another execution world from revoking
access. The source must prevent such revocation during each guard interval.

The source must also coordinate teardown for its whole binding lifetime.
IOService retain alone does not keep `stop()` from freeing resources.
An uncertain result or BUSY means native votes/backing cannot be removed.
A clean close detaches atomically before unmapping; backing must remain valid
until close returns. `invalidate` latches revoke before waiting for the local
executor; it cannot cancel a hardware transaction already executing under the
real upstream guard. Guard `leave` only unlocks CPU serialization; even after
loss it must **never** drop access, clocks, voltage or the source's backing.
No callbacks may synchronously reenter this executor or invert its lock order.

- `enter == 1`: genuine native guard acquired for the exact fixed scope/epoch.
- `enter == 0`: unchanged denial; no MMIO, return BUSY. First-open denial
  cleans software state; a denied later transaction keeps existing tickets.
- Any other result, lost authority, failed readback or uncertain write:
  quarantine. Retain exclusion/source/map/tickets; do not optimistically undo.
- `live` must be a CPU-only upstream authority check, not an MMIO probe or
  self-issued certificate. `quiescent == 0` retains the last vote as BUSY;
  exactly 1 is required to disable; any other result quarantines.

## Transactions and lifetime

The state must be initially zero. One registration per boot, one permanent
IOLock executor, two clocks and 16 generation-ticket slots. Only aligned
volatile 32-bit accesses at page offset zero are allowed, with native DSB SY
barriers, noncached mapping and IODelay(10). No arbitrary register command or
HVC/SCM transport is exposed. Status is cached and never probes hardware.

Only bits 15/16 may change. Unrelated bits are preserved from a fresh snapshot
and the entire write is read back while the native guard excludes other
writers. External field changes between transactions are allowed; changes
inside the serialized transaction are an error. All-ones reads are rejected.
An inherited enabled managed bit is **not** adopted as an independent vote
and is never cleared. Core init refuses that state without writing.

Allocate the ticket before a possible write; retain it even on an uncertain
result. First reference enables, repeated references reuse it, final release
requires real dependency quiescence, a fresh read, disable, delay and readback.
Busy release retains the ticket. There is no production reset/retry API after
quarantine and no fallback that merely pretends an operation succeeded.

## Validation boundaries

`python3 tests/ios16_gpu_parents.py` compiles the actual C core and native C++
executor with ASan/UBSan. Test register, IOKit mapping, native authority and
lock doubles are explicitly **host RAM only**, alongside actual shared broker
algorithms. Fixtures resetting quarantine are never compiled as kernel APIs.
These tests cover callback-boundary loss, failures, inherited votes, shared
field preservation, ticket capacity/staleness, BUSY, clean detach versus
revoke, and invalidation waiting for an in-flight callback.

The full GPU regression includes this test. The post-link checker now
retains these 80 executable external definitions (the previous 68 plus 5 core
and 7 native parent functions). The later [ARC snapshot-only subset](GPU_ARC_LEVELS.md)
adds 3 more and its own snapshot-only marker, for a current total of 83
definitions; the parent-open marker remains required. Host tests and native
compilation/linkage are not hardware validation. Real resource handoff,
voltage/PLL/power backend, SMMU/GMU readiness, CP/fence submission and compositor
integration remain separate unfinished work; `acceleration` remains false.
