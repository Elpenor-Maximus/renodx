# Donkey Kong Bananza PC Recompilation

Experimental RenoDX add-on for the Vulkan `dkb.exe` of the DKB PC recompilation (game 3.2.0) in native HDR. It replaces the native HDR scene composites: the gameplay composite (final Vulkan fragment module, RenoDX CRC32 `0xB537A7CA`) from a build-time entry, and every other composite permutation it recognises by rewriting it when the game creates it (the structural rewrite). The game still runs its bloom, LUT grade, HUD composite and HDR10 (PQ) encoder.

Below the game's HDR knee the image is the game's own. Above it, RenoDX's three-input `ToneMapPass` (RenoDRT, Neutwo) extends the game's graded highlights toward peak brightness. The neutral and graded references come from the game's own LUT, brightness and chroma. Method, captures and measurements: `dkb-pc-recomp/docs/renodx-vulkan.md`.

## Files

| File | Origin |
| --- | --- |
| `addon.cpp`, `shared.h`, `gameplay_highlights.slangi` | Add-on source |
| `structural_tonemap.slangi`, `structural_output.slangi` | Add-on source: the same correction as hook bodies for structurally rewritten composites |
| `structural.hpp`, `runtime_rewrite.hpp` | Add-on source: the structural rewrite, run when the game creates a composite (C++ port of `dkb-pc-recomp/tools/dkb_composite_struct.py`) |
| `dkb_tonemap_hook.frag.slang`, `dkb_output_hook.frag.slang` | Add-on source: the two slangi bodies compiled as stand-alone hooks, embedded and spliced into each rewritten composite |
| `INSTALL.txt` | The installation notes shipped in the release package |
| `gameplay_0xB537A7CA.frag.slang` | Generated locally from your own capture. Game-derived, and ignored by `.gitignore` here. |
| `structural_0x*.frag.slang` | Optional, generated locally by `dkb-pc-recomp/build/hdr-test/structural-install.cmd` for other composite permutations. Game-derived and ignored. |

## Build

A release build needs nothing from the game: the structural rewrite replaces every composite when the game creates it. Build the add-on with the documented RenoDX toolchain:

```powershell
cmake --preset clang-x64
cmake --build --preset clang-x64-release --target dkb-recomp
```

`dkb-pc-recomp/build/hdr-test/package-release.cmd` makes the distributable package. It builds without any locally generated entry, checks that none was embedded, adds `INSTALL.txt` and the RenoDX licence, and zips the result.

### Development: the gameplay reference entry (optional)

The reference correction for the gameplay composite can be built in as well, from your own capture. A build-time entry takes precedence over the rewrite for its CRC32. The GPU A/B compares the rewrite against it.

1. Dump the stock gameplay composite with RenoDX DevKit (native HDR, `hdr_shoulder=0`), as described in `docs/renodx-vulkan.md`.
2. In the DKB checkout, generate the entry shader. The tool checks the module's SHA-256 and the exact SPIRV-Cross output. It compiles private baseline and corrected checks, then writes the entry file here.

   ```powershell
   python tools/build_renodx_gameplay.py path\to\0xB537A7CA.spv
   ```

3. In this RenoDX checkout, with the documented RenoDX toolchain:

   ```powershell
   cmake --preset clang-x64
   cmake --build --preset clang-x64-release --target dkb-recomp
   ```

   Re-run the configure step after generating the entry file the first time.

4. Optional, and not needed with the structural rewrite on: build-time entries for other composite permutations (no bloom, title, reordered bindings and others). In the DKB checkout run `build\hdr-test\structural-install.cmd`. It does the following:
   - rewrites each composite final by dataflow and checks it offline;
   - renders it on the GPU against stock and against the reference entry;
   - only if every check passes, installs `structural_<CRC>.frag.slang` here and rebuilds the add-on.

   Method and validation: `dkb-pc-recomp/docs/renodx-structural.md`.

## Structural rewrite

When the game creates a pipeline whose pixel shader is a scene composite without a build-time entry, the add-on recognises its dataflow, rewrites it, and splices in the two hooks. `runtime_rewrite.hpp` registers the result as a RenoDX runtime replacement for that CRC32, which is built at its first bind like the embedded shaders. The recogniser and rewrite are the ones validated offline. The C++ port is byte-identical to the Python prototype on all 129 composites in the corpus, and `dkb-pc-recomp/build/hdr-test/structural-ab.cmd` renders each rewritten module on the GPU against stock and the build-time path.

The rewrite is fail-safe. The game's own shader stays in place when:
- a composite is not recognised (the reason code is shown in Coverage and logged as `DKB runtime: kept the game's composite …`);
- the hooks cannot be loaded (the rewrite turns itself off);
- a rewritten pipeline fails to build. After a few binds without a replacement, the add-on removes the runtime replacement (`E_PIPELINE`).

**Structural rewrite** (Coverage section, on by default) takes effect the next time the game starts.

## Install

Use the ReShade build with full add-on support for the Vulkan `dkb.exe`. Copy `renodx-dkb-recomp.addon64` into ReShade's add-on directory. In `dkb.ini` set `hdr=1`, `hdr_native=1` and `hdr_shoulder=0`. With the shoulder enabled, DKB rewrites the composite: the CRC changes and the add-on stays inactive. Don't load RenoDX DevKit at the same time.

## Settings (ReShade overlay)

| Setting | Default | Effect |
| --- | --- | --- |
| Tone Mapper | RenoDRT | Vanilla runs the game's composite unchanged. With `hdr_shoulder=0` (required), that is the game's console HDR curve. Vanilla + shoulder applies the recompilation's own highlight shoulder (`spirv_hdr_shoulder.c` at `hdr_shoulder_strength` 100) in the add-on, as the main DKB build shows it; the brightness sliders do not apply to either Vanilla. |
| Brightness Source | Game | Game follows `hdr_peak_nits` (capped by the game at 1400) and `hdr_game_nits`. Custom uses the two sliders below. |
| Peak Brightness | 1000 | Custom only. HDR10 metadata still reports the game's own peak. |
| Game Brightness | 203 | Custom only. Scales the scene. The HUD stays at `hdr_game_nits`. |
| Hue Correction | 100 | RenoDRT hue retention in extended highlights. |
| Highlight Saturation / Blowout / Flare | 50 / 0 / 0 | Standard RenoDX highlight controls. |

With the defaults, the shader computes the revised correction that was verified offline.

## Coverage logging

The add-on classifies every pixel and compute shader the game creates by structure (`coverage.hpp`, the same tests as `dkb-pc-recomp/tools/dkb_hdr_survey.py`). It logs each tone-mapping, grading or encoding shader to `ReShade.log` (`DKB coverage: …`) when it is created and again at its first draw, marked replaced, NOT REPLACED (a scene composite left as the game draws it: the only state that needs attention) or unchanged (a grade or encoder pass, which the correction does not need to replace). The overlay's Coverage section shows the same list, with how each one is replaced (`embedded` or `runtime`) or why not (a reason code such as `E_GRADE`). `coverage.tsv` carries the same two columns; a file from an earlier build is kept as `coverage-v1.tsv`. **Save unreplaced composites** writes the exact bytes of each unreplaced scene composite to `renodx-dkb-recomp-coverage\` beside the add-on. Method: `dkb-pc-recomp/docs/renodx-coverage.md`.

## Not covered yet

- All 129 composite permutations in the corpus are recognised, including the 27 with a saturation grade instead of a LUT (formerly `E_GRADE`; none of those has been drawn in play yet). The SDR path is not covered.
- Vanilla + shoulder also extends the extra-stage (ID-mask) composites, which the recompilation's own rewrite does not match; none of them has been drawn in play so far.
- A separate UI brightness setting would need the HUD draws or the final encoder replaced.
