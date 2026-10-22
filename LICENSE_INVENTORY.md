# Source license inventory

Prepared 2026-10-20 for the icon-free publication review. This document records evidence and unresolved scope. It does not change any license or assert new authorship. Existing notices govern.

The repository retains its GPLv3 LICENSE, the launcher GPL-3.0-only declaration, RWE attribution, and the dated modified-work notice in PUBLICATION.md. DOWNSTREAM_PROVENANCE.json records contributions, not ownership warranties. SOURCE_DEPENDENCIES.json supplies exact revisions and notice hashes for all 23 dependency repositories.

## Verified notice repair

`cmake/Modules/FindSDL2.cmake` and `cmake/Modules/FindSDL2_image.cmake` had a reference-only BSD notice pointing to a missing Copyright.txt. Their headers explicitly direct standalone distributors to substitute the full license. The referenced conditions and disclaimer are now included as comments, with every existing copyright line preserved. No executable CMake command changed. `FindSDL2_mixer.cmake` already contained the full conditions.

Primary source: [CMake v3.0.2 Copyright.txt](https://raw.githubusercontent.com/Kitware/CMake/v3.0.2/Copyright.txt), SHA-256 `9d0c2f3673a8d261d8111d7c6120e78e5dfa73d9f5b6c07d6447b5aeea691b26`. The matching [FindSDL](https://raw.githubusercontent.com/Kitware/CMake/v3.0.2/Modules/FindSDL.cmake) and [FindSDL_image](https://raw.githubusercontent.com/Kitware/CMake/v3.0.2/Modules/FindSDL_image.cmake) headers identify that same notice.

## Superproject material

| Paths | Evidence and status |
|---|---|
| RWE engine and modified/new C++ and GLSL | Existing repository license and Git/source continuity retained. GPL permits compliant downstream modifications without new permission from every upstream author. Phase 10J resolves all 110 paths: 97 verified owner-directed agent outputs, 12 verified GPL derivatives and 1 verified third-party license; zero unresolved. See PROVENANCE_REVIEW.md and PUBLICATION_PROVENANCE_ADDENDUM.md. |
| `src/rwe/math/rwe_math.cpp` | Existing round-up algorithm links to [Bit Twiddling Hacks](https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2). That source grants public-domain use of individual snippets unless noted; its prose collection is separately copyrighted. Existing link retained; no copied article added. |
| `libs/png++-0.2.9/png++/**` | BSD-style COPYING and source notices; `test/README` states permission for PngSuite image use, copying and distribution. Existing 941 PNG fixtures/outputs retained. No game artwork added. |
| `launcher/src/launcher/font/**` | Twelve inherited Roboto fonts and Apache-2.0 license retained. |
| Documentation and scripts | Existing upstream material and source references preserved. New source/tooling attribution is recorded; the 110 paths have final Phase 10J classifications with zero unresolved source origins; exclusive ownership is not certified. No external ownership is inferred. |
| Game names and product branding | Descriptive names, app strings and non-affiliation statement remain. No trademark clearance is asserted. Custom reconstructed icon and review artwork remain excluded. |

## Pinned dependencies

PASS below is limited to the identified standard license and retained evidence in unchanged source references. It is not whole-package legal clearance or approval to redistribute optional binaries. PARTIAL identifies a concrete scope that requires further evidence. GPLv3 can combine with the usual MIT, BSD, Boost, zlib and Apache-2.0 terms subject to their conditions; this inventory does not relicense them. See [GNU compatibility guidance](https://www.gnu.org/licenses/license-list.html).

| Repository path | Pin | License evidence | Status / precise scope |
|---|---|---|---|
| `libs/rapidcheck` | `b96a4e626ef4c7348dcd16c500353c2f997a9f3f` | BSD-2-Clause; `LICENSE.md` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/protobuf` | `f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c` | BSD-family; component-specific extras; `LICENSE` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/json` | `55f93686c01528224f448c19128836e7df245f72` | MIT; `LICENSE.MIT` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/imgui` | `6ded5230d043aa32c755e65c910c2af5002fb9f9` | MIT; separate bundled example notices; `LICENSE.txt` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/catch2` | `29c9844f688acb27c87338c39cd186ebfe41aa19` | BSL-1.0; `LICENSE.txt` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/utfcpp` | `6bbbaccabb1440d87acf5abfa3ed1ac604bfbe1a` | BSL-1.0; `LICENSE` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/asio` | `bd500f0a018db9a845ebaaed5c0318343ae9f497` | BSL-1.0; `LICENSE_1_0.txt` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/sdl3` | `683181b47cfabd293e3ea409f838915b8297a4fd` | zlib; separately licensed components; `LICENSE.txt; src/hidapi/LICENSE.txt; src/video/yuv2rgb/LICENSE` | PARTIAL. Select and document applicable hidapi alternatives and yuv2rgb terms for actual linked output; root zlib does not cover these automatically. Unchanged source references and notices are retained. |
| `libs/sdl3_mixer` | `cedfeef30e93db35eee6b25759117da63f8e5a4f` | zlib; separately licensed components; `LICENSE.txt; src/timidity/README; src/timidity/COPYING` | PARTIAL. TiMidity README grants GPL/LGPL/Artistic alternatives without a version in that paragraph. Record the selected applicable basis and modification obligations; binary packager currently omits this README. Source pins retain both README and COPYING. |
| `libs/protobuf/third_party/benchmark` | `5b7683f49e1e9223cf9927b24f6fd3d6bd82e3f8` | Apache-2.0; `LICENSE` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/protobuf/third_party/googletest` | `5ec7f0c4a113e2f18ac2c6cc7df51ad6afc24081` | BSD-3-Clause; `LICENSE` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/rapidcheck/ext/catch` | `03d122a35c3f5c398c43095a87bc82ed44642516` | BSL-1.0; `LICENSE.txt` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/rapidcheck/ext/googletest` | `e38ef3be887afc0089005e394c5001002e313960` | BSD-3-Clause; `LICENSE` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/sdl3_mixer/external/flac` | `833a260a7bc29f80899d33e8c1beb2fc56b4905b` | BSD/Xiph, GPL, LGPL, documentation FDL; `COPYING.Xiph; COPYING.GPL; COPYING.LGPL; COPYING.FDL` | PARTIAL. An expanded archive or enabled/linked configuration requires a path-level selection of the separate terms listed here and exact corresponding-source/notice mapping. This phase transmits gitlinks only. |
| `libs/sdl3_mixer/external/ogg` | `936fdd822a4e4fc963197b3eda931b89b52859a0` | BSD-3-Clause; `COPYING` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/sdl3_mixer/external/vorbis` | `a1938f4f0490402eb2a0fc50ef984b2f1fb6e786` | BSD-3-Clause; per-file terms retained; `COPYING` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/sdl3_mixer/external/opus` | `ac9f053e1db9cc1c7608b74e029c25ded0254e3e` | BSD-3-Clause plus retained patent/IPR texts; `COPYING; LICENSE_PLEASE_READ.txt` | PARTIAL. An expanded archive or enabled/linked configuration requires a path-level selection of the separate terms listed here and exact corresponding-source/notice mapping. This phase transmits gitlinks only. |
| `libs/sdl3_mixer/external/opusfile` | `840684e76b69ef2221b28402e7c21f4c586e6ae4` | BSD-3-Clause; `COPYING` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/sdl3_mixer/external/tremor` | `7afc84841ffa2e9610b65950fd233e6c428699c7` | BSD-style; per-file terms retained; `COPYING` | PASS. None for the stated root-license compatibility and unchanged gitlink scope; not certification of every optional file or binary. |
| `libs/sdl3_mixer/external/mpg123` | `e38e9027a3c1c254c2857c733017859c6da0ba77` | LGPL plus separately licensed components; `COPYING; doc/LICENSE` | PARTIAL. An expanded archive or enabled/linked configuration requires a path-level selection of the separate terms listed here and exact corresponding-source/notice mapping. This phase transmits gitlinks only. |
| `libs/sdl3_mixer/external/libxmp` | `75e85f8a5be65f9f95fb946086bee17da158166f` | MIT plus separate loader terms; `docs/COPYING; src/loaders/prowizard/LICENSE.txt` | PARTIAL. An expanded archive or enabled/linked configuration requires a path-level selection of the separate terms listed here and exact corresponding-source/notice mapping. This phase transmits gitlinks only. |
| `libs/sdl3_mixer/external/wavpack` | `c88aaf8fb902c547a11053273df289d4b892335f` | BSD-style plus separate plugin terms; `COPYING; license.txt; xmms/COPYING` | PARTIAL. An expanded archive or enabled/linked configuration requires a path-level selection of the separate terms listed here and exact corresponding-source/notice mapping. This phase transmits gitlinks only. |
| `libs/sdl3_mixer/external/libgme` | `8ebb4f65096fdf980b39be2283699d523ddc09af` | LGPL/GPL and bundled component terms; `license.txt; license.gpl2.txt; gme/ext/LICENSE` | PARTIAL. An expanded archive or enabled/linked configuration requires a path-level selection of the separate terms listed here and exact corresponding-source/notice mapping. This phase transmits gitlinks only. |

## Included and external notice records

The `license_evidence_files` array in SOURCE_DEPENDENCIES.json lists 72 files/headers, their exact committed revisions and SHA-256 values. Component sources are not embedded in this superproject archive. Retain their notices when fetching or redistributing those sources. A source reference to a dependency is distinct from a bundled binary or offline expanded archive.

Built-in dr_libs and stb_vorbis preserve their complete dual-license text; hidapi and yuv2rgb preserve separate terms. TiMidity retains COPYING, README and source headers. No silent license selection is made here. The known binary packager gap remains separate: it copies TiMidity COPYING without README and takes a utf8_range notice from a source revision that must be matched to the actual host dylib. No binary is approved by this inventory.

No npm packages, host GLEW/Protobuf/Abseil binaries, game data, saves, proprietary icons or Git histories are bundled. Launcher package declarations/lockfile remain; a launcher binary would need its own resolved dependency and notice inventory.

## Phase 10H correction: inherited CMake installer template

`cmake/Modules/NSIS.template.in` is a third-party CMake template, not solely RWE-authored GPL source. RWE import commit `324fd30ea0a761651844c8d3679e480bafd24682` explicitly records the copy; its content matches [CMake 3.10.3 Modules/NSIS.template.in](https://github.com/Kitware/CMake/blob/v3.10.3/Modules/NSIS.template.in) after line-ending/trailing-blank-line normalization. The later RWE start-menu modification is retained. This comparison identifies a matching licensed release, not the exact version installed by the original importer.

The complete [CMake copyright/license notice](licenses/CMake-3.10.3-Copyright.txt), verified against [the official 3.10.3 Copyright.txt](https://github.com/Kitware/CMake/blob/v3.10.3/Copyright.txt), now accompanies the template and its retained historical copies. The BSD conditions permit redistribution and modification with notice retention; separate rights-holder permission is not needed for this correction. The existing NSIS script-source and author references remain in the template. No executable template line changed.

The root GPL, three FindSDL module notices, png++/PngSuite notices, Roboto Apache text and embedded font copyright/license records are retained. No missing mandatory notice is currently identified in the reviewed superproject tree after this correction. Dependency binaries and expanded archives retain the separate unresolved scope described above.

## Phase 10I refinement: in-file CMake notice and hash permutation

The complete CMake 3.10.3 notice is now also prepended as NSIS comment lines in `cmake/Modules/NSIS.template.in`; the original CRLF template body is byte-identical. Existing FindSDL2/FindSDL2_image/FindSDL2_mixer notices and every original author line are unchanged.

`src/rwe/grid/StateHashGrid.h::avalanche` (lines 24–29) uses the same shifts/multipliers as `lowbias32` in [hash-prospector](https://github.com/skeeto/hash-prospector/blob/396dbe235c94dfc2e9b559fc965bcfda8b6a122c/README.md), verified at `396dbe235c94dfc2e9b559fc965bcfda8b6a122c`. The matching implementation is available under the [retained Unlicense](licenses/hash-prospector-UNLICENSE.txt). Its final xor is returned directly and unsigned literal suffixes are added. This verifies a permissive source basis for the snippet; it does not identify the historical copying route or relicense the locally authored wrapper. No gameplay source was changed.
