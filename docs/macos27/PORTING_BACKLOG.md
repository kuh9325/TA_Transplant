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

_None confirmed — now verified against real data_ (Phase 2, 2026-09-17).
A stock install directory at `~/Games/OriginalData/TotalAnnihilation`
(`totala1-4.hpi`, `rev31.gp3`, `ccdata/ccmaps/ccmiss.ccx`, `BT*.CCX`,
map `.ufo`s, loose `Objects3D/`) is read **in place** via `--data-path`.
Main menu reached (palettes, ALLSOUND.TDF, cursors GAF, SIDEDATA.TDF,
MAINMENU.GUI) and a skirmish map loaded and ticked
(`--map "Great Divide 2"` — OTA, TNT, FBI/3DO/COB, audio streaming).
No P2 blocker found.

Watch items, updated after Phase 2:

- ~~HPI/TNT/GAF/3DO/COB parsers vs real data~~ — VERIFIED working
  (archives with ZLib-compressed members, mixed-case internal paths).
- ~~Audio decode paths~~ — mixer streams buffers; actual sound content
  correctness still unverified.
- Map-name lookup requires exact internal archive names
  (`maps/<name>.ota`, e.g. `"Great Divide 2"`, spaces not underscores).
- Deeper gameplay (opponents, orders, COB under load) untested — the
  Phase 2 skirmish auto-ended via correct win/draw logic after ~5 s.
- Screenshot-based visual verification pending (screen-recording
  permission blocked `screencapture`).

## P3 — prevents correct gameplay

### P3-1. float→uint16 UB in angle conversion saturates on arm64 — RESOLVED

- **Sites**: `src/rwe/sim/SimAngle.cpp:13` (`fromRadians`),
  `src/rwe/sim/SimAngle.h:41` (SimScalar→SimAngle),
  `src/rwe/cob/cob_util.cpp:41` (`toCobAngle`, used by `cobAtan`).
- **Evidence**: `rwe_test` failures — `cobAtan(-1,0)` → 0 (want 49152);
  `fromRadians(toRadians(32768))` → 0 (want 32768). Scratch binary shows
  `fcvtzu` saturation to 0 on arm64 vs `cvttss2si` truncation-wrap on x86.
- **Fix applied** (Phase 1): `std::round`→`std::llround` at the two rounding
  sites; `static_cast<int64_t>` intermediate at the truncating site. All
  conversions now go float→signed-wide-int (defined) → `uint16_t` (defined
  mod 2^16). All 88 test cases pass on arm64.
- **Residual risk** (deliberately out of scope, same UB shape):
  `SimScalar.h` `simScalarToUInt` casts float→`unsigned int`; used for
  sea-level/damage/frame-lifetime quantities that are non-negative by
  contract. No failing test evidence; revisit only if a divergence appears.

### P3-3. Intermittent freeze under sustained combat load — UNCONFIRMED CAUSE

- **Evidence** (Phase 3): one freeze in ~42 min of interactive play on
  `Great Divide 2`. User was firing EMGs with 10 FLASH tanks during a
  failed-pathfind storm (unreachable target across the divide). Window
  became fully unresponsive → force-quit; log stops mid-tick with no
  graceful-exit line; no crash report / desync dump / jetsam.
- **Structural suspects** (unproven — no stack sample yet):
  `CobExecutionContext::execute()` has no instruction bound;
  `executeThreads` re-enters on interrupt statuses without popping;
  `runCobQuery` executes `AimFrom`/`Query`/`SweetSpot` synchronously.
  A wait-less COB loop would hang a tick permanently.
- **Not proven platform-specific**: could equally be an upstream
  all-platforms defect or an arm64-divergent value feeding a script.
- **Next diagnostic**: reproduce with 10+ units mass-attacking an
  unreachable target under fire; `sample <pid>` the frozen process to
  identify the spin site, then classify.
- Per policy: no speculative patch.

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
