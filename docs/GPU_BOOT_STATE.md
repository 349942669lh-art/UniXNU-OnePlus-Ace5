# Per-boot ARC metadata and EBS observation

`GIULIA_GPU_BOOT_STATE=1` opts the GPU image builder into a fixed 256-byte
`/chosen/giulia-gpu-boot-state` request. The payload verifier requires its
compiled kernel consumer. The default image configuration keeps this request
absent. The host-only GPU profile can capture it.

DarwinBoot queries the pinned RPMh and Command DB providers after filesystem
closure and before the final memory-map acquisition. It copies the two actual
ARC resource addresses, numeric slave IDs and complete aux level tables for
`cx.lvl` and `mx.lvl`. Query status, aux result and returned capacity are checked
separately. The data is a resource snapshot acquired through firmware APIs.

The source RPMh resource table uses realloc and sorted insertion. A retained
pointer to one of its command elements could become stale. Lookup and command
copy run at `TPL_NOTIFY`. Only the independently allocated voter-list heads
are retained inside the loader until successful EBS return.

Immediately after successful EBS, the loader masks DAIF as DarwinEnter already
does for kernel entry. It reads those retained normal-RAM voter nodes and
copies the final DRV2 active aggregate only when the resource is idle, its
active vote is clean and its completed hardware cache agrees. A missing voter
stays explicitly unknown. The loader records counter values before and after
EBS and reseals the snapshot CRC.

That final path calls loader-resident code and reads normal RAM. Boot Services
and protocol methods have finished. The candidate's audited RPMh EBS callback
uninstalls its protocol and clears TCS control and command enable while leaving
the voter allocations in RAM. The copy completes before DarwinEnter cleans the
owned kernel/ADT region and before XNU can reclaim Boot Services memory.
`verify-gpu-boot-state-abi.py` pins the exact candidate RPMh and CmdDb binaries
whose protocol and internal cache layouts are used.

The wire record contains values and reserved zero fields. Its phases are
request, captured before EBS, and returned from EBS. CRC, lengths, names, ARC
type, aux ordering, selector bounds and reserved fields are validated before
the kernel publishes the record and logs it through `gpu.status`.

The snapshot supplies observations for the native source implementation.
Current-boot resource assignment, native mapping grants, persistent supply and
clock ownership, transfer/revocation synchronization and powered reset exit
still need their own implementation and hardware qualification. The consumer
publishes `grant=0` and does not register a power source from this record.

Host tests compile the actual producer and decoder. They exercise every
firmware callback failure, unsupported protocols, malformed aux data,
in-flight and dirty votes, missing votes, moved resource tables, four-byte
alignment, post-EBS updates, abort/repeated finish, CRC corruption, payload
round trips and loader installation order.
