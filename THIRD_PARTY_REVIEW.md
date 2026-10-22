# Third-party source and redistribution review

Prepared 2026-10-20 for source review. This inventory records retained evidence; it is not legal clearance. Component terms remain in their original files and take precedence over this summary.

| Component | Retained license evidence | Distribution scope |
|---|---|---|
| Robot War Engine | Root `LICENSE` (GPL v3), existing source notices, upstream history | Modified engine source; retain attribution, license and warranty notices and modified-work notice/date |
| Asio / utfcpp / Catch | `libs/asio/LICENSE_1_0.txt`, `libs/utfcpp/LICENSE`, Catch license files | Boost Software License 1.0; keep notices |
| ImGui / nlohmann JSON | `libs/imgui/LICENSE.txt`, `libs/json/LICENSE.MIT` | MIT notices |
| Protobuf / GoogleTest / RapidCheck | Their LICENSE files | Component-specific BSD-family terms |
| Google Benchmark | `libs/protobuf/third_party/benchmark/LICENSE` | Apache 2.0 |
| SDL3 / SDL3_mixer | Their LICENSE.txt files | zlib; separate bundled-component licenses also apply |
| dr_libs / stb_vorbis | `src/dr_libs/LICENSE`, license block in `src/stb_vorbis/stb_vorbis.h` under SDL_mixer | Dual permissive/public-domain alternatives; preserve complete texts |
| TiMidity | `libs/sdl3_mixer/src/timidity/COPYING` and `README`, source headers | COPYING contains Perl Artistic terms; README records an additional GPL/LGPL/Artistic choice. Exact release license basis and downstream modification obligations require confirmation. Source retains both texts. |
| hidapi / yuv2rgb | Their SDL component LICENSE files | Multiple component-specific notices |
| Optional codec trees | FLAC, libxmp, GME, mpg123, Ogg, Vorbis, Tremor, Opus, Opusfile, WavPack license/COPYING files | Mixed BSD/MIT/LGPL/GPL terms and related IPR notices; do not infer terms from SDL alone |
| png++ | `libs/png++-0.2.9/png++/COPYING` and source headers | BSD-style source and PNG fixture notices |
| Roboto | `launcher/src/launcher/font/LICENSE.txt` | Apache 2.0 font notice; previously public font payload |
| Electron launcher dependencies | `launcher/package.json`, lockfile and upstream package notices | No npm dependency installation or launcher binary is included by this preparation |
| GLEW / host Protobuf / Abseil / utf8_range | Actual installed package source/license files | Host build dependencies, not part of the superproject source payload; binary/source-version and complete notice correspondence remain release work |

The source candidate retains all nine top-level gitlinks and the same 23 recursively pinned repositories. Fetch dependencies by those pins. A public superproject push does not publish the local dependency object databases. An expanded offline source archive would be a different deliverable and needs a complete media/binary/notice audit: upstream trees include optional Windows DLL/LIB examples and test fixtures. They are not a proposed binary release.

The validated standalone profile disables optional external FLAC, mpg123, FluidSynth, GME, MOD, Opus, WavPack and system Vorbisfile/Tremor decoder backends, while retaining built-in formats, dr_libs, stb_vorbis and TiMidity. Changing the configuration changes the distribution review scope.

The existing packager copies TiMidity COPYING but not its README choice statement, and its utf8_range notice comes from the source pin while the dylib comes from the host Protobuf installation. These remain explicit binary-release issues. No bundled binary is approved by this source review.

No original Total Annihilation game-data license is supplied. Game archives, saves, owner reference images and the disputed reconstructed insignia are excluded from this candidate. A software license, a redrawn SVG, or a non-affiliation notice does not establish artwork or trademark rights.

Phase 10F adds the detailed [source license inventory](LICENSE_INVENTORY.md) and [exact dependency/notice manifest](SOURCE_DEPENDENCIES.json). Two legacy CMake modules now include their verified BSD notice in full; executable CMake commands are unchanged.

Phase 10H also identifies the inherited CMake NSIS installer template. Its verified full BSD copyright/license notice is retained in [licenses/CMake-3.10.3-Copyright.txt](licenses/CMake-3.10.3-Copyright.txt). The template and embedded author/source references are unchanged. See [PROVENANCE_REVIEW.md](PROVENANCE_REVIEW.md) for the narrower downstream source-basis decision.

Phase 10I retains that CMake notice and additionally embeds it as comments in the NSIS template, with the original body unchanged. The matching `StateHashGrid.h::avalanche` permutation has a verified [hash-prospector source](https://github.com/skeeto/hash-prospector/blob/396dbe235c94dfc2e9b559fc965bcfda8b6a122c/README.md) and [retained Unlicense](licenses/hash-prospector-UNLICENSE.txt). See PROVENANCE_REVIEW.md for the final Phase 10J 110-path classifications and zero unresolved origins; PUBLICATION_PROVENANCE_ADDENDUM.md supersedes earlier source-uncertainty statements.
