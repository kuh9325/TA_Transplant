# TA Transplant

A preservation-focused **Apple Silicon / modern macOS compatibility fork of Robot War Engine (RWE)**, aimed at reproducing the original **Total Annihilation** experience with user-supplied retail game data.

TA Transplant does not contain original Total Annihilation source code or game assets. It builds on the open-source [Robot War Engine](https://github.com/MHeasell/rwe) codebase and adds platform fixes, compatibility work, and original-TA behavior restoration where the upstream engine differs from the retail game.

> **Status:** uncommitted source publication review candidate; owner review and provenance confirmation pending. Not a finished replacement for every original Total Annihilation feature.

## Project Goals

- Run RWE natively on modern Apple Silicon macOS.
- Preserve original Total Annihilation gameplay behavior rather than redesign it.
- Fix architecture-dependent behavior only when there is concrete evidence of a divergence.
- Restore missing original-TA commands and UI behavior incrementally.
- Keep original Total Annihilation data outside the repository.
- Avoid unnecessary renderer or engine rewrites when the existing path is already viable.

See [PUBLICATION.md](PUBLICATION.md) for source provenance, publication exclusions, and validation limits. Historical local audit material is not included in this source distribution.

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

This source review candidate incorporates the approved C2 native-development source snapshot. The historical integrated baseline passed 583 tests / 695,342 assertions. That is automated coverage, not complete retail parity or human acceptance.

It includes the simulation, command, mission, save, network, renderer and macOS packaging changes present in that baseline. All native engine source, tests, protocol definitions and 22 shaders are retained without byte changes by this publication preparation. Save writer v16 and hash contract 2 are retained.

## Known Compatibility Gaps

Full mission breadth, cross-architecture determinism, live multiplayer, save compatibility, interactive gameplay, and clean external-Mac validation remain qualified or pending. No stable signed/notarized binary release is claimed.

This source distribution deliberately omits custom application artwork. Its macOS bundle uses an icon-independent resource configuration; the operating system may display a generic application icon. No generic icon image is copied into this repository.

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

### 3. Configure a fresh native build

The source snapshot includes a version label so a source archive can configure without Git history. The public profile contains no custom icon or retail data. A supported Apple toolchain and the dependencies above are required.

```sh
cmake -S . -B build-public -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=27.0 \
  -DRWE_STANDALONE_MACOS=ON -DRWE_MACOS_ICON_FORMAT=NONE \
  -DSDLMIXER_VORBIS_VORBISFILE=OFF
cmake --build build-public --target rwe rwe_test rwe_bench ta_transplant -j 8
```

### 4. Run tests from the source root

```sh
./build-public/rwe_test
```

Graphics tests require an available graphics session and the source-root shader paths. Some optional runtime checks require externally configured, legitimately installed retail data. The build itself does not require retail data.

### 5. Run with original game data

```sh
./build-public/rwe --data-path "/path/to/Total Annihilation"
```

The standalone packaging helper remains a local validation tool. Its strict release policy still requires an icon and signing; this source candidate does not claim binary-release readiness.

## Development Rules

Compatibility work retains the established rules below. Source publication constraints are recorded in [PUBLICATION.md](PUBLICATION.md).

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
PUBLICATION.md   source provenance, exclusions, and validation limits
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
