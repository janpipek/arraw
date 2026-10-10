# Brush prototype: measurements and recommendations (plan step 4)

- Date: 2026-10-09. Branch `brush-prototype`, uncommitted, HEAD `cbb80c2`.
- Code measured:
  - `src/core/{BrushStrokes,BrushRaster,BrushCoverageCache,StrokeCodec}`;
  - benches B1 to B7 in `tests/test_BrushBench.cpp`;
  - the unit tests in `tests/test_Brush*.cpp` and `tests/test_StrokeCodec.cpp`.
- Review and fix verification: `docs/reviews/claude-opus-5-5_2026-10-09_brush-prototype.md`.
- Contract: ADR 044 §6, with the spec's D1 to D6 and the thin-stroke rule (review finding 1).
- **Accepted 2026-10-10 (user):** the preferred option of every recommendation below (1a, 2b,
  3a, 4b with an area budget of 4 and radius 1, 5a). ADR 044 sections 6, 7 and 9 were amended
  with the text proposed here, and `docs/ideas/local-adjustment-plan.md` records step 4 as done.
- This report proposed the ADR text; the ADR now holds it.

## Set-up

| | |
|---|---|
| Machine | Intel Core i7-7700HQ @ 2.80 GHz, 4 cores / 8 threads (`nproc` 8), 31 GiB RAM, laptop, dev container on Linux 7.2.7 (Fedora 44 host) |
| Build | `release` preset (`build/container-release`), GCC 15.2, `-O3 -DNDEBUG`, `-ffp-contract=off` |
| Runs | `arraw-tests "[brush-bench]"`, three full runs and a fourth. In run 1, B5 and B6 aborted on the broken `ARRAW_BENCH_OUT` path (see the review), so those two have three runs, not four. |
| Reporting | Median of the runs, with [min..max] where they differ by more than about 5%. MiB = 2^20 bytes. 24 MP = 6000×4000. |
| Extra | A scratch program linked against `libarraw.a` measured edge-pixel error, edge width and banding crops. It was not committed. |

`just test`: 1497/1497 pass. `"[brush]"` in Debug and Release: 29 cases and 32 598 assertions pass.

## Measurements

### Encoding (B1)

Three sets:
- realistic: 16 masks × 60 everyday strokes, 310 153 points;
- heavy: 16 × 100 strokes × 500 points, 800 000 points;
- zigzag: one dense 10 000-point stroke.

| | text | base64-delta | plain f32 base64 (reference) |
|---|---|---|---|
| bytes per point, realistic / heavy / zigzag | 21.3 / 21.3 / 20.6 | 7.65 / 7.62 / 5.34 | 10.7 |
| per 1000 points | 21.3 KB | 7.6 KB (5.3 KB dense) | 10.7 KB |
| sidecar file, realistic / heavy | 6.63 / 17.06 MB | 2.39 / 6.13 MB | |
| `readSidecar` (XMP parse), heavy | 216 ms [209..230] | 75 ms | |
| decode only, heavy | 48 ms | 31 ms | |
| **read + decode, heavy** | **262 ms** [257..282] | **106 ms** | |
| read + decode per 1000 points | 0.33 ms | 0.13 ms | |
| encode, heavy | 95 ms | 19 ms | |
| exact round trip (all sets, and B2's replay) | yes | yes | |

Rule 3 picks text unless (the heavy text sidecar is over 5 MB **or** read plus decode is over
250 ms) **and** base64 is at least 2× better on both. Text fails both limits (17.1 MB and 262 ms).
Base64 is 2.78× smaller and 2.48× faster. The rule therefore picks **base64-delta**. On the
realistic set base64 is 2.77× smaller and 2.0× faster.

### Sixteen full-size masks at 24 MP (B2)

16 realistic masks, 55 700 dabs.

| | |
|---|---|
| rasterise all 16, 8 threads | **13.4 s** [12.7..13.9], 0.73 to 0.97 s per mask |
| one mask, 1 thread | 3.16 s |
| float coverage | 91.6 MiB per mask (1465 MiB for 16 if all are held) |
| quantise and pack 4 masks into RGBA8 | 46 ms |
| GPU textures, 16 masks, full / one level coarser | RGBA8: 366 / 92 MiB; RGBA16F: 732 / 183 MiB |
| replay after reload, both codecs | bit-identical, all 16 |

### Long strokes and the live stroke (B3)

A 10 000-point zigzag (radius 0.02) places 2 280 dabs in 0.1 ms. It rasterises in 246 ms at 24 MP
and 12.8 ms at 1500×1000.

The live stroke has 2 000 points, fed 10 at a time (200 updates), on top of 20 strokes. Two ways
to draw each update:
- **whole**: the cache repaints the whole partial stroke each time;
- **tail**: keep "prefix + settled dabs" and paint only the new dabs, then the end dab onto a
  clone. The final bits are `REQUIRE`d equal.

| size, tile | whole: median / p95 / max ms | tail: median / p95 / max ms | dirty tiles per update, whole / tail (median) |
|---|---|---|---|
| 1500×1000, 128 | 3.0 / 4.8 / 5.1 | 0.21 / 0.26 / 0.34 | 14 / 8 |
| 1500×1000, 256 | 3.8 / 4.9 / 6.7 | 0.25 / 0.52 / 0.85 | 5 / 2 |
| 6000×4000, 128 | 32.8 / 60.9 / 63.7 | **1.36** / 1.64 / 1.78 | 154 / 36 |
| 6000×4000, 256 | 34.1 / 62.6 / 68.0 | 1.59 / 1.97 / 2.30 | 46 / 12 |

### Painting session through the cache (B4)

Two sessions:
- painting: 200 everyday strokes;
- overpaint: 300 strokes in a 0.2 × 0.2 box.

Each is followed by 50 undos under the default 512 MiB budget. Medians in ms; the three runs agree
within about 5%.

| size, tile, session | append median / p95 / max | full raster each time | dirty tiles redone from all strokes | incremental vs redone | memory, all states float | newest float + older u8 | undo hits / 50 |
|---|---|---|---|---|---|---|---|
| 1500×1000, 128, painting | 1.82 / 4.5 / 5.6 | 86 | 28 | 20× faster | 124 MiB | 35 MiB | 50 |
| 1500×1000, 128, overpaint | 2.10 / 6.3 / 10.6 | 253 | 285 | 180× | 171 | 43 | 50 |
| 1500×1000, 256, painting | 1.83 / 4.5 / 6.2 | 83 | 23 | 14× | 207 | 56 | 50 |
| 1500×1000, 256, overpaint | 1.70 / 4.0 / 7.0 | 252 | 174 | 135× | 270 | 68 | 50 |
| 1500×1000, 512, painting | 2.44 / 7.0 / 12.1 | 86 | 64 | 30× | 388 | 101 | 50 |
| 1500×1000, 512, overpaint | 2.27 / 6.2 / 8.8 | 257 | 301 | 160× | 571 | 144 | 50 |
| 6000×4000, 128, painting | **12.5** / 39.7 / 70.0 | 1410 | 62 | 6× | 1136 | 353 | 50 |
| 6000×4000, 128, overpaint | **13.3** / 46.4 / 72.7 | 4085 | 1178 | 110× | 1398 | 359 | 50 |
| 6000×4000, 256, painting | 14.3 / 41.4 / 71.6 | 1370 | 96 | 7× | 1403 | 419 | 50 |
| 6000×4000, 256, overpaint | 16.6 / 48.7 / 81.5 | 3950 | 1600 | 130× | 1762 | 451 | 50 |
| 6000×4000, 512, painting | 16.7 / 49.5 / 84.8 | 1388 | 159 | 10× | 1973 | 562 | **43** (7 misses, 2.2 s each) |
| 6000×4000, 512, overpaint | 20.3 / 66.1 / 114 | 3978 | 2269 | 160× | 2697 | 686 | 50 |

- An undo hit takes under 1 µs.
- An older state kept as u8 costs 0.15 MiB at 1500×1000 and 1.2 to 1.3 MiB at 24 MP, both with
  tile 128.
- The "newest float + older u8" column is arithmetic over distinct tiles, not a measured
  implementation.

### Precision and banding (B5)

The test stroke is soft, wide and low-flow: radius 0.3, hardness 0, flow 0.05. It is drawn on its
own and as 4 overlapping copies across 6000×4000. Coverage drives up to +4 EV on mid-grey, the
largest local exposure. The values are ΔL*.

| quantiser | overlaps | neighbour step | contour between plateaus ≥ 4 px | 8×8-blurred step | 8×8-blurred error vs float | longest plateau |
|---|---|---|---|---|---|---|
| float | 1 / 4 | 0.011 / 0.041 | 0 / 0 | 0.011 / 0.041 | 0 / 0 | 1 px |
| u8, rounded | 1 / 4 | 0.281 / 0.396 | **0.280 / 0.396** | 0.035 / 0.077 | 0.131 / 0.173 | 156 / 174 px |
| u8 + 4×4 Bayer | 1 / 4 | 0.281 / 0.728 | **0 / 0** | 0.018 / 0.049 | 0.021 / 0.025 | 47 / 37 px (no contour) |
| half (RGBA16F) | 1 / 4 | 0.017 / 0.060 | 0.009 / 0.049 | 0.011 / 0.041 | 0.004 / 0.020 | 24 / 36 px |
| u16 | 1 / 4 | 0.011 / 0.042 | 0.001 / 0.002 | 0.011 / 0.041 | 0 / 0 | 14 px |

At +1 EV every 8-bit figure is about 4.5× smaller: plain u8 has a contour of 0.062 to 0.067.

**Visible-banding judgement:**
- **Plain u8** makes regular contours of about 0.3 to 0.4 ΔL*, 150 to 175 px apart, at +4 EV. A
  20×-stretched crop of the error against float shows clean horizontal bands across the stroke.
- Such a step is at or just under the usual 0.5 ΔL* just-noticeable difference for adjacent
  patches. Mach banding on smooth gradients is seen below that, so on a calibrated display it is
  **borderline visible**, and it fails a 0.3 threshold.
- An 8-bit display or JPEG adds its own step of about 0.4 L* near L* 58. Coverage contours
  therefore matter most in 16-bit exports and on 10-bit displays.
- **Bayer-dithered u8** shows no contour. In the stretched crop only a fine 4×4 texture of one
  code is left, which is not visible at 1:1. Its blurred error (0.02 ΔL*) is 6 to 7× smaller than
  plain u8's.

### Resolution at hardness 1 (B6, plus the probe)

There are 40 detail strokes (hardness 1, radius 0.002 to 0.01) and 3 large hardness-1 strokes.
- **full**: the strokes rasterised at the frame size.
- **coarse**: rasterised one level coarser (`ceil` halving), then bilinearly upsampled to the
  frame, with centres aligned.
- **ideal**: twice the frame size, box-halved.

Errors are absolute coverage differences, 0 to 1.

| frame | variant | mean | max | fraction > 4/255 | **fraction > 32/255** | raster ms | float MiB |
|---|---|---|---|---|---|---|---|
| 3000×2000 | full | 0.001 | 0.62 | 0.006 | **0.003** | 28 | 22.9 |
| 3000×2000 | coarse | 0.003 | 0.72 | 0.017 | **0.008** (2.7×) | 6.5 | 5.7 |
| 1500×1000 | full | 0.002 | 0.66 | 0.011 | **0.006** | 6.8 | 5.7 |
| 1500×1000 | coarse | 0.006 | 0.72 | 0.031 | **0.016** (2.8×) | 2.4 | 1.4 |

Error on edge pixels only, where `1/255 < ideal < 254/255` (the probe):

| frame, set | full: mean / p99 | coarse: mean / p99 |
|---|---|---|
| 3000×2000, detail (r 0.002 to 0.01) | 0.036 / 0.31 | 0.104 / 0.56 |
| 3000×2000, fine (r 0.0005 to 0.0015) | 0.119 / 0.39 | 0.243 / 0.64 |
| 1500×1000, detail | 0.061 / 0.34 | 0.166 / 0.60 |
| 1500×1000, fine | 0.145 / 0.41 | 0.229 / 0.57 |

- The 10 to 90% width of a hard edge is 1.6 px full, 2.2 to 3.4 px coarse (depending on phase)
  and 1.5 px ideal.
- Side by side at 2×, the coarse crop is visibly softer, and the full crop cannot be told from the
  ideal.
- After the thin-stroke fix, the hardness-1 error at two sizes (T-R4) is a mean of 0.0025.

### Caps, worst case (B7)

Stand-ins for the worst list the current caps allow are extrapolated to 2 000 strokes × 10 000
points.

| stand-in | 1500×1000: measured | 1500×1000: at the caps | 24 MP: measured | 24 MP: at the caps |
|---|---|---|---|---|
| radius 1, frame-crossing zigzag | 100 pts: 0.90 s | 1.8·10^5 s (50 h) | 10 pts: 1.38 s | 2.8·10^6 s |
| radius 0.0005, frame-crossing | 200 pts, 1.43 M dabs: 83 ms | 8 300 s | 162 ms | 16 000 s |
| 2 000 everyday strokes × 100 pts | 0.57 s | 57 s | 8.75 s | 875 s |
| sidecar at the caps | | text 405 MiB, base64 149 MiB | | |

- Throughput is 6.3 to 8.9·10^8 dab-pixel evaluations per second on 8 threads.
- Cost follows the **swept area** `A = Σ length × radius`, in long-edge units². It does not follow
  the point count.
- On this machine `t ≈ 1.0 s × A × (L / 6000)²`. The model predicts 0.75 s for a realistic B2
  mask (A ≈ 0.84), against 0.84 s measured, and 7.8 s for the 2 000-stroke stand-in (A ≈ 8.75),
  against 8.75 s measured.
- Rule 4's limits (10 s at 1500×1000, 50 MB of sidecar) fail by 4 to 6 orders of magnitude.
- After the caps of 2026-10-10 B7 builds lists that fill the budgets and measures them (no
  extrapolation): 0.1 to 0.7 s at 1500×1000 and 0.3 to 3.5 s at 24 MP; a mask at its point cap
  is 0.75 MiB in base64. B4's overpaint session now stops at the budget, at 227 strokes.

## Recommendations

For each open decision, the options, with the preferred one marked.

### 1. Coverage resolution

| | option | for | against |
|---|---|---|---|
| **a (preferred)** | **The rendered source's size**, one coverage pixel per source pixel, as provisional. A preview renders a reduced source, so its coverage is already at preview size. | Exact, and parity needs only `texelFetch`. Rule 1 holds: coarse has 2.7 to 2.8× full's pixels over 32/255, edges 1.4 to 2× wider, and visibly softer. | 4× the memory and time of coarse at the same render size. |
| b | Always one level coarser, with a hand-written bilinear lookup in both backends | 4× less memory, 3.6 to 4.3× faster | Hard edges soften (2.2 to 3.4 px). A second pixel loop to keep in parity. |
| c | Full size for commit and export; coarse only while a stroke is drawn | Faster live feedback | The live stroke is already 1.4 ms at 24 MP with the tail method. Two rasters to reconcile on commit. |

Reload latency follows from (a): 16 masks at full 24 MP take 13.4 s. Step 5 should rasterise at
the displayed size on open and rasterise full size lazily, or in bands at export.

### 2. Precision and texture packing

| | option | memory, 16 masks at 24 MP | worst contour at +4 EV |
|---|---|---|---|
| a | RGBA8, rounded (provisional) | 366 MiB | 0.28 to 0.40 ΔL*: passes Q1 at 0.5, fails at 0.3 |
| **b (preferred)** | **RGBA8 with a 4×4 ordered (Bayer) dither when the CPU quantises**, anchored to raster pixel coordinates and offset per mask so that masks do not dither in step | 366 MiB | 0 (blurred error 0.02 ΔL*) |
| c | RGBA16F, four per texture | 732 MiB | ≤ 0.05 ΔL* |

- (b) keeps the provisional bindings, the memory and exact parity, because the CPU quantises and
  both backends `texelFetch` the same texels. It meets the stricter Q1 threshold on what is
  visible.
- Choose (c) if a dither texture is unwanted. It costs twice the memory and nothing else, since
  `src/gpu` already uses RGBA16F.
- R16 per mask was not pursued. It needs 16 bindings for 16 masks, and R16 is not guaranteed on
  GLES.

### 3. Stroke encoding in XMP

| | option | per 1000 points | heavy sidecar, read + decode |
|---|---|---|---|
| **a (preferred)** | **base64-delta**: style as float bits, points as zigzag varints of the deltas of order-preserving bit patterns, canonical | 7.6 KB | 6.1 MB, 106 ms |
| b | text `radius hardness flow erase; x,y …` in shortest decimal | 21.3 KB | 17.1 MB, 262 ms |
| c | hybrid: style as readable XMP fields per stroke (`arraw:radius` and so on), points as base64-delta | about 7.7 KB, plus about 150 B per stroke | about as (a) |

- (a) is what rule 3, agreed before measuring, picks.
- Both encodings round-trip bit-exactly.
- (c) costs little and keeps the style readable and diffable. Take it if readability of the style
  matters to you; the points are unreadable in (a) and (c) alike.
- Quantising positions to fixed point gains nothing. A float's order-preserving delta is already
  about 2 to 3 bytes per coordinate, and fixed point would lose exactness.
- State JSON keeps readable numbers (shortest decimal) either way.

### 4. Caps

| | option | bounds time? | bounds sidecar? |
|---|---|---|---|
| a | Lower the counts only: 10 000 points per stroke, 2 000 strokes, plus 100 000 points per mask | no (one radius-1 stroke can take minutes) | yes: ≤ 0.76 MB per mask in base64 |
| **b (preferred)** | **(a), plus per-mask budgets that need no raster size: swept area `Σ length × radius ≤ 4` and dab count `Σ length / (0.25 × radius) ≤ 2·10^6`**, with lengths in normalised (u, v) units, which bound the long-edge metric for every aspect | yes: about 4 s per mask at 24 MP and 0.25 s at 1500×1000 on this laptop (a realistic mask is A ≈ 0.84) | yes |
| c | (a), plus a smaller maximum radius (0.25) instead of the area budget | partly: each dab costs 16× less, but many points can still sum to minutes | yes |

- With (b) the radius range of D4 stays [0.0005, 1] (Q3). A radius-1 dab costs 0.2 s at 24 MP,
  and the area budget allows at most about 16 of them.
- The area budget can be 2, 4 or 8, a trade between headroom and the worst export time. 4 is
  about 5× a realistic heavy mask.
- The edit rules refuse an append past any budget, which is what the UI shows as "mask full". A
  reader drops strokes from the first one that would pass a budget, with a warning.

### 5. The cache

| | option | for | against |
|---|---|---|---|
| **a (preferred)** | **Float tiles of 128 px for every held entry, LRU on a 512 MiB budget of distinct tiles, serving the sizes the window renders. Export rasterises in bands outside the cache. The live stroke uses the tail method.** | Rule 5 gives 128: the best append median in 3 of 4 cases, and 256 is best only in 1500×1000 overpaint. Float retention beats redone tiles by 6 to 180× (rule 5 needs 3×). The window size holds a 300-stroke session in 171 MiB. | At 24 MP a long session is 1.1 to 2.7 GiB unbounded, so 512 MiB evicts, and with tile 512 there were 7 undo misses of 2.2 s |
| b | (a), but undo states older than the newest kept as u8 tiles | 3 to 4× more undo states per MiB (1.3 MiB per state at 24 MP) | An append after an undo has no float base. It redoes its dirty tiles from all strokes: 62 ms to 1.2 s at 24 MP. |
| c | No float retention: every append redoes its dirty tiles from all strokes | Least memory | 6 to 180× slower appends: 1.2 s per overpaint stroke at 24 MP |

For step 5, finding the tiles to upload means comparing tile pointers against the grid on the GPU.
`Result::dirty` is only a hint.

## Proposed ADR 044 amendments (exact text)

These blocks are proposed text for the user to apply. Each replaces the named part whole.

### §6, replacing the whole section

````markdown
### 6. Brush: the stroke contract

```text
StrokeList   rasteriser: std::uint32_t;
             strokes: std::vector<std::shared_ptr<const Stroke>>; a content hash
Stroke       radius (long-edge units, 0.0005 to 1), hardness (0 to 1),
             flow (0 to 1), erase (bool), points: std::vector<SensorPoint>
             (1 to 10 000)
```

Strokes, not rasters, are stored, in the sensor frame (ADR 009). Pressure is
reserved and ignored. A list is persistent: appending makes a new list that
shares every earlier stroke and extends the content hash.

**Dabs by distance.** Arc length is measured in the long-edge metric of the
raster being made: a point `(u, v)` sits at `(u * W / L, v * H / L)`, with
`W x H` the raster and `L = max(W, H)`. A dab is placed at every arc length
`k * s` from the first point, with `s = 0.25 * radius` and `k = 0, 1, …`,
computed from the integer `k`, never by accumulation. One more dab is placed
at the last point when `(k_last) * s` falls short of the length. A single
point, or a run of equal points, is one dab. Placement is in double precision.

The dabs do not depend on the pointer's event rate. An extra point on a straight
segment gives the same coverage bit for bit when the split is exact in
floating point, and within 1e-5 otherwise. Extending a stroke keeps every
`k * s` dab and moves only the end dab.

**Dab profile.** In raster pixels, with `R = radius * L`, a dab is drawn with
radius `R_d = max(R, 1.5)` and flow `f_d = flow * (R / R_d)^2`. A thin stroke
therefore keeps its weight: it neither falls between pixel centres nor changes
with its sub-pixel position. With `inner = min(hardness * R_d, R_d - 1)`, at a
distance `r` from the dab's centre:

```text
d = 1 - smoothstep(inner, R_d, r)          (0 for r >= R_d)
paint:  m <- m + f_d * d * (1 - m)
erase:  m <- m * (1 - f_d * d)
```

Coverage `m` starts at 0. Strokes apply in list order, and dabs in path order.
Pixel `(i, j)` is evaluated at its centre `(i + 0.5, j + 0.5)`. The pixel loop
uses only `+ - * /`, `sqrt` and comparisons, and is built without
floating-point contraction. A pixel's bits therefore do not depend on the
banding, the tiling or the platform. Two golden digests pin version 1.
Radius, hardness, flow and erase are captured per stroke when it begins.

**Rasteriser version.** `StrokeList::rasteriser` is 1 for the rules above. An
arraw keeps rendering every version it has written. A version it does not
know drops the mask with a warning (section 9).

**Caps.** Per stroke, 1 to 10 000 points. Per mask, at most 2 000 strokes and
100 000 points in all. Two budgets bound the work without knowing a raster
size, with lengths measured in normalised `(u, v)` units, which bound the
long-edge metric for every aspect:
- swept area, `Σ length * radius <= 4`;
- dabs, `Σ length / (0.25 * radius) <= 2 000 000`.

The edit rules refuse an append past any cap or budget with
`std::invalid_argument`, and leave the list unchanged.

**Rasterising and caching.** `rasteriseBrush(strokes, size)` is a pure
function and the reference. There is no pixel scale: radii are in long-edge
units, so the size alone fixes the pixels. A cache in front of it is an
optimisation that correctness never relies on:
- it is keyed by the stroke list (pointer, then content hash and contents),
  the raster size and the rasteriser version;
- it holds float coverage in 128-pixel tiles. An appended stroke is painted
  onto clones of the tiles its dabs touch, on top of the longest held prefix.
  Untouched tiles are shared between entries;
- it is bounded by an LRU budget of 512 MiB of distinct tiles, and it serves
  the sizes the window renders. Export rasterises outside it, band by band.

Cached and uncached coverage are equal bit for bit.

**During a gesture** the window keeps two tile states. The first holds the
prefix plus the live stroke's settled dabs (those at `k * s`). The second is
that plus the end dab. A pointer update paints only the new settled dabs onto
clones of the first state, then the end dab onto a clone of that. This equals
the reference bit for bit, because the end dab is painted last. Commit freezes
the stroke into a new `StrokeList` that shares the previous strokes. Undo
restores the previous list, whose tiles the cache still holds. Cancel drops
the live stroke.

**Resolution.** Coverage is rasterised at the rendered source's size, one
coverage pixel per source pixel. A preview's coverage is therefore at the
preview's size. One level coarser was measured and rejected: at hardness 1 it
puts 2.7 to 2.8 times as many pixels more than 32/255 off, and edges grow from
1.6 to between 2.2 and 3.4 px.

**Tests.**
- The same path sampled at different event rates gives identical coverage
  when the split is exact, and within 1e-5 otherwise.
- The same strokes at two raster sizes agree after box-halving: max 0.02 and
  mean 0.00015, or a mean of 0.005 at hardness 1.
- A thinnest stroke has a stable weight across sizes and sub-pixel phases.
- Dab placement matches an independent placement from this text.
- A reloaded stroke list rasterises bit-identically.
- A cached and an uncached raster are equal, for any tile size and thread
  count.
- An extended stroke differs from the original only within `R_d + s` of the
  old end point and within `R_d` of the added path.
- Rasteriser 1's golden digests hold.
````

### §7, replacing the paragraph "Brush coverage packing (provisional)"

```markdown
**Brush coverage packing.** Four brush masks go in each RGBA8 texture, in up
to four textures (bindings 8 to 11). Every QRhi backend supports RGBA8, so this
needs no format check. The CPU rasterises float coverage and quantises it to 8
bits with a 4x4 ordered (Bayer) dither. The dither is anchored to the raster
pixel and offset per mask, so that masks do not dither in step. Both backends
read the same texels (`texelFetch` at the pixel, as the source), so coverage
adds nothing to the parity gap.

Measured at +4 EV over a soft, low-flow stroke, plain rounding left contours of
0.28 to 0.40 ΔL* between plateaus 156 to 174 px wide. The dither leaves no
contour, and a blurred error of 0.02 ΔL*. A brush mask's `coverage texture` and
`channel` say where it lives. Sixteen full-size brush masks take 366 MiB at
24 MP. RGBA16F, at twice the memory, is the fallback if the dither is ever
unwanted.
```

### §9, replacing the paragraph "Brush strokes (provisional)…"

```markdown
Brush strokes: `arraw:strokes` is an `rdf:Seq` with one base64 string per
stroke (RFC 4648 alphabet, padded, no line breaks). The bytes are:
- the format, 1;
- flags (bit 0 erase, the others 0);
- radius, hardness and flow as little-endian float32 bits;
- the point count as a LEB128 varint;
- for each point, `u` then `v`, each as the zigzag LEB128 difference of the
  float's order-preserving bit pattern from the previous point's (from 0.0 for
  the first point).

The form is canonical: shortest varints, zero padding bits, and nothing after
the padding. It round-trips exactly. It takes about 7.6 bytes per point, where
shortest-decimal text took 21.3. The caps are those of section 6, and the edit
rules hold them, so a saved list always reads back equal. On reading, points
past 10 000 are cut from their stroke and strokes past 2 000 are dropped. From
the first stroke that would pass the mask's point, area or dab budget, that
stroke and every later one are dropped. Each of these gives a warning.
```

### Plan (`docs/ideas/local-adjustment-plan.md`), "Open (for the ADR)"

Replace the two brush lines:

```markdown
- Brush coverage resolution and texture packing (after the prototype).
- Stroke encoding: text vs base64 binary (after the prototype).
```

with:

```markdown
- ~~Brush coverage resolution and texture packing~~: settled by the brush
  prototype (`docs/ideas/brush-prototype-report.md`). Full rendered-source
  size; RGBA8 four per texture with an ordered dither (RGBA16F if the dither is
  unwanted).
- ~~Stroke encoding~~: settled. Base64-delta per stroke, with caps of 10 000
  points per stroke, 2 000 strokes and 100 000 points per mask, plus swept-area
  and dab budgets.
- For step 5: rasterise the displayed size first on open; full size lazily or
  in bands at export.
```

## Decisions for the user

- **Q1 (banding threshold):** with the dither, 8-bit meets 0.3. Do you accept the dither (2b), or
  do you prefer RGBA16F (2c)?
- **Q2 (caps):** do you accept the per-mask budgets in 4b? Is 4 long-edge units² the right
  swept-area budget, or should it be 2 or 8?
- **Q3 (radius):** keep the maximum radius at 1 (with 4b), or lower it to 0.25 (4c)?
- **Encoding:** base64 (3a) as rule 3 says, or the hybrid (3c) for a readable style?
