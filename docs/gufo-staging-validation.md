# Gufo staging validation: November 11, 2026

These additions address findings in [Gufo #1144](https://github.com/gufo-org/gufo/issues/1144)
after staging base `d8345774e34e0b0824f11661d331a323e8771dd9`. Each change
has its own branch, merged separately into `gufo-staging`. None is an
upstream merge.

## Preserve floating vector signs

Branch: `fix/aie2p-narrow-sign`, commit `2e00d48926`.

**Issue:** AIE2P's software vector F32-to-F16 conversion loses negative signs.
The F32-to-FP8 recipes share the same problem. The isolated F32 sign bit is
negative when interpreted as a signed accumulator; saturating SRS to an
unsigned result clamps it to zero. The test evaluator incorrectly treated the
accumulator as unsigned, hiding the problem in numerical boundary tests.

**Fix:** Disable saturation only while narrowing that sign payload, then restore
saturation for magnitude conversion. Correct the evaluator's signed accumulator
and unsigned destination semantics. The FP8 paths from 16-bit inputs retain
their previous instruction sequence.

**Gufo benefit:** Negative NPU activations and signed zero survive conversion.
LFM3's `@tof16` integer sign-repair workaround can be removed once its runtime
uses this fix. That model-side removal is not included here. No end-to-end
Gufo speedup has been measured; this is a correctness fix with a potential
reduction in workaround instructions, not another GPU decode throughput gain.

Validation on the local Strix Halo NPU:

- Correcting only the evaluator reproduces failures in both existing F16 and
  FP8 rounding-boundary tests. With the recipes fixed, all 39 selected
  non-exhaustive conversion tests pass.
- The initial native F16 regression fails all three trials using the original
  staging recipe and passes all three using the candidate. It compares bits
  against scalar conversions lowered independently for the VM reference.
- The final maintained native fixture passes all 12 trials: three mixed-sign
  random trials and one negative-zero trial for each of F16, F8E4M3 and F8E5M2.
  There are no skipped scenarios or planning issues.
- All six existing `numeric_conversion*.loom-test` fixtures and the new F16
  lowering fixture pass. The FP8 golden change adds saturation control only
  around the affected F32 sign extraction.
- Canonical formatting, Ruff, Buildifier, package boundaries, generated-file
  checks, authored-source lint and repository source lint pass individually.

## Describe pre-encoded NPU matrix weights

Branch: `docs/xdna-packed-weight-layout`, commit `f0aad2317e`.

**Issue:** Host code preparing `bfp16ebs8` weights had to infer the
`target_fragment` byte layout from an execution test.

**Fix:** Add a navigable target reference specifying 72-byte operands,
row exponent/mantissa offsets, finite-value decoding, RHS orientation for
`A * B^T`, the schema, and grouped fragment loads and slices.

**Gufo benefit:** Weight packing and loading code has a maintained reference
for pre-encoded operands. This supports avoiding repeated weight encoding;
the documentation itself changes no execution behavior and establishes no
additional speedup.

Validation: the maintained packed-matrix test passes three native trials
against its independent scalar decoder. An eight-block variant matching the
page's 576-byte group also passes three native trials. Navigation and relative
links resolve; the existing site contract test passes. A complete static-site
build was not run.

## Schedule AIE2P element-wise leaves in source order

Branch: `perf/aie2p-schedule`, commit `b2409b515`.

**Issue:** The AIE2P leaf scheduler always used resource-stall scoring. In
element-wise code it serializes independent chains that the source order
interleaves: four independent `vadd.f32x16` chains issued one add every three
cycles, although the physical planner accepts one per cycle. Only 14 of 251
instructions in that loop co-issued. Source-priority scheduling issues the
same loop at one add per cycle. Matrix loops show the opposite: source order
makes K-split `mma.bfp16ebs8` projections five times slower.

**Fix:** Each leaf chooses its strategy once, before planning, from its Low
packets: resource-stall when it contains `mma.*`, source priority otherwise.
Planning rewrites the function (fixed-input copies, guarded motion, loop
placement), so planning twice and comparing is not sound without a clone.
`LOOM_AIE2P_SCHEDULE_STRATEGY=<0..3>` forces one strategy. With
`LOOM_AIE2P_DUMP_BUNDLES=1`, every planned bundle is printed: issue cycle,
logical cycle, block and slot mnemonics. Before this there was no view of the
final VLIW schedule.

**Gufo benefit:** LFM3-1.7B NPU prefill of 2,117 tokens drops from 6.47 s to
5.96 s, with identical outputs. The SwiGLU row kernel drops from 1.89 ms to
1.28 ms per 64 rows; SwiGLU time per 64-row pass falls from 43 ms to 29 ms. The
K-split projections are unchanged.

Validation on the local Strix Halo NPU:

- 270 AIE2P `.loom-test` files were run with `loom-check`, with and without
  the change (`LOOM_AIE2P_SCHEDULE_STRATEGY=3` reproduces the previous
  behavior). Both runs fail the same 59 files; those failures exist on this
  base without the change. Eight files had schedule-dependent code bytes or
  sizes. Their goldens were updated, and one compile-report emission line now
  matches its schedule-dependent counts with wildcards.
- On device, the K-split projection results match an independent host
  reference. For LFM3's teacher-forced next-token distributions (185 positions
  against an FP32 GPU reference), the results are bit-identical before and
  after the change.

## Build and qualification limits

Compiler tools were built in Release with the Gufo Nix development environment
(GCC 16.2), AIE2P/ELF and VM execution enabled. Local configuration disables
warnings-as-errors because unrelated existing GCC warnings prevent this base
from building with its default setting. No compiler-warning policy was changed
in source.

The aggregate Bazel hygiene command remains incomplete: generated-tool actions
fail with `env: 'python3': No such file or directory` in their action
environment. This is recorded separately from the individually passing checks
above; the aggregate gate is not reported as passing.

The called loop-carried F32 helper crash did not reproduce in the small
add/dot/multiply and reconstructed Newton-loop programs tried here. It remains
open, as do the other unchecked runtime and Loom findings in #1144. No model
quality sweep, Gufo inference benchmark, GPU performance claim, or general
cross-device qualification is implied by these focused NPU checks.
