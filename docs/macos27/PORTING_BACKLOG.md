# RWE macOS 27 / Apple Silicon — Porting Backlog

Derived from `BASELINE_AUDIT.md` (2026-09-16, upstream `b2d8a314`).
Severity legend:

- **P0** — prevents native build
- **P1** — prevents startup
- **P2** — prevents loading original game data
- **P3** — prevents correct gameplay
- **P4** — packaging/polish
- **FUTURE** — optional modernization

Nothing here implies Metal or SDL3 migration — neither is needed to run the
game; evidence below says OpenGL-on-Metal and SDL3 already work.

---

## P0 — prevents native build

### P0-1. SDL3_mixer probes Homebrew codecs and fails on split include dirs

- **Evidence**: `decoder_vorbis.c` → brew `vorbis/codec.h` →
  `#include <ogg/ogg.h>` not found. `SDL3MIXER_VENDORED=ON` does not suppress
  system probing; `FindVorbis` adds libvorbis' keg include dir but not
  libogg's. Fails only on machines where brew `libvorbis` is installed.
- **Status**: CONFIRMED on this machine; workaround verified
  (`-DSDLMIXER_VORBIS_VORBISFILE=OFF`, zero source changes, keeps `vorbis_stb`).
- **Candidate resolutions** (choose later, with evidence):
  - document the flag for macOS builds, or
  - force SDL3_mixer to prefer vendored codecs on APPLE
    (e.g. `-DSDLMIXER_DEPS_SHARED=OFF` / per-codec options), or
  - treat as upstream SDL3_mixer `FindVorbis` defect (missing Ogg include dep).
- **Note**: same mechanism could theoretically trip `flac`/`mpg123`/`opus`
  system detection later; only vorbis actually broke.

### P0-2. Documented devbox path is not viable on aarch64-darwin as written

- **Evidence**: `devbox.json` requires Linux-only nixpkgs (`alsa-lib`,
  `libX11*`, `wayland`, `pipewire`, `pulseaudio`, `libdecor`, `libxcb`,
  `libxkbcommon`, …); `devbox.lock` contains a single `aarch64-darwin`
  resolution. `devbox shell` cannot satisfy these on macOS. Additionally,
  devbox requires Nix, whose installer requires `sudo` on macOS.
- **Status**: LIKELY BLOCKER (unverified locally — devbox not installed);
  the README's "MacOS should work via the devbox build" is probably stale.
- **Candidate resolutions**: platform-conditional devbox.json
  (`"packages": {"...": {"platforms": [...]}}` devbox supports this), or
  document the Homebrew dependency set (`cmake glew protobuf libpng
  pkg-config`) as the supported macOS path.

## P1 — prevents startup

_None found._ Startup probe reached the missing-data stage cleanly:
SDL window, GL 4.1 Metal core context, GLEW, ImGui all functional.

- ~~OpenGL availability~~ — Apple's deprecated-but-present OpenGL 4.1
  satisfies the 3.2-core request. NO CURRENT EVIDENCE OF PROBLEM.

## P2 — prevents loading original game data

_None confirmed._ VFS, case-insensitive lookup, and all TA parsers compile
and their unit tests pass. Data-path resolution (`~/.rwe/Data`,
`--data-path`) works; absent data produces a graceful modal error.

Watch items once real data is available:

- HPI/TNT/GAF/3DO/COB parsers read raw LE structs — fine on arm64 (LE), but
  real-data smoke tests should verify.
- Audio decode paths (WAV in HPI archives via SDL3_mixer) — untested.

## P3 — prevents correct gameplay

### P3-1. float→uint16 UB in angle conversion saturates on arm64

- **Sites**: `src/rwe/sim/SimAngle.cpp:13` (`fromRadians`),
  `src/rwe/sim/SimAngle.h:41` (SimScalar→SimAngle),
  `src/rwe/cob/cob_util.cpp:41` (`toCobAngle`, used by `cobAtan`).
- **Evidence**: `rwe_test` failures — `cobAtan(-1,0)` → 0 (want 49152);
  `fromRadians(toRadians(32768))` → 0 (want 32768). Scratch binary shows
  `fcvtzu` saturation to 0 on arm64 vs `cvttss2si` truncation-wrap on x86.
- **Impact**: negative TA angles silently become 0 — unit turret/body
  headings and COB script results wrong on arm64.
- **Fix direction (later phase, evidence-based)**: convert through a signed
  wide type before truncating (e.g. `static_cast<uint16_t>(static_cast<int32_t>
  (std::round(...)))`), preserving the x86 wrap semantics that tests encode.
  Do NOT "fix" the tests.
- **Also audit**: any other narrowing float→int casts that rely on wrap.

### P3-2. Float determinism across architectures

- **Evidence**: `SimScalar` is `float`-backed; clang/arm64 may emit FMA
  contraction where x86 builds don't → potential multiplayer desync vs
  x86 peers. No measured evidence yet.
- **Action (later)**: check effective `-ffp-contract` on both toolchains;
  if it matters, pin `-ffp-contract=off` for sim code and document.

## P4 — packaging / polish

- **P4-1. dylib surface**: brew-protobuf build links ~50 `libabsl_*` +
  `libprotobuf` + `libGLEW` + `libpng` dylibs. For distribution prefer the
  vendored static protobuf (`libs/build-protobuf.sh`, needs autotools) or
  static-link policy; also SDL3_mixer dlopens brew codec dylibs when present.
- **P4-2. `make install`/`package`** contain Windows-only paths
  (`launcher/rwe-launcher-win32-x64`, NSIS cpack generator) — will need
  macOS-conditional packaging later.
- **P4-3. No `.app` bundle / icon / signing** — binary runs from CLI fine;
  proper bundle deferred.
- **P4-4. `build/` is not in `.gitignore`** — hygiene only.
- **P4-5. Latent upstream bug**: `Grid.h:179` returns reference to temporary
  (`-Wreturn-stack-address`). Platform-independent; fix upstream first.
- **P4-6. Deprecated literal-operator warnings** (`"" _ss`/`"" _ssf`) under
  Clang 21 — cosmetic; will hard-error under a future standard. Defer.

## FUTURE — optional modernization (explicitly NOT in scope now)

- OpenGL → Metal (not needed: GL 4.1 works; revisit only if Apple removes GL)
- SDL3 → anything else (already SDL3 upstream)
- C++ standard upgrades, refactors, dead-file removal
  (`cmake/Modules/FindSDL2*`, `appveyor.*`)
- Launcher (Electron) macOS packaging
- macOS CI coverage (GH `macos-*` runners are arm64) — worth adding once the
  build is green by default; deliberately not added in Phase 0.
