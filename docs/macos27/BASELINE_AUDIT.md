# RWE macOS 27 (Apple Silicon) — Phase 0 Baseline Audit

## 1. Audit date

2026-09-16 (all times PDT).

## 2. Exact upstream commit

- Repository: `https://github.com/MHeasell/rwe` (remote `origin`, fetch+push)
- Branch: `master`
- HEAD: `b2d8a314b14142d8873400160768777532b1dcab`
  - `b2d8a314 2026-03-25 Reset CMAKE_POSITION_INDEPENDENT_CODE after SDL3_mixer`
- `git describe --dirty=-d` (what CMake parses): `v0.1.0-1950-gb2d8a314` — matches the `^v#.#.#` requirement
- `git describe --tags`: `msvc-libs-v6-41-gb2d8a314` (lightweight tag; irrelevant to CMake)
- Tags present: `v0.1.0` (annotated), `msvc-libs-v6` (lightweight)
- Worktree at audit start: **clean**; local `master` == `origin/master` (verified after `git fetch origin --tags`)
- Submodule SHAs (all initialized, none modified — `git submodule status` showed no `+`/`-`/`U` prefixes):

| Submodule | SHA | Description |
|---|---|---|
| libs/asio | `bd500f0a` | asio-1-38-0-18-gbd500f0a0 |
| libs/catch2 | `29c9844f` | v3.13.0 |
| libs/imgui | `6ded5230` | v1.62-5203-g6ded5230d |
| libs/json | `55f93686` | v3.12.0 |
| libs/protobuf | `f0dc78d7` | v21.12 |
| libs/protobuf/third_party/benchmark | `5b7683f4` | v1.2.0 |
| libs/protobuf/third_party/googletest | `5ec7f0c4` | release-1.8.0-1696-g5ec7f0c4 |
| libs/rapidcheck | `b96a4e62` | remotes/origin/dev-183-gb96a4e6 |
| libs/rapidcheck/ext/catch | `03d122a3` | v2.0.1-403-g03d122a3 |
| libs/rapidcheck/ext/googletest | `e38e9027` | release-1.8.0-3122-ge38ef3be |
| libs/sdl3 | `683181b4` | release-3.4.2 |
| libs/sdl3_mixer | `cedfeef3` | release-3.2.0 |
| libs/sdl3_mixer/external/* | (13 codec submodules) | flac, libgme, libxmp, mpg123, ogg, opus, opusfile, tremor, vorbis, wavpack |
| libs/utfcpp | `6bbbacca` | v2.3.4-143-g6bbbacc |

## 3. Machine / OS / toolchain

| Item | Value |
|---|---|
| `uname -a` | `Darwin Musicboxui-Macmini.local 27.0.0 ... RELEASE_ARM64_T8132 arm64` |
| `uname -m` / `arch` | `arm64` / `arm64` |
| `sw_vers` | macOS 27.0, build 26A428 |
| Hardware | Mac mini (Mac16,10), Apple M4, 10 cores (4P+6E), 24 GB |
| Rosetta | **Not installed** — `arch -x86_64 /usr/bin/true` → `Bad CPU type in executable`. x86_64 cannot silently become a dependency on this machine. |
| Xcode | 27.0 (27A266a), `xcode-select -p` → `/Applications/Xcode.app/Contents/Developer` |
| clang | Apple clang 21.0.0 (clang-2100.3.34.2), target `arm64-apple-darwin27.0.0` |
| make | GNU Make 3.81 (`/usr/bin/make`) |
| git | 2.54.0 (Apple Git-157) |
| cmake | **not preinstalled** → installed via Homebrew: 4.4.3 (`arm64_golden_gate` bottle) |
| devbox | **not installed** (and infeasible — see §11) |
| nix / ninja | not installed |
| Homebrew | 7.0.2, prefix `/opt/homebrew` (native arm64) |
| Other build inputs | `pkgconf` 3.0.7 and `libpng` 1.6.58 were already installed via Homebrew; `glew` 2.3.1, `protobuf` 36.1 (+`abseil` 20260817.0) installed during this audit; `zlib` resolved from the Xcode SDK (`libz.tbd` 1.2.12) |
| node/npm | v26.5.0 (Homebrew) — launcher not exercised this phase |

Active shell/build environment is genuinely `arm64` end to end.

## 4. Build procedure

Documented upstream path (`devbox shell` → cmake/make) could not be followed literally
(see §11 — devbox requires Nix, Nix install requires `sudo`, and `devbox.json` pins
Linux-only packages). Closest feasible equivalent used:

```sh
brew install cmake glew protobuf          # libpng, pkgconf already present; zlib via Xcode SDK
git submodule update --init --recursive   # already clean, verified no drift
mkdir build && cd build
cmake .. -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug
make -j$(sysctl -n hw.ncpu)
```

Second configure (diagnostic flag only, no source change — see §5):

```sh
cmake .. -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug -DSDLMIXER_VORBIS_VORBISFILE=OFF
make -j$(sysctl -n hw.ncpu)
```

## 5. Build result

### First attempt (documented procedure): FAIL

- CMake configure: **success** (44.1 s). Detected AppleClang 21.0.0, `ARM64` target CPU,
  protobuf 7.36.1 (`/opt/homebrew/lib/libprotobuf.dylib`), GLEW via
  `glew-config.cmake`, ZLIB from Xcode SDK, PNG 1.6.58. SDL3 configured with Cocoa
  video, coreaudio, Metal+OpenGL render drivers — all native Darwin backends.
- Compile failure at 5–6%:

  ```
  libs/sdl3_mixer/src/decoder_vorbis.c:34
  → /opt/homebrew/Cellar/libvorbis/1.3.7/include/vorbis/vorbisfile.h:26
  → /opt/homebrew/Cellar/libvorbis/1.3.7/include/vorbis/codec.h:25:10:
  fatal error: 'ogg/ogg.h' file not found
  ```

- Complete causal chain:
  1. RWE sets `SDL3MIXER_VENDORED=ON`, but that option only controls vendored
     *fallback* — SDL3_mixer 3.2.0 still probes the system for codec libraries.
  2. CMake's default search prefixes on Apple Silicon include `/opt/homebrew`,
     so SDL3_mixer "found" Homebrew `libvorbis 1.3.7`
     (`Using system vorbisfile`, `Dynamic vorbisfile: libvorbisfile.3.dylib`).
  3. Its `FindVorbis` adds only the libvorbis Cellar include dir
     (`/opt/homebrew/Cellar/libvorbis/1.3.7/include`), which contains `vorbis/`
     but not `ogg/` — libogg is a separate Homebrew keg.
  4. `vorbis/codec.h` does `#include <ogg/ogg.h>` → not found → hard error.
  5. `libogg 1.3.6` **is** installed (`/opt/homebrew/include/ogg/ogg.h`); its
     include dir was simply never added.
- Why upstream CI/devbox don't hit this: no system codec dev packages are visible
  there, so the vendored `external/{ogg,vorbis}` copies are used instead.
- Classification: **environment-dependent build failure**, not an engine-source
  defect. Fixable via cache flags or environment, zero source changes required.

### Second attempt (`-DSDLMIXER_VORBIS_VORBISFILE=OFF`): PASS

- Reproduces the CI/devbox condition (system vorbisfile invisible); `vorbis_stb`
  backend remains enabled so vorbis decoding is still covered by a vendored path.
- `make -j10`: **all targets built** — `librwe.a` (142 MB, Debug), `rwe`,
  `rwe_bridge`, `rwe_test`, plus `hpi_test vfs_test gaf_test pcx_test tnt_test
  3do_test cob_test fnt_test texture_test`.
- 210 warnings, none fatal. Categories:
  - `-Wdeprecated-literal-operator` — `operator"" _ss` / `_ssf` in
    `src/rwe/sim/SimScalar.h` (Clang 21 deprecation; cosmetic)
  - `-Wmissing-field-initializers`, unused params/vars/private fields (routine)
  - `src/rwe/grid/Grid.h:179:20: warning: returning reference to local temporary
    object [-Wreturn-stack-address]` — **latent upstream bug** (`Grid::get` const
    overload), platform-independent. Recorded, not fixed.
- Also observed during configure: SDL3_mixer additionally picked up *system*
  Homebrew `libflac`, `mpg123`, `opus` (all via runtime `dlopen` — "Dynamic
  libflac/mpg123", so they are soft deps). Vorbis was the only one whose headers
  broke compilation.

## 6. Test result

Command: `./build/rwe_test` (Catch2 v3.13.0 + RapidCheck). Duration ~0.83 s.

```
test cases:   88 |   86 passed | 2 failed
assertions: 1159 | 1156 passed | 3 failed
```

Failing tests — **both explained by one confirmed arm64-vs-x86 codegen
difference**:

1. `SimAngle.test.cpp` — prop `fromRadians inverts toRadians`
   (`SimAngle.test.cpp:53`): falsifiable at `a = 32768`; `fromRadians(toRadians
   (32768))` returned `0`, expected `32768`.
2. `cob_util.test.cpp` — `REQUIRE(cobAtan(-1, 0) == 49152)` (`:51`) returned `0`;
   plus prop `toCobAngle inverts toRadians` (`:69`) fails at `32768` → `0`.

Root cause (verified, see §12 evidence):

- `RadiansAngle::fromUnwrappedAngle` wraps to `[-π, π)`, so `toRadians(32768)`
  yields `-π`; `fromRadians` then computes `std::round(-π/π·32768) = -32768.0f`
  and does `static_cast<uint16_t>(-32768.0f)` (`src/rwe/sim/SimAngle.cpp:13`).
  Same pattern at `src/rwe/cob/cob_util.cpp:41` (`toCobAngle`) and
  `src/rwe/sim/SimAngle.h:41`.
- Float→`uint16_t` conversion of a negative value is **out-of-range UB**:
  - x86-64: `cvttss2si` produces signed `-32768`, truncated to `uint16` →
    **wraps to 32768** (what upstream tests assume).
  - arm64: Clang lowers to `fcvtzu` (unsigned saturating) → **0**.
- Verified with a scratch program (Apple clang 21, `-O0`): `uint16(-32768.0f) = 0`,
  `uint16(-16384.0f) = 0`, `int32(-32768.0f) = -32768`; disassembly shows
  `fcvtzu`/`fcvtzs` as expected. At `-O2` the same UB produces yet another value
  (`4126474624`), confirming the result is optimizer-sensitive.
- Impact is real gameplay behavior, not just tests: `cobAtan` feeds COB unit
  scripts; `fromRadians`/`toCobAngle` underlie all angle quantization. Negative
  (i.e. "left-of-forward") angles silently become 0 on arm64.
- These same tests presumably pass on upstream x86 CI; they are *portable-UB*
  failures, not logic errors that any engine code change should "fix" in tests.
  Per phase rules: tests were not modified.

No crashes; no hangs; suite completes in <1 s.

## 7. Native architecture verification

`file`/`lipo -info` on every produced artifact:

- `rwe`, `rwe_test`, `rwe_bridge`, `hpi_test`, `vfs_test`, `gaf_test`,
  `pcx_test`, `tnt_test`, `3do_test`, `cob_test`, `fnt_test`, `texture_test`,
  `librwe.a` — all **Mach-O arm64** (non-fat). No x86_64, no universal.

`otool -L rwe` dynamic deps: system frameworks only (OpenGL, Cocoa, CoreMedia,
CoreVideo, IOKit, ForceFeedback, Carbon, CoreAudio, AudioToolbox, AVFoundation,
Foundation, GameController, Metal, QuartzCore, CoreHaptics, AppKit,
CoreFoundation, CoreGraphics, CoreServices, UniformTypeIdentifiers), `libz.1`,
`libc++`, `libSystem`, `libobjc`, plus arm64 Homebrew dylibs: `libGLEW.2.3`,
`libprotobuf.36.1.0`, `libpng16.16` (rwe_bridge), and ~50 `libabsl_*` dylibs.
**Zero x86_64 dependencies** — nothing could load under Rosetta even if present
(it isn't).

The produced `rwe` binary is **genuinely native Apple Silicon**.

Packaging note: the brew-protobuf route produces a large dylib surface
(`libprotobuf` + ~50 `libabsl_*` + `libGLEW` + `libpng`). The vendored
`libs/build-protobuf.sh` path (static `libs/_protobuf-install`) is what CI
uses and would shrink this; not exercised here (requires autoconf/automake/
libtool and runs `git clean -fdx` inside the protobuf submodule).

## 8. Runtime / startup result

`./build/rwe` (no args, no TA assets): **PASS — reaches expected missing-data state**.

Log evidence (`~/.rwe/rwe.log`):

```
[info] Robot War Engine v0.1.0-1950-gb2d8a314-Debug
[info] Initializing SDL
[info] Initializing OpenGL context
[info] Requesting OpenGL version 3.2, core profile
[info] OpenGL version: 4.1 Metal - 91.7
[info] OpenGL vendor: Apple
[info] OpenGL renderer: Apple M4
[info] OpenGL shading language version: 4.10
[info] Initializing Dear ImGui
[info] Initializing virtual file system
[critical] filesystem error: in directory_iterator::directory_iterator(...):
           No such file or directory ["/Users/junhokim/.rwe/Data"]
```

Confirmed:

- process startup: OK
- SDL window creation (Cocoa): OK
- OpenGL 3.2 Core context request → **granted 4.1 (Apple's GL-on-Metal)**:
  OK, no fallback needed
- GLEW 2.3.1 init on a core context (`glewExperimental = GL_TRUE` already in
  code): OK — no error thrown
- ImGui (sdl3+opengl3 backends, `#version 150`): OK
- Missing-data behavior: `findPathCaseInsensitive` /
  `getFileNames` iterate `~/.rwe/Data` during VFS setup;
  `fs::directory_iterator` on the nonexistent dir throws
  `std::filesystem::filesystem_error`, which propagates to `main`'s outer
  catch → `LOG_CRITICAL` + `SDL_ShowSimpleMessageBox` modal error dialog.
  The process then blocks on the modal dialog — i.e. it reports the missing
  data gracefully rather than crashing (the dialog is why it stayed alive;
  killed deliberately after verifying state).
- Note: the engine fails at directory *iteration* before reaching the intended
  `"Couldn't find palette"` check — same on Linux; not macOS-specific.
- Side effect: creates `~/.rwe/` for `rwe.log` (by design; `--data-path` or
  populating `~/.rwe/Data` is the documented next step — not performed here).

## 9. Platform dependency findings

| Area | Evidence | Verdict |
|---|---|---|
| OpenGL context | requests 3.2 Core fwd-compat, fallback 3.0 Compat (`main.cpp:182-190`); got 4.1 Metal core | NO CURRENT EVIDENCE OF PROBLEM |
| GLSL | all 18 shaders `#version 150` (GL 3.2 era); ImGui `#version 150` | NO CURRENT EVIDENCE OF PROBLEM (≤4.1 core supported) |
| GLEW | 2.3.1, `glewExperimental` already set, init OK on core profile | NO CURRENT EVIDENCE OF PROBLEM |
| SDL | SDL3 3.4.2 vendored+static, native Cocoa/coreaudio/Metal backends | NO CURRENT EVIDENCE OF PROBLEM |
| SDL3_mixer | system-codec probing broke on Homebrew libvorbis (§5) | CONFIRMED BLOCKER (env-dependent, workaround = cache flag) |
| Filesystem paths | `std::filesystem` everywhere; `~/.rwe/Data` via `RWE_PLATFORM_LINUX` branch works on macOS | NO CURRENT EVIDENCE OF PROBLEM |
| Case sensitivity | `findPathCaseInsensitive` exists; APFS default is case-insensitive anyway | NO CURRENT EVIDENCE OF PROBLEM |
| Endianness | `readRaw<T>` struct reads assume little-endian host; arm64 is LE | NO CURRENT EVIDENCE OF PROBLEM (would break only on BE) |
| `#pragma pack(1)` structs | hpi/gaf/tnt/pcx/3do/cob headers + ColorPalette + GraphicsContext | NO CURRENT EVIDENCE OF PROBLEM (arm64 tolerates unaligned; parsers' unit tests pass) |
| Integer sizes | LP64 same as Linux; no `sizeof(long)` assumptions found | NO CURRENT EVIDENCE OF PROBLEM |
| Pointer sizes | none found | NO CURRENT EVIDENCE OF PROBLEM |
| Timers | `std::chrono::steady_clock`, `SDL_GetTicks` | NO CURRENT EVIDENCE OF PROBLEM |
| Threading | `std::thread` + asio `io_context` | NO CURRENT EVIDENCE OF PROBLEM |
| Sockets | standalone asio, UDP over `asio::ip::udp::v6()` dual-stack | POTENTIAL ISSUE (untested; macOS v6 sockets default dual-stack so likely fine) |
| Compiler extensions | none found (`__attribute__`, `__declspec`, `__builtin_` absent in `src/`) | NO CURRENT EVIDENCE OF PROBLEM |
| x86 intrinsics | none in `src/` (SDL has its own arch dispatch; `SDL_ARMNEON=ON`) | NO CURRENT EVIDENCE OF PROBLEM |
| Deprecated Apple APIs | OpenGL.framework itself is deprecated (still functional in macOS 27); `Carbon`/`ForceFeedback` pulled in by SDL | POTENTIAL ISSUE (future removal risk, not today) |
| float→uint16 UB in angle conversions | `SimAngle.cpp:13`, `SimAngle.h:41`, `cob_util.cpp:41` — saturates to 0 on arm64 vs wraps on x86 | **CONFIRMED BLOCKER for correct gameplay** (2 failing tests; mechanism verified) |
| Float determinism (sim) | `SimScalar` is `OpaqueField<float>`; clang/arm64 may contract `a*b+c`→FMA unlike x86 codegen → cross-platform desync risk | POTENTIAL ISSUE (no evidence yet; would only matter vs x86 peers) |
| protobuf version | brew 36.1 vs vendored pin v21.12; proto2 wire format stable, builds+links OK | POTENTIAL ISSUE (deviation from upstream pin) |
| Runtime codec dlopen | SDL3_mixer dlopens brew `libFLAC`/`libmpg123` when found; graceful fallback if absent | POTENTIAL ISSUE (packaging) |
| `Grid.h` returning ref to temporary | `-Wreturn-stack-address` at `Grid.h:179` | POTENTIAL ISSUE (latent upstream bug, all platforms) |
| `main` full-screen path | `getClosestDisplayMode` etc. untested | NO CURRENT EVIDENCE OF PROBLEM (untested) |
| `install(DIRECTORY launcher/rwe-launcher-win32-x64/)` | Windows-only path in install rule | POTENTIAL ISSUE (only if `make install`/`package` run on macOS) |

## 10. macOS blockers (summary — see PORTING_BACKLOG.md for priorities)

1. **[Confirmed, env-dependent] SDL3_mixer system-vorbisfile detection** breaks
   the *documented* build on machines with Homebrew libvorbis. Workaround:
   `-DSDLMIXER_VORBIS_VORBISFILE=OFF` (or hide `/opt/homebrew` from CMake).
2. **[Confirmed, arm64] float→uint16 UB** in angle conversions — wrong angles
   on Apple Silicon; 2 test failures. The single most important porting defect.
3. **[Likely] documented devbox path is broken on aarch64-darwin** — devbox.json
   requires Linux-only nixpkgs (alsa-lib, wayland, pipewire, libX11…);
   devbox.lock contains exactly one `aarch64-darwin` resolution. Unverified
   locally because devbox/Nix install requires `sudo`.

## 11. Documentation/code discrepancies (recorded, not resolved)

| Document says | Reality (verified) |
|---|---|
| CLAUDE.md: "C++17 core engine" | `CMAKE_CXX_STANDARD 20` / `/std:c++20` — **C++20** |
| CLAUDE.md: SDL2 ("migrate SDL2 to SDL3" as future work); README MSYS2/Ubuntu sections list `SDL2*` packages | Upstream **already migrated to SDL3 3.4.2 + SDL3_mixer 3.2.0** (vendored submodules, `MIX_*` API, `imgui_impl_sdl3`); the SDL2 package lists are stale |
| CLAUDE.md: "fixed-point math types (SimScalar, SimVector, SimAngle) for cross-platform determinism" | `SimScalar = OpaqueField<float>` — **float**, not fixed-point; determinism claim is weaker than stated |
| CLAUDE.md: "first time setup … `libs/build-protobuf.sh`" | Only needed when no system protobuf; CMake prefers `libs/_protobuf-install` if present, else `find_package(Protobuf)` |
| CLAUDE.md: "OpenGL 3.0+" | Requests **3.2 Core** (fallback 3.0 Compat); shaders are GLSL `#version 150` |
| CLAUDE.md CI: "gcc-12, clang-15, MSVC 2022" | Actual CI: **gcc-14, clang-18** (ubuntu-24.04), **MSVC VS 2026** (`windows-2025-vs2026`), MinGW64 (windows-2025) |
| README: "MacOS should work via the devbox build" | `devbox.json` contains Linux-only packages that cannot resolve on `aarch64-darwin`; `devbox.lock` has 1 aarch64-darwin entry → the devbox path almost certainly does not work unmodified on Apple Silicon |
| `cmake/Modules/FindSDL2*.cmake` | Dead files — no `find_package(SDL2)` remains |
| `appveyor.yml`/`appveyor.bash`/`.bat` + README AppVeyor badge | Legacy CI files still present alongside GitHub Actions |
| CMakeLists `install(DIRECTORY launcher/rwe-launcher-win32-x64/)` | Hardcoded Windows artifact path in cross-platform `install()` |

## 12. Evidence — exact commands used

```sh
# Step 1 — baseline
git status --short                     # clean
git branch --show-current              # master
git rev-parse HEAD                     # b2d8a314b14142d8873400160768777532b1dcab
git remote -v                          # https://github.com/MHeasell/rwe.git
git fetch origin --tags
git rev-parse origin/master            # == HEAD
git submodule status --recursive
git describe --dirty=-d                # v0.1.0-1950-gb2d8a314
git cat-file -t v0.1.0                 # tag (annotated)

# Step 2 — machine
uname -a; uname -m; arch; sw_vers; system_profiler SPHardwareDataType
xcodebuild -version                    # Xcode 27.0 (27A266a)
clang --version                        # Apple clang 21.0.0, arm64-apple-darwin27.0.0
cmake --version                        # (absent → brew cmake 4.4.3)
make --version                         # GNU Make 3.81
git --version                          # 2.54.0
devbox version                         # unavailable
arch -x86_64 /usr/bin/true             # Bad CPU type in executable (no Rosetta)

# Step 4 — build
brew install cmake glew protobuf
mkdir build && cd build
cmake .. -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug          # configure OK
make -j$(sysctl -n hw.ncpu)                                     # FAIL: ogg/ogg.h
cmake .. -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug \
    -DSDLMIXER_VORBIS_VORBISFILE=OFF                            # reconfigure
make -j$(sysctl -n hw.ncpu)                                     # PASS (all targets)

# Step 5 — tests
./build/rwe_test                       # 88 cases / 86 pass / 2 fail (~0.8s)

# Step 6 — binaries
file build/{rwe,rwe_test,rwe_bridge,...}   # all Mach-O arm64
lipo -info build/rwe                        # arm64
otool -L build/rwe                          # frameworks + arm64 brew dylibs only

# Step 7 — startup
./build/rwe            # from build/; window+GL4.1-Metal+GLEW+ImGui OK;
                       # critical: filesystem_error on ~/.rwe/Data (modal dialog)
cat ~/.rwe/rwe.log

# UB mechanism verification (scratch dir, repo untouched)
clang++ -O0 -std=c++20 /tmp/rwe_probe/conv.cpp -o conv_O0 && ./conv_O0
#   uint16(-32768.0f) = 0      (fcvtzu — saturates)
#   int32(-32768.0f)  = -32768 (fcvtzs — signed ok)
```

## 13. Untested / out of scope this phase

- Launcher (Electron) — `node`/`npm` present but `npm ci` not run
- `libs/build-protobuf.sh` vendored protobuf build (needs autotools)
- Gameplay with actual TA assets (per rules — not sought, not copied)
- Fullscreen mode, audio output, networking, `rwe_bridge` IPC
- `make install` / `make package`

---

# Phase 1 addendum — arm64 angle-conversion fix (2026-09-16)

## Root cause (confirmed, mechanism verified)

`static_cast<uint16_t>(negative_float)` is out-of-range float→unsigned
conversion (UB). x86-64 lowers it to `cvttss2si` (signed 32-bit convert) +
truncation → modular 16-bit wrap; arm64 lowers it to `fcvtzu` (unsigned
saturating convert) → 0. The engine relied on the x86 artifact.

## Files changed

| File | Site | Change |
|---|---|---|
| `src/rwe/sim/SimAngle.cpp:13` | `fromRadians` | `std::round` → `std::llround` |
| `src/rwe/cob/cob_util.cpp:41` | `toCobAngle` | `std::round` → `std::llround` |
| `src/rwe/sim/SimAngle.h:41` | `simAngleFromSimScalar` | `static_cast<int64_t>` intermediate |
| `src/rwe/sim/SimAngle.test.cpp` | `toRadians` case | +SECTION "converts radians to SimAngle" (4 deterministic REQUIREs locking negative→wrap: −π→32768, −π/2→49152) |

## Chosen defined semantics

float → signed `long long`/`int64_t` (defined; inputs are provably bounded to
±32768 for the rounding sites, asserted ≥0 for the truncating site) →
`uint16_t` (defined modular reduction mod 2^16, C++20 [conv.integral]).

- Rounding sites: `std::llround` preserves the exact round-half-away-from-zero
  behavior of `std::round`; the integer result then wraps mod 2^16.
- Truncating site: `static_cast<int64_t>` preserves truncation-toward-zero;
  defined for all |value| < 2^63 (covers every meaningful input; x86 results
  beyond that were already meaningless UB artifacts).
- Produces bit-identical results to historical x86 behavior on every input
  where x86 was meaningful: 0→0, +π/2→16384, −π/2→49152, ±π→32768,
  and all in-between values wrap mod 2^16 on both architectures.
- No API/type/representation changes; no shared helper added (none exists —
  `rwe::wrap(int,int)` only wraps integers post-conversion).

## Test results

- Before: 88 cases / **86 pass / 2 fail** (`cob_util` deterministic +
  `SimAngle` prop, seed `13223679111204649236`).
- After: **88/88 pass, 1164 assertions** (~0.14 s). Focused reruns with the
  previously-failing seed pass. `git diff --check` clean; no new warnings;
  `file`/`lipo` confirm `rwe`/`rwe_test` remain Mach-O arm64.
- Startup probe rerun: unchanged — SDL window + GL 4.1 Metal core + GLEW +
  ImGui OK, stops at expected `~/.rwe/Data` missing-data modal.

## Remaining uncertainty

- `simAngleFromSimScalar` divergence surface is assert-guarded
  (`s.value >= 0`) and was only UB for negative/out-of-range floats; the
  fix hardens it identically, but no runtime divergence was ever observed
  from that site.
- `simScalarToUInt` retains the same UB shape (float→`unsigned int`) for
  negative inputs; its call sites pass non-negative quantities — flagged
  in the backlog as residual risk, not touched per phase scope.
- Cross-platform equivalence is established by static reasoning +
  instruction-level evidence, not by executing x86 builds.

---

# Phase 2 addendum — real-data runtime smoke test (2026-09-17)

Scope: determine how far unmodified RWE (Phase 1 state, HEAD `25c5a728`)
gets with real Total Annihilation data on arm64 macOS. No engine source
was changed this phase — **no runtime blocker was found**.

## Game data

- Path used: `~/Games/OriginalData/TotalAnnihilation` — an existing,
  legally obtained Windows-era TA install directory. Inspected read-only;
  nothing copied, renamed, modified, or committed.
- Layout: stock install-dir contents — `totala1-4.hpi`, `rev31.gp3`,
  `revision.gpf`, `ccdata.ccx`/`ccmaps.ccx`/`ccmiss.ccx`, `BTDATA.CCX`/
  `BTMAPS.CCX`, `newunit.ccx`, `tactics1-8.hpi`, map packages
  (`Metal_Heck_2.UFO`, `great divide 2.ufo`, `cdmaps.ufo`), loose
  `Objects3D/`, plus Windows executables/DLLs (ignored by RWE).
- RWE reads it **in place**: `--data-path <dir>` mounts the directory and
  scans for `.{hpi,ufo,ccx,gpf,gp3}` (case-insensitive extension match);
  loose files take precedence, `.gp3` archives take precedence over others.
  Case-insensitive internal path lookup (`findPathCaseInsensitive`)
  handles the mixed-case archive contents; APFS default case-insensitivity
  makes loose-file access additionally forgiving.

## Commands used

```sh
# Stage J probe — main menu
./build/rwe --data-path "$HOME/Games/OriginalData/TotalAnnihilation"

# Stage K probe — direct skirmish (bypasses menu UI)
./build/rwe --data-path "$HOME/Games/OriginalData/TotalAnnihilation" \
    --map "Great Divide 2" --player "Test;Human;ARM;0"
```

Note: `--map` resolves `maps/<name>.ota` inside the VFS, so the argument
must match the archive's internal filename (e.g. `"Great Divide 2"` —
spaces, not underscores). An earlier probe with `--map Metal_Heck_2`
failed with `Failed to read OTA file` purely because the archive's
internal name is `Metal Heck 2.ota`; input error, not an engine defect.
Archive contents verified read-only with `./build/hpi_test <archive>`.

## Results

**Main-menu run: Stage J reached, no errors.**

```
Initializing SDL / OpenGL context (4.1 Metal - 91.7, Apple M4, GLSL 4.10)
Initializing virtual file system
Loading palette                      (PALETTE.PAL from HPI)
Loading GUI palette                  (GUIPAL.GUI)
Loading global sound definitions     (ALLSOUND.TDF)
Loading cursors                      (anims/cursors.GAF)
Loading side data                    (SIDEDATA.TDF)
Launching into the main menu         (MAINMENU.GUI loaded + built)
Entering main loop
Finished main loop, exiting          (graceful, external quit)
```

**Skirmish run ("Great Divide 2"): Stage K reached.**

```
Launching into game on map: Great Divide 2
Opening listen socket on port 1337
we are loading  (≈1.9 s — OTA + TNT + FBI/3DO/COB unit load)
we are ready    (game scene entered, sim ticking)
Buffer levels (real/target) 14/14 ...  (SDL3_mixer actively streaming)
Finished main loop, exiting   (≈5.2 s after "ready")
```

The ~5 s auto-exit is **correct engine behavior, not a crash**:
`GameScene::update` calls `simulation.computeWinStatus()` each tick;
`WinStatusWon`/`WinStatusDraw` schedules `requestExit()` after
`SceneTime(5*30)` ticks (GameScene.cpp:2366-2377). A 1-player game with
no opponents satisfies a terminal condition immediately. Timeline matches
exactly (ready 00:11:05.95 → exit 00:11:11.14).

Main-menu runs also exited cleanly after ~7–23 s via `SDL_EVENT_QUIT`
(window close / Cmd+Q — the only menu exit paths; no self-quit exists in
menu code). System logs show orderly SDL/CoreAudio teardown, no crash
report.

## Runtime-stage classification

| Stage | Result |
|---|---|
| A SDL/window | PASS (Cocoa) |
| B OpenGL/renderer | PASS (4.1 Metal core) |
| C game-data path accepted | PASS |
| D archive/VFS discovery | PASS (all archive types, mixed case) |
| E palette/core assets | PASS |
| F config/TDF parsing | PASS (ALLSOUND.TDF, SIDEDATA.TDF, OTA) |
| G graphics assets | PASS (cursors GAF, GUI, TNT terrain, 3DO) |
| H audio | PASS (SDL3_mixer streaming; buffer telemetry active) |
| I UI/menu construction | PASS (MAINMENU.GUI) |
| J main menu | **PASS** |
| K skirmish/game init | **PASS** (map load + sim ticks + audio) |

**Furthest stage: K — no runtime blocker identified.**

## First-blocker analysis

None. The only failure observed (`Failed to read OTA file` on
`Metal_Heck_2`) was a wrong map-name argument, resolved by reading the
archive listing. No patch was applied or justified this phase.

## Caveats / unresolved risks

- Visual verification is inferred from logs + scene entry, not
  screenshots — `screencapture` failed with "could not create image from
  display" (screen-recording permission), so text/texture corruption is
  unconfirmed. A human looked at the window during runs without reporting
  artifacts, but this is not rigorously verified.
- The skirmish ran ~5 s with no opponents before auto-exit; deeper
  gameplay (real opponents, orders, combat, COB scripts under load) is
  untested.
- Input interaction was not exercised (probes ran unattended; exits came
  from window close and the win/draw timer).
- Networking was initialized (UDP socket on 1337, asio) but no peer
  traffic was tested.
- Full audio *content* correctness unverified — mixer streamed buffers
  (internal telemetry) but actual sounds were not audited.
- `Float determinism` (P3-2) and `simScalarToUInt` residual risk remain
  as documented; nothing new observed.

## Validation

- `./build/rwe_test`: **88/88 cases, 1164 assertions — unchanged**
  (Phase 1 result intact; no source changed).
- `git diff --check`: clean. `file build/rwe`: Mach-O arm64.
- Worktree note: `e7734452 chore: ignore local build directory` (user
  commit) added `build/` to `.gitignore` between phases; baseline
  otherwise clean.

---

# Phase 3 addendum — interactive skirmish verification (2026-09-17)

Scope: verify the interactive game loop with two opposing participants
(defeats the Phase 2 ~5 s no-opponent auto-exit). HEAD `6b1a3add`, no
source changes this phase.

## Setup (existing CLI mechanisms only)

```sh
./build/rwe --data-path "$HOME/Games/OriginalData/TotalAnnihilation" \
    --map "Great Divide 2" --width 1280 --height 800 \
    --player "Player;Human;ARM;0" \
    --player "Enemy;Computer;CORE;1"
```

- `--player <name;type;side;color>` (repeatable, ≤10, `"empty"` for open
  slots). Types: `Human`, `Computer`, `Network,host:port`; sides `ARM`/
  `CORE`; color 0–9. Metal/energy fixed at 1000/1000.
- `Computer` exists upstream but is a **passive** opponent — its command
  queue is fed empty commands (`GameScene.cpp:2015`, `TODO: implement
  computer AI logic`). Sufficient to keep `computeWinStatus()` at
  `WinStatusUndecided` (≥2 alive players, GameSimulation.cpp:729-757).
- Map: `Great Divide 2` — 6×8 Lush, `numplayers=2, 4`, schema 0 defines
  StartPos1–4. Player index i → `StartPos(i+1)`; each side spawns
  `sideData.commander` (LoadingScene.cpp:295-314).
- Note: default `--interface-mode left-click` (authentic TA) — right-click
  does NOT issue orders, it clears selection/drags minimap
  (GameScene.cpp:1514-1525). Orders go via orders-panel buttons/hotkeys
  then left-click. User-reported "orders didn't work" via right-click is
  **expected upstream behavior**, not a defect.

## Results — two runs

### Run 1: ~13.5 min, ended in a freeze (force-quit)

- Human-confirmed: terrain/textures/units/HUD/cursor all rendered
  correctly; audio normal; selection worked; user built a vehicle plant,
  produced **10 FLASH tanks**, and was firing EMGs at the enemy when the
  window became fully unresponsive → user force-quit.
- Log signature: heavy `Failed to find goal, visited 1000 vertices`
  storm (2,558 failed A\* searches — land units repathing toward an
  unreachable target across the water divide); main loop stopped writing
  at 00:37:46 mid-storm; no `Finished main loop` line; `proc_exit` ~21 s
  later; no crash report, no desync dump, no jetsam event.
- Interpretation: the main thread hung inside a single `update()`/tick —
  a starved loop would have spammed `Blocked waiting` (only 12 total),
  and the logger flushes per line so the exit line can't have been lost.

### Run 2: ~28 min, clean graceful exit

- Same command; user played through build-up + combat pathing
  (126 failed / 44+ found pathfinds), audio streaming throughout,
  window closed normally — `Finished main loop, exiting` logged.
- **Freeze did not reproduce.**

## First real runtime finding — intermittent freeze under combat load

- Observed once in ~42 min of gameplay; correlated with sustained combat
  + repath storm, not with elapsed time.
- Structural suspects identified (unproven without a stack sample):
  - `CobExecutionContext::execute()` — `while (!callStack.empty())` with
    **no instruction bound**; a script loop without a yielding opcode
    (wait/sleep/block/query) spins forever inside a tick.
  - `runUnitCobScripts`/`executeThreads` — interrupt statuses
    (PieceCommand/Query/SetQuery) return without popping the thread; a
    script emitting piece commands in a wait-less loop alternates
    executeThreads↔handlePieceCommand indefinitely.
  - `runCobQuery` (`AimFrom`/`Query`/`SweetSpot`) runs scripts
    **synchronously** per weapon-aiming tick and must reach
    FinishedStatus.
- A\* itself is bounded (1,000 pops; 4,000-closed-vertex/tick budget in
  PathFindingService) — the storm alone is not the hang.
- Classification: **unproven** — could be a pre-existing upstream defect
  (all platforms) or an arm64-divergent value feeding a script loop.
  No stack sample captured; watchdog armed on run 2 did not trigger.
- Per policy: **no speculative patch.** Reproduction + `sample <pid>` of
  the frozen process is the required next diagnostic.

## Verified this phase

| Claim | Evidence |
|---|---|
| Sustained sim (28 min vs ~5 s Phase 2) | log timestamps + user play |
| 2-player game prevents win-condition exit | code (computeWinStatus) + observed |
| Tick progression, unit orders, pathfinding | log: order→A\* events, repath cycles |
| Unit production, building, combat initiation | human: plant + 10 Flashes + EMG fire |
| Terrain/units/UI/cursor render correctly | **human-confirmed** |
| Selection works | **human-confirmed** |
| Audio audible and normal | **human-confirmed** + buffer telemetry |
| Camera/scroll, box-select, keyboard | partially exercised; not itemized |
| COB animation/scripts | ran (aim scripts execute per weapon tick); visual COB anim not itemized by observer |
| Graceful exit path | run 2 clean `Finished main loop` |

## Validation

- `rwe_test`: **88/88, 1164 assertions — unchanged** (no source changes).
- `git diff --check` clean; `rwe`/`rwe_test` Mach-O arm64.
- No TA assets touched; game data read in place.
