# Native APPS RSC executor — not a board ownership provider

`giulia_gpu_rsc_native.cpp/.h` connects the restricted `ggrsc_*` transport to
real XNU IOKit memory descriptors, an executor lock, the shared platform broker,
native monotonic time, 32-bit volatile accesses and ARM64 `DSB SY` barriers.
It is GPU dependency work, not a general peripheral/power framework.

**No production caller/authority is supplied. No boot argument, DT-ready bit,
terminal command, successful host test or retained IOService enables hardware.**
This unit is another necessary backend layer, not working GPU acceleration.

## Source binding

`hardware/gpu-rsc-native-source.json` records the local source provenance.
The APPS DRV2 candidate window is 0x17a20000/0x10000; TCS base offset 0xd00,
channel0 ACTIVE mask 0x7, IRQ GIC SPI5 / INTID37. The current-boot source must
prove the actual board allocation, assignment, access, routing and backing.
This code NEVER scans for an idle TCS or treats these constants as a grant.
The observed `qcom,hw-channel` property in the base DTS belongs to CAM RSC,
not APPS RSC. The full product include chain/revision remains to be qualified.

The native mapping conservatively excludes the whole DRV aperture in the global
broker, including its shared IRQ registers. The source must delegate that
mapping exclusively and coordinate all existing DRV users. This is **not** a
claim that allocating only a TCS gives exclusive ownership of the whole DRV;
an incompatible platform broker allocation must refuse open, not be bypassed.

## Durable grant and executable path

1. `initialize` allocates only the executor lock. No automatic invocation.
2. `open` validates and copies config/callbacks, retains the actual IOService,
   reserves software exclusion and asks the source for a durable native grant.
   Unchanged denial is zero result + zero grant only. All partial/malformed
   transfers remain pinned in quarantine.
3. Only after that grant, the executor creates an uncached IOKit mapping. It
   checks exact extent, alignment, explicit 2.7/3.0 hardware version, no HW
   solver bit, hardware TCS capacity and command count. These reads validate a
   granted scope; they cannot establish the authority to perform the reads.
   Direct native MMIO cannot turn a synchronous external abort into a safe
   error return. Powered-off/protected windows must NEVER be probed to discover
   access; the source must establish live access before any metadata read.
4. The real C transport checks an idle/disabled/clean, IRQ-enabled assigned
   TCS. The native adapter permits only the transport's exact register subset;
   in particular no IRQ_ENABLE, foreign TCS or status register write exists.
5. Each submit additionally requires CPU-only current aggregate approval.
   Source aggregation/Command DB must be authoritative for this boot and kept
   serialized until the command completes. A mask alone cannot approve a
   stale aggregate. One trigger; bounded poll; real IRQ/status/idle checks.
6. `close` refuses an in-flight command. Source `settled` must confirm no users
   remain and the last ARC vote has been reconciled; it must not mutate/drop
   backing. Transport completion alone is NEVER vote withdrawal. The native
   unit does not perform blind rollback or release another client's vote.
7. A close/revoke CAS is resolved before unmapping. After unmap, source drop
   occurs while software exclusion and source are still held. Only confirmed
   drop permits broker/source release. Drop failure retains exclusion/source
   without retry, even though aliases have already been safely unmapped.

The grant is a durable ownership reservation, **not a mutex held by a thread
across API returns**. The source must exclude foreign writers/handlers and
upstream backing removal for the entire session. Callbacks are copied,
kernel-resident, non-reentrant; no Boot Services callback survives as a source.
IOService retain alone cannot stop `stop()` from freeing its resources.

## Loss and concurrency

Invalidate atomically latches loss, then joins the permanent executor. It never
removes upstream access or backing and never performs hardware cleanup. The
source must honor quarantine/BUSY, including BUSY during an in-progress detach.
Later submit/poll/close cannot use a quarantined mapping. No reopen/reset API.
An uncertain submit serial is retained; callers must not erase it or retry.
Status reports cached CPU-only state, not live hardware readiness.

## Tests and build boundary

`tests/ios16_gpu_rsc_native.py` checks the source reference and absence of any
production activation caller, then runs the actual production executor and C
transport with RAM-only IOKit/authority/register doubles under ASan/UBSan.
It covers all assigned TCS candidates and both layouts, real transport control
flow, every authority boundary, unchanged/uncertain grants, invalid readback,
source-table/config copying, inherited state, incomplete transactions,
settlement gating, timeouts and close/revoke races. These are NOT hardware tests.

The post-link checker requires nine native executable definitions and the
native marker, in addition to the previous 89 definitions. Source/object and
ISA checks do not replace a full kernel link or an actual device qualification.

Missing top-level source: real boot handoff/ownership manager, stable Command
DB acquisition, shared ARC aggregation and parent clock/reset/IRQ/SMMU lifetime.
GPUCC/GDSC clock programming and a complete `ggpo_source` are not implemented by
this unit. Power-only boot remains inadmissible, and GMU/HFI/CP/render/Metal
enablement remains disabled until those independent prerequisites are satisfied.

## Shared CX and MX transport

The ABI 2 configuration can copy a second ARC address and its independent
selector mask. Both fields zero selects one resource. The original 40-byte
ABI 1 configuration remains accepted and is copied without reading the ABI 2
suffix. State and status objects must be rebuilt against the current headers.

`ggrsc_submit_arc` and `giulia_gpu_rsc_native_submit_arc` select one of those
copied bindings. The existing submit entry points continue to select the first
binding. Both resources share one assigned TCS, mapping, lock, grant and serial
stream. An in-flight request blocks submission to either resource.

Dual-resource sessions require ABI 2 native authority. Its approval callback
checks `scope.request_address` together with the selector and keeps that
aggregate reserved through completion. Take receives both bindings with
`request_address` zero. Settlement must reconcile every earlier request on
both resources before the shared grant can be dropped. A failed second vote
retains the first vote's completed serial and quarantines the shared session.

The 2026-11-18 Lanai boot source describes one HLOS DRV with ACTIVE TCS 0–2,
sleep TCS 3–4 and wake TCS 5–6. Its EBS callback uninstalls the RPMh protocol
and clears TCS control and command-enable fields for all seven TCSs. These
source facts guide the handoff implementation. Candidate execution and
post-EBS resource lifetime still require validation.

GCC and Clang RAM tests cover alternating resources, independent masks,
copied bindings, shared busy handling, legacy configuration bounds and loss
through the second request. Production registration and hardware power-only
qualification remain pending.

The local Lanai UEFI source for build
`23851_16.1.0.3_2026111805140000_noroot` confirms three ACTIVE TCSs and
two each for sleep and wake on the HLOS DRV. Its RPMh EBS callback uninstalls
the protocol and clears control and command enable for all configured TCSs.
The source digest and TCS partition are recorded in the hardware manifest.
Candidate EBS completion, current-boot delegation and persistent rail requests
still require a native platform source and observed handoff.
