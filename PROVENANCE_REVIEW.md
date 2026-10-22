# Source provenance review — final Phase 10J findings

Prepared 2026-10-22. This Phase 10K assembly adopts the [Phase 10J provenance addendum](PUBLICATION_PROVENANCE_ADDENDUM.md). It supersedes the Phase 10I classifications and requests for source declarations for 75 files. No source-provenance uncertainty remains in this exact 110-file inventory.

| Classification | Files |
|---|---:|
| VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | 97 |
| VERIFIED_GPL_DERIVATIVE | 12 |
| VERIFIED_THIRD_PARTY_LICENSE | 1 |
| UNRESOLVED_WITH_SPECIFIC_REASON | 0 |

These findings record observed construction and concrete reuse, not exclusive authorship, copyrightability, a clean-room claim or a worldwide originality warranty. All 75 previously uncertain constructions have successful creation records and matching post-hashes, bound through later project revisions to the reviewed source. Ten have concrete GPL reuse and receive that primary classification. The benchmark's October 13 owner-directed creation and reuse of existing loading/restoration code are established. No additional owner source declaration is needed.

RWE continuity remains anchored at `b2d8a314b14142d8873400160768777532b1dcab`. The separate 137-path RWE derivative audit is preserved. Existing GPL-covered modifications do not require a new permission letter from each upstream author. Existing copyright, license, warranty and attribution notices remain. No new copyright holder is invented.

`GameParameters.h` extracts upstream declarations; `game/matrix_util.test.cpp` mirrors upstream camera math. The `StateHashGrid.h::avalanche` permutation has the verified hash-prospector Unlicense basis documented in LICENSE_INVENTORY.md. That finding does not establish its historical copying route or relicense the GPL wrapper. These bounded qualifications do not reopen the resolved 110-file source-basis review.

The private Phase 10I and Phase 10J evidence packages retain exact construction/results, owner instructions, donor revisions, line comparisons and candidate hash bindings. Private transcripts and development history are excluded from publication. The table below preserves Phase 10J ordering, classifications and exact candidate SHA-256 values.

Retail behavior and data formats informed compatibility work; this is not a grant of rights in retail data or artwork. Retail assets, private saves and disputed icons remain excluded. TA Transplant and its independent unofficial-project disclaimer are final owner-approved presentation decisions. The owner has adopted this Option B source candidate with sanitized history and retained attribution/license notices. Only separately authorized remote publication may update GitHub.

## Exact 110-file classification

| # | Path | Final classification | Candidate SHA-256 |
|---:|---|---|---|
| 1 | `packaging/macos/Icon.cmake` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `fab76e3219469844e38f5ba85096d740eb798ef265ec4ee24f0a9fec89649ab5` |
| 2 | `packaging/macos/Info.plist.in` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `e4905b964001471c2c0f84fe2da4ff55114cc95b39adefb8b6299651bc5299cb` |
| 3 | `scripts/export_cloud_source.py` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `89102a5512c807cc2727854929552c28b8160e5bf2a0784b7fc64ee8f2079c70` |
| 4 | `scripts/macos_bundle.py` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `be37dbd245ee24e8531b1ded742e7dbc01815894b68d808798ada48caec6ce1a` |
| 5 | `scripts/macos_icon.py` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `dbfd1279cdc6124daac184ca8d6d47deddfc2094148c4aec851497ec59086378` |
| 6 | `scripts/phase8c/allocations.inc` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `6fa654101d8b3456fd8e4dbf0459583e7b233f96e4cc3e1133a0c7044e5dc4b6` |
| 7 | `scripts/phase8c/build_observer.py` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `f0478a0218efc387976539add9862bd9ae9fa108c451e522d64cc29f292e1b11` |
| 8 | `scripts/phase8c/hash_diagnostics.inc` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `2797a8639888391b901c6d9f890975b90ddd7144d0c1f044563cfb2e78f22a86` |
| 9 | `scripts/phase8c/run_benchmarks.py` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `c88cc40b253c686a91ac8ad25d55c346b412e82736d5bf38b999349e75f8f977` |
| 10 | `scripts/run_followup_regressions.py` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `311035537832ce8438246527a7e3c1003f799100b1c5bad95370ff5f61461521` |
| 11 | `scripts/run_mission_completion_audit.py` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `469d38f2191ac507ec16437c417c6d304535114070da8ead0eaefd0fa158ef3d` |
| 12 | `scripts/run_mission_followup.py` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `2aedcbb89a3b73d6a469c2cf028149ef8efb65c11754e02b772593dd90f3a13b` |
| 13 | `scripts/summarize_mission_completion_audit.py` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `f40dd3c86f4e51d9dd0d1a9bac7f3c0909095c1624385156b8be7f0f2c58220d` |
| 14 | `src/rwe/CampaignClosure.test.cpp` | VERIFIED_GPL_DERIVATIVE | `3bcdc826fed83abfee98d3cda05b3b28a310eea0afda8c9e2cc92e68d883c7f3` |
| 15 | `src/rwe/CampaignProgress.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `db8d5795436212caf5d26f86973fe9b12c9fef218a6f3aba9226aee2363b4d0e` |
| 16 | `src/rwe/CampaignProgress.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `d21917b6cc2206bbd065ab4cecc82504a3ae5841ca4c85b85c992c225809f7b7` |
| 17 | `src/rwe/CampaignProgress.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `d15beb42353a489aed1778d254e9328b91bbbe716ac4abbada75aca322e727bd` |
| 18 | `src/rwe/EndMissionScene.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `33f718051b00a752d04de97949ae534713d30e2321c86ae1bc1fc6f9d1baeba5` |
| 19 | `src/rwe/EndMissionScene.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `57d7724a0455d381ab93deb07d918f0932204f6b8a154e375125ed571783aee3` |
| 20 | `src/rwe/EndMissionScene_util.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `ebf40e36c0c03ec86055e5c69ec7782ecd75e6d56e8624b8576804a293c7d839` |
| 21 | `src/rwe/EndMissionScene_util.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `e309fc297e5bbd5cdf4bd631b3c4521d0f3c50eba7245ea8e7ddc946fcf32306` |
| 22 | `src/rwe/EndMissionScene_util.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `c65a677b2ff46b29146bd9a7c9e40b5697f44d6abdf0e8d6ddd5301aa9e6cce9` |
| 23 | `src/rwe/GameParameters.h` | VERIFIED_GPL_DERIVATIVE | `04d55bf1d94fa64b719674e80d66621a0ab7aad2ef12830b9038fbef18e6f5a9` |
| 24 | `src/rwe/LoadingScene_util.test.cpp` | VERIFIED_GPL_DERIVATIVE | `f89129fed53d4299872ae719fd965d5c2cdf32f9853c12efbfcfdccb4c38e0d2` |
| 25 | `src/rwe/MissionCompletionAudit.inc` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `08673522725bed8690489bc475d11fceacd7eb66daf1ae77c76d7ef25298576e` |
| 26 | `src/rwe/MissionCompletionFollowup.inc` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `5fd75a674486e95f90517a606b22bfaa67704f8484efc7d1ef1c9e8f68e8a37c` |
| 27 | `src/rwe/MissionCompletionPhase7C.inc` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `0bb313b99c083f5b8be1d03501df01c6a83bc30874669d60abaa96b2c43bdc86` |
| 28 | `src/rwe/RecoverySaveRegression.inc` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `4a49c454915186ffb5ba53a82dfd7a3144fb0a38dbc7683c1db0077408b7fb4e` |
| 29 | `src/rwe/RenderService.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `911343b8a484ca110f37369ea9aa61c54b2bc08e8c39b12b580e03bd57976910` |
| 30 | `src/rwe/SkirmishClosure.test.cpp` | VERIFIED_GPL_DERIVATIVE | `2eceee807ccfb6d710640147ad8f099211693f9fb4c3ceeb509dbdb0e9529835` |
| 31 | `src/rwe/SkirmishSettings.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `5e7521aa31c09911022de3495e3e3088916ee8680b3c7d7a3f96195f36749328` |
| 32 | `src/rwe/SkirmishSettings.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `ebece563d8c340597617a7735efc6d66f7b776ac4284b54368addb405eee16b0` |
| 33 | `src/rwe/game/GameNetworkService.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `75cba321fcfef10be7eed2fa786813fd20526927538e061746144e047f16bb5d` |
| 34 | `src/rwe/game/GameScene_util.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `6f200b35fc8cbbff016160e2c878047a7999a826b8dd9da3880f6e032219646a` |
| 35 | `src/rwe/game/PlayerCommandService.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `290a9e47843c32bb536c4e5f6ffbb7a72a39ef492098e3648546ab640d6d7202` |
| 36 | `src/rwe/game/SceneTickScheduler.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `69e91ebd80aea4648394ccea889f148090454c14bec6458bad1d9f7856868fd7` |
| 37 | `src/rwe/game/SceneTickScheduler.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `2d3494292cb54a4a0ff7fa93b9679ffbd5932a8e77fc14c2dba770b96074e1c0` |
| 38 | `src/rwe/game/matrix_util.test.cpp` | VERIFIED_GPL_DERIVATIVE | `a1b954e5632dcc74db6c3ec9cbeb43f77c2aad1e35a8c5b0ade56fdd1e738f3d` |
| 39 | `src/rwe/game/selection_util.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `42199098101781d53d4ba3542968931c39d5c8382c60f10c0b58c1db6bd2a6a5` |
| 40 | `src/rwe/game/selection_util.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `3ea75b67de5d53b74e58dbedbcd764407ef9378e1f007bd021a6c171be7b671d` |
| 41 | `src/rwe/grid/StateHashGrid.h` | VERIFIED_THIRD_PARTY_LICENSE | `5de8b4f184931978c30ab52bf15d13d78753c583b5520d25b25bfe8145614cb0` |
| 42 | `src/rwe/io/campaigntdf/CampaignTdf.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `ddd448996e7f03f48521bde6251ad5c63375cae4d23b0a686f516bbcf90f675a` |
| 43 | `src/rwe/io/campaigntdf/CampaignTdf.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `417fd381c0a58246069e8b5a0dc365ff33d2169bfdb63fc31311a0ca0f592413` |
| 44 | `src/rwe/io/campaigntdf/CampaignTdf.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `b2a68718ef2aeb32d14add2058d5b2bfb2db15f9c41193331e19c2cd163d1d1a` |
| 45 | `src/rwe/io/fbi/io.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `bb7ce3696feb50efc30067838b61b2cb23dcc290aebc2248fe88912487786db9` |
| 46 | `src/rwe/io/tnt/TntArchive.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `e959479353752958cdd99552863d2e968dc0f8cc232e67efc2c8f48f29e1aa4d` |
| 47 | `src/rwe/mesh_util.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `a61977b58b31a1e43e41cad06ca407705ba854004e38ec6ca8700e7b681c44ee` |
| 48 | `src/rwe/proto/serialization.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `05ca9299ee9f3ae2969b075921cefe4ee1c64a2116e8cea05c213aaa40960371` |
| 49 | `src/rwe/save/Phase8bSaveHash.inc` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `eb8b5d7c8ccf3fa2966b44e1f4af819dc22626e12c2736d45fb2c6950e4f7e6d` |
| 50 | `src/rwe/save/Phase8cHashRegression.inc` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `bba6e9fbab8c2b98173156c03b412bcfff51e8ed9ff55831b65251ebc3b8c37a` |
| 51 | `src/rwe/save/SaveCodec.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `9b1711112e92586dff70da0b9eaa5212b3735efd2ed30c7f58b17c7db48c92f2` |
| 52 | `src/rwe/save/SaveCodec.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `163a87f8decb3a3206aaf534054ca0faec49a38ef26b7192e9a7e59dd6667a6c` |
| 53 | `src/rwe/save/SaveGameService.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `c10690b0db98ddc3c53bd079357f599764ee7a2f663db38e7913540390e57385` |
| 54 | `src/rwe/save/SaveGameService.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `7f574913c2f30fa9510ac6297b60d9f306e1556dec9b6136edfd0046a57b0da8` |
| 55 | `src/rwe/save/SaveGameService.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `88c20d083e9516ab664bc052f02933c9e67c64c3c50341cbaf36d4148d168ab4` |
| 56 | `src/rwe/save/SaveListDialog.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `72c57b24104129086b0133f1c5c2d1396142e8a85d791ce87d83b53c65eef268` |
| 57 | `src/rwe/save/SaveListDialog.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `c0dcfe0074213c1abd026049900e79d5dec996fcf5d9a42215305a2d54e64210` |
| 58 | `src/rwe/save/SavedGame.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `6566df1cf3b01dc5add83e0eb2b741b4481659853299281096bf4db399225d96` |
| 59 | `src/rwe/save/SavedGame.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `bdc357bee421fbf1759f5b5deba6c2366dffc2d6d4b1acd32b28c9d3c5307326` |
| 60 | `src/rwe/save/SavedGame.test.cpp` | VERIFIED_GPL_DERIVATIVE | `d5cbac853583fe49a689b128af956f1c590cea609918a43af41d548998201ecb` |
| 61 | `src/rwe/sim/AircraftService.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `e2e1c8c04df13039102a1420fe3e26bd3f6f4a9673ebde575af36bfebd9fc1c4` |
| 62 | `src/rwe/sim/AircraftService.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `5a411973b33832a31f1053100b98d08a4acf293f499ec6bae0839f03585c5a3f` |
| 63 | `src/rwe/sim/Bomber.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `91c8fef1eff61e99277125d14f5436f9e9e0e33e691d82e89ac7250129b85bdd` |
| 64 | `src/rwe/sim/CampaignAi.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `e97bef1eb6350913ddd16629f5c3e2332e0fb1fba55b39ef5dc253d7b79a7100` |
| 65 | `src/rwe/sim/CampaignAi.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `1488d11d14ddd7d5db898f95fa57ec34e225bb7fbab78ebd78a61eae127071e1` |
| 66 | `src/rwe/sim/CampaignAi.test.cpp` | VERIFIED_GPL_DERIVATIVE | `36ea52188925559815c713bae4c118353abda5711cdc0b76431cffe6b5bb1704` |
| 67 | `src/rwe/sim/CampaignAiProfile.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `62e863dedffdd84f70e5f49acb177dd6081282699113875d7675da1fcd373382` |
| 68 | `src/rwe/sim/CampaignAiProfile.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `442d10ba06e5e368adfdd987fc801a05c02612be3934a61c71e84e0f5e89466f` |
| 69 | `src/rwe/sim/CampaignAiProfile.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `45dae05eae81a5ff825c524aa288fc0419d666705debc6ffec8e7c90f4a0a6c6` |
| 70 | `src/rwe/sim/DestructibleFeatures.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `7fbe695cd03437d003d6efb2b63fbd88f5b688c056475de5c792cfbe26da0017` |
| 71 | `src/rwe/sim/Emp.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `c857bb076df2b89b942dabf6affdd3b24708026efd7511e6ae35a02484bc8d13` |
| 72 | `src/rwe/sim/FeatureSimulation.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `a6c2a68a0f14fe2c73dcfc8d1bebf47863bc3ba15f81369fbf928cd7f6ea7f96` |
| 73 | `src/rwe/sim/GameSimulation.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `3ce06678039b74866e528b308a0e49028dae3ac137ef293e3bb4b1317bffe160` |
| 74 | `src/rwe/sim/Kamikaze.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `9dafd8259c88245760c6ae2ab992698275a3bf0706eb9a3707a617f321a9cc9c` |
| 75 | `src/rwe/sim/MapTerrain.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `cf9c601ff07c2f5880be7804bbe84287f2bbd8fa2f76388fdabaaeab972903d5` |
| 76 | `src/rwe/sim/MeteorShower.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `bad882e952642f180e52899f3246c99977e7132eacf02e6da8c14339be183e79` |
| 77 | `src/rwe/sim/MeteorShower.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `7fdcd0c99c8487c8b2f328156af0d6f9051a6fabda663708a158fc13c36e472a` |
| 78 | `src/rwe/sim/MissionInfo.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `41d99107139133bbfc087b80fc9cf9bd115a688e29d8ac51d1d0b24c3d23a0ed` |
| 79 | `src/rwe/sim/MissionInitialCommand.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `b0b4e4a5d7c775c5046a34e542faae22d828a66f726be185686820b3f9157a7a` |
| 80 | `src/rwe/sim/MissionInitialCommand.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `30fc2f958e3cdf5d7f4c0aba99741dba515a2260a4a5257ee61a8c441ec0a9b5` |
| 81 | `src/rwe/sim/MissionInitialCommand.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `03d9f69948f35b4817a3b4f1498eccb7d6ef41dc29c34c80218c60bc2783555d` |
| 82 | `src/rwe/sim/MissionObjectives.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `5f6a97392f3551cbf7dea28c50a34deb946837a8c1865ef9bfed1d4210950f86` |
| 83 | `src/rwe/sim/MissionObjectives.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `b8a1c11bc2b2a2655b3c699850cdf9f849d48c5a37393ac9e59f698a4f2362ed` |
| 84 | `src/rwe/sim/MissionObjectives.test.cpp` | VERIFIED_GPL_DERIVATIVE | `531eba644599b6c940c17eea60c552fee8c149f0ebc310edbd08efa679e7b605` |
| 85 | `src/rwe/sim/UnitBehaviorService.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `dd567e466e3ca570db31b6267310554dea7d52dd6d1ed46acf742f78249d2755` |
| 86 | `src/rwe/sim/UnitMoveOrders.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `631ca8e29121e8696d41c7f22a336418f5841468ac9804a5ada658785370ecee` |
| 87 | `src/rwe/sim/UnitState.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `4899eb518f710e860491e10feb796a2e3ebf7162de247d22a47cc231d8cf8a04` |
| 88 | `src/rwe/sim/Veterancy.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `e70c0d48b2bc42acfbd38415bb9f713d64798f1c19681b1dff70579f13832497` |
| 89 | `src/rwe/sim/Veterancy.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `d83d5aafc929e1bc938e8ff76a9d06b329d4ceb1f85f46af46d20dc17fc95eca` |
| 90 | `src/rwe/sim/cob.test.cpp` | VERIFIED_GPL_DERIVATIVE | `8d405972a56295772f1c6171cf47f1f1ee1ecae8c7546ca43cab349379b1be35` |
| 91 | `src/rwe/ui/UiFactory.test.cpp` | VERIFIED_GPL_DERIVATIVE | `4c742b42220fc42bc1ce90cf1f1a93a50f38496bd12a46368384362988fbf968` |
| 92 | `src/rwe/ui/UiListBox.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `f400a80d9b53b1badd249f5599de60941f4567d32a37b4b0fc11164680f4a425` |
| 93 | `src/rwe/ui/UiPanel.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `e514b94c656fd4b535ad08a978edd009d01d5b70ed9c41b4fa480533f8ce874b` |
| 94 | `src/rwe/ui/UiSlider.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `d113f0ab1180898a9cceeac66e1b4b30347025dd51527c38fd10922e0141ad72` |
| 95 | `src/rwe/ui/UiSlider.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `012bae6540e8e50cc7f3422064c4df1ef2ba724cd59e2777b64eef4b7f9962d8` |
| 96 | `src/rwe/ui/UiStagedButton.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `5ee59da1408e0d94a09af9101a4c0713908d9930db114fc9da702b90e33f2062` |
| 97 | `src/rwe/ui/UiTextBox.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `cc48d4450fea4a78de62a1e746fed62df1ef6027e8c698424b176ae50e02d63d` |
| 98 | `src/rwe/ui/UiTextBox.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `a45f558bd96f4553fdd068479957322b707fa34581954612673193c7d3c0f4d0` |
| 99 | `src/rwe/ui/button_sound_util.h` | VERIFIED_GPL_DERIVATIVE | `bc4677371b6c606f644072d57e09f99a3dce3fbd6750d1f7bce988214f6c9163` |
| 100 | `src/rwe/ui/button_sound_util.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `650bae956a6556b7fb05cb34f43559073061b0a8dcfbc3787afb8ea207736e04` |
| 101 | `src/rwe/ui/difficulty_util.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `79af57c3f563ebfe4e8d4ac16547c24b922b5293b338da503adfaacaaf45390a` |
| 102 | `src/rwe/ui/difficulty_util.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `ae09af9874752ae3176e697f3bb8d5a9d48119489cea510c2b21c8ab041562ec` |
| 103 | `src/rwe/ui/narration_util.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `9bcca3b39712c4a537c0cc68b9767b9526d8a074810bf17312361c2d72957682` |
| 104 | `src/rwe/ui/narration_util.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `682f20000b4d2c69e09327bec9f98d2df17a2a3df5fed33971fac92bc1188933` |
| 105 | `src/rwe/util.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `8140d530366b49de17d159fefcf553f5fd6920bbdca2d9e410e7a2e809a6c50a` |
| 106 | `src/rwe/util/ConfigFile.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `3d5606fa9796d35a73052351bcf323860a30dac95cf603b136b0378870676d54` |
| 107 | `src/rwe/util/ConfigFile.h` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `2b63d4069069a12ce81114d5a86c77c683d28b41f36e1160440ffa695d4b00f6` |
| 108 | `src/rwe/util/ConfigFile.test.cpp` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `c94b72d50ea1b03fca43f11ba8550b159e03f7aca15428f45c44f037cad14e74` |
| 109 | `src/rwe_bench.cpp` | VERIFIED_GPL_DERIVATIVE | `e8b39a1831b5d4b76d23ca5f902e5083962fc59d46029a750e45d35c9c0cc696` |
| 110 | `src/rwe_bench_recovery.inc` | VERIFIED_OWNER_DIRECTED_AGENT_OUTPUT | `9965f06bd2f0c30c32f18edd12a14c60743ca8ba847b71dce8ff578c67bcb8a4` |
