# Transactional ARC aggregate planner (not a native authority source)

`giulia_gpu_arc_votes.c/.h` implements bounded multi-client interval aggregation
using the real Command DB decoder's level table. It is normal CPU RAM code,
not a hardware provider or an authorization mechanism. No production caller
activates this unit. Full kernel linkage is useful integration evidence but
does not grant access to RSC/GPU registers.

The future native source must serialize this state and the real RSC executor,
retain current-boot snapshot provenance, and represent **all** other clients'
constraints in the base interval. A caller cannot assume the APPS DRV is unused
or replace a firmware/VM owner with this local ledger. External aggregate
changes require the same transaction discipline; uncoordinated changes revoke
the source. This is not the complete shared-platform provider.

Each add/remove/base change computes max(minimums) and min(maximums), then
selects the lowest available ARC logical level in that intersection. The
table is copied. At most 16 tickets and one pending transaction are allowed.
No client is committed/removed before an exact epoch/serial and independently
verified transport completion. The owner must pin the pending ticket before
triggering hardware. `finish(APPLIED)` is a source contract, not proof created
by this function; echoing the requested plan does not satisfy it.

`UNCHANGED` is only legal for an unissued or independently proved unchanged
operation. A possibly triggered timeout, wrong completion or lost authority
quarantines without rollback, preserving old votes and pending transaction.
No automatic retry/cancel/reset exists. Stale completions cannot commit another
transaction. Dropping the last GPU ticket returns to the real base aggregate,
which may be nonzero; selector zero is never assumed to mean OFF.

`tests/ios16_gpu_arc_votes.c` executes the production C module and real ARC
selector under ASan/UBSan. Exhaustive two-client interval comparisons,
transaction barriers, stale/capacity/overflow handling, nonzero base levels,
copy semantics and uncertain completion retention are RAM-only validation.
Hardware completion, a current-boot DB, remote clients and native ownership
remain unverified. There is deliberately no terminal enable command.
