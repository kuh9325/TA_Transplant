# TA Transplant

TA Transplant is an independently developed Apple Silicon / macOS compatibility fork of [Robot War Engine (RWE)](https://github.com/MHeasell/rwe), designed to work with separately supplied Total Annihilation game data.

**Unofficial independent project.** TA Transplant is not officially affiliated with, sponsored by, or endorsed by the rights holders of Total Annihilation, its original developers or publishers, or the upstream RWE project. References to Total Annihilation identify the game and data formats this software aims to support. They do not imply an official port or release.

**Status:** source publication completed on 2026-10-23 at commit [`deddba33`](https://github.com/kuh9325/TA_Transplant/commit/deddba33dd11a9d0616e9caf9e40434f71714478). The published scope retains the owner-approved TA Transplant name, independent/unofficial disclaimer, icon-free source, exclusion of proprietary retail assets, sanitized history, and upstream attribution/license notices. No binary release is approved.

## Source and attribution

This project builds on RWE, developed by Michael Heasell and other RWE contributors. Their source, history and existing copyright and license notices are retained. Downstream work adds macOS support and compatibility changes; this fork does not claim sole authorship of the engine.

The inherited sponsorship link in `.github/FUNDING.yml` points to the upstream RWE maintainer. It is not a funding channel for this fork or evidence of upstream endorsement.

See [PUBLICATION.md](PUBLICATION.md), [PROVENANCE_REVIEW.md](PROVENANCE_REVIEW.md), [DOWNSTREAM_PROVENANCE.json](DOWNSTREAM_PROVENANCE.json) and [LICENSE_INVENTORY.md](LICENSE_INVENTORY.md) for the source basis and retained notices.

## Game data and artwork

Original retail game assets are not distributed with this project. Users must supply legitimately obtained Total Annihilation game data from an installation they are authorized to use, for example:

```sh
./build-public/rwe --data-path "/path/to/Total Annihilation"
```

The engine can read original visual assets from that external installation. This does not grant permission to redistribute those assets or screenshots of them. Do not add retail archives, executables, saves, game-derived artwork or promotional game logos to the source distribution.

The publication profile has no custom application icon. It uses `RWE_MACOS_ICON_FORMAT=NONE`; it does not redistribute an operating-system icon image. The approved private development icon is outside this distribution.

## Validation scope

The reviewed executable inputs retain the existing Phase 10E result: **583 tests / 695,342 assertions passed**. Phase 10I changes documentation and license notices only and reuses that evidence; it does not claim a fresh test run. Human gameplay acceptance, complete mission coverage, live multiplayer, cross-architecture determinism and clean external-Mac compatibility remain qualified or pending.

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

The standalone packaging helper remains a local validation tool. Its strict release policy still requires an icon and signing; this source publication does not claim binary-release readiness.

## License and names

The RWE-derived work is provided under GNU GPL version 3; see [LICENSE](LICENSE). Third-party files retain their own applicable licenses, listed in [LICENSE_INVENTORY.md](LICENSE_INVENTORY.md) and [THIRD_PARTY_REVIEW.md](THIRD_PARTY_REVIEW.md). Keep existing copyright, attribution and warranty notices. The software is provided without warranty as described in those licenses.

Total Annihilation, its logos and original content belong to their respective rights holders. The source licenses do not grant rights to game data, artwork or trademarks. "TA Transplant" is the owner-approved project name. This disclaimer does not by itself establish trademark clearance or rights-holder permission.

The Electron launcher source and pinned dependency references are included for source completeness. No installed npm packages or launcher binary are included. Existing upstream service endpoints are source references, not a claim that this fork operates or is endorsed by those services.
