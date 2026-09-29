# TA Transplant

A preservation-focused **Apple Silicon / modern macOS compatibility fork of Robot War Engine (RWE)**, aimed at reproducing the original **Total Annihilation** experience with user-supplied retail game data.

TA Transplant does not contain original Total Annihilation source code or game assets. It builds on the open-source [Robot War Engine](https://github.com/MHeasell/rwe) codebase and adds platform fixes, compatibility work, and original-TA behavior restoration where the upstream engine differs from the retail game.

> **Status:** playable development build, not a finished replacement for every original Total Annihilation feature.

## Project Goals

- Run RWE natively on modern Apple Silicon macOS.
- Preserve original Total Annihilation gameplay behavior rather than redesign it.
- Fix architecture-dependent behavior only when there is concrete evidence of a divergence.
- Restore missing original-TA commands and UI behavior incrementally.
- Keep original Total Annihilation data outside the repository.
- Avoid unnecessary renderer or engine rewrites when the existing path is already viable.

The source of truth for the macOS port and compatibility work is [docs/macos27/](docs/macos27/).

## Relationship to Robot War Engine

This repository is a downstream compatibility fork of **Robot War Engine (RWE)**.

RWE provides the core engine, including:

- Total Annihilation archive and data-format support;
- deterministic simulation infrastructure;
- unit, weapon, projectile, resource, pathfinding, and COB-script systems;
- OpenGL rendering;
- SDL-based platform/input support;
- multiplayer/networking infrastructure;
- the Electron-based launcher.

TA Transplant keeps that architecture and focuses on modern macOS support plus original-game compatibility gaps.

Upstream project: [MHeasell/rwe](https://github.com/MHeasell/rwe)

## Current macOS Status

The current public branch has been validated on **Apple Silicon / macOS 27**.

Confirmed work includes:

- native arm64 compilation with Apple Clang;
- SDL3 startup on macOS;
- OpenGL 4.1 core rendering through Apple's Metal-backed OpenGL driver;
- loading a stock Total Annihilation installation directly from an external data path;
- HPI/UFO/CCX/GP3 and related original-data access through the RWE VFS;
- main-menu startup with real retail data;
- loading and running skirmish maps with original OTA/TNT/FBI/3DO/COB content;
- an arm64-specific angle-conversion undefined-behavior fix that restores the semantics expected by the existing simulation tests;
- repair-command compatibility, including command routing, cursor/order handling, networking serialization, repair behavior, resource use, and human QA;
- an input-routing fix for the in-game debug overlay on macOS.

All produced engine/test executables in the audited native build were arm64; Rosetta was not required.

## Known Compatibility Gaps

This fork is still under active compatibility work.

On the current public branch:

- **Repair** is implemented and connected end-to-end.
- **Reclaim** is not yet implemented end-to-end.
- **D-Gun** command behavior is not yet implemented end-to-end.
- **Patrol** is not yet implemented end-to-end.
- several original function-key features (F1-F9) are still absent;
- cross-architecture floating-point determinism has not been fully proven for multiplayer;
- one sustained-combat freeze was observed during testing, but its cause has not been confirmed;
- macOS application bundling, signing, and polished distribution packaging are not complete.

See [docs/macos27/COMPATIBILITY_GAPS.md](docs/macos27/COMPATIBILITY_GAPS.md) and [docs/macos27/PORTING_BACKLOG.md](docs/macos27/PORTING_BACKLOG.md) for the detailed audit.

## Original Total Annihilation Data

**Original game data is not included.**

You must supply data from a legally owned Total Annihilation installation.

RWE can use an external directory directly:

```sh
./build/rwe --data-path "/path/to/Total Annihilation"
```

It also looks in the traditional default location:

```text
~/.rwe/Data
```

Typical data includes files such as:

```text
*.hpi
*.ufo
*.ccx
*.gpf
*.gp3
```

Do not commit retail game data, archives, executables, or other proprietary Total Annihilation material to this repository.

## Building on Apple Silicon macOS

The following is the currently documented, known-working native development path for the audited macOS environment.

### 1. Install build dependencies

Using Homebrew:

```sh
brew install cmake glew protobuf libpng pkgconf
```

### 2. Fetch submodules

```sh
git submodule update --init --recursive
```

### 3. Configure

On the audited machine, Homebrew's split Vorbis/Ogg headers caused SDL3_mixer to detect an unusable system `vorbisfile` configuration. The known-working configuration disables that specific system backend while retaining the vendored/STB Vorbis path:

```sh
cmake -S . -B build \
  -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DSDLMIXER_VORBIS_VORBISFILE=OFF
```

### 4. Build

```sh
cmake --build build -j"$(sysctl -n hw.ncpu)"
```

### 5. Run the test suite

```sh
./build/rwe_test
```

### 6. Run with original game data

```sh
./build/rwe --data-path "/path/to/Total Annihilation"
```

For the full environment audit and rationale behind the macOS-specific configure flag, see [docs/macos27/BASELINE_AUDIT.md](docs/macos27/BASELINE_AUDIT.md).

## Development Rules

Compatibility work should follow [docs/macos27/DECISIONS.md](docs/macos27/DECISIONS.md).

In particular:

1. Original Total Annihilation behavior is the compatibility target.
2. RWE's existing implementation remains the starting point; rewrites require evidence.
3. Native arm64 is preferred over satisfying dependencies through Rosetta.
4. Test expectations should not be changed simply to hide platform failures.
5. Behavioral changes should be supported by a failing test, observed divergence, documented platform contract, disassembly, or other concrete evidence.
6. Work should proceed one compatibility layer at a time.
7. Original copyrighted assets remain outside Git.

## Repository Layout

```text
src/rwe/          C++ engine and simulation
shaders/          OpenGL shaders
proto/            network protocol definitions
launcher/         Electron/TypeScript launcher and lobby
libs/             vendored/submodule dependencies
docs/macos27/     macOS port audit, decisions, backlog, compatibility gaps
cmake/            CMake support modules
```

## Platform Notes

The upstream engine also targets Windows and Linux. This fork's additional validation and compatibility work is currently centered on Apple Silicon macOS.

A Metal renderer rewrite is **not** currently required: the audited Apple Silicon system successfully created the existing OpenGL core context. Likewise, SDL3 is already present in the upstream codebase, so this project does not perform a separate SDL migration.

## License

The RWE-derived source in this repository is distributed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE).

The GPL license applies to the project source code, not to original Total Annihilation assets or other proprietary content supplied by the user.

## Legal / Non-Affiliation Notice

Total Annihilation and its original copyrighted content remain the property of their respective rights holders.

TA Transplant is an independent preservation and compatibility project. It is not affiliated with or endorsed by the original developers, publishers, or rights holders, and it does not redistribute the original game.
