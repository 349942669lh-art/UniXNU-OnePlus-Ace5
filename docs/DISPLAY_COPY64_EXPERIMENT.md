# Opt-in CPU framebuffer write experiment

`sweetfb_copy64=1` enables a general-register-only CPU copy variant. The
existing scalar algorithm remains the default. This is **not GPU acceleration**.

The driver keeps its current 1125x2436 logical canvas, 1264x2780 native panel,
source gather/nearest-neighbour transform, clipping, scheduling, capture lock,
memory attributes, source validation, CPU count and touch implementation.
For normal WriteCombine framebuffer RAM only, it assembles four output pixels
into two 64-bit values and uses fixed-size builtin copies. Destinations are
aligned before each group; scalar tails handle all remaining pixels. It does
not read panel memory or touch additional hardware registers. All source reads
remain in the same previously checked range. Unsupported host byte orders use
the scalar path.

Host validation compares every output byte and return value with a frozen
pre-change implementation across full coverage, 4-byte/8-byte destinations,
clipped rectangles, deterministic random transforms, guard bytes and invalid
inputs. ASan/UBSan are used. A separate ordered-profile check verifies the
actual generated header and opt-in integration. Existing callback/timer tests
continue to run. These tests are not phone performance measurements.

Deployment requires a newly compiled and verified XNU kernel and explicit
local boot arguments. Keep the existing image as rollback; no GPU/SCM/GMU/CP,
clock, power, SMP or permanent boot-partition change is part of this experiment.
Measure aggregate `FBPERF` present wall time after boot, plus real UI behavior;
these counters do not measure physical FPS or prove reduced application-render
cost. A copy speedup alone must not be reported as fixing UI lag.
