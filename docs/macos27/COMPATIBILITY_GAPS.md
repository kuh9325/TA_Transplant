# RWE — Original-TA Command Compatibility Audit

Audited at HEAD `ad68d9aa` (post-Phase-3) against real TA game data at
`~/Games/OriginalData/TotalAnnihilation` and the official TA manual's
key list (F1 unit-info, F2 options, F3 last-reporting unit, F4 scores,
F5–F7 bookmarks, F8 movie capture, F9 screenshot PCX, F11 debug toggle,
F12 clear chat; order keys A/G/M/P/S/D/C/R/E/L/U/K/O/B/V/F/X/T/N/H).

Human-observed runtime evidence (authoritative):
- **Fn+F10: WORKS** — RWE debug window opens → SDL function-key delivery
  is fine on macOS; F-key gaps are missing RWE bindings, not Fn issues.
- **Reclaim / D-Gun / Patrol: not working.**
- **Repair: implemented Phase 4** — order/cursor/proto/behavior complete,
  human-QA'd end-to-end (see §2 addendum).

Classification legend:
- **connected** — implemented and wired end-to-end
- **disconnected** — exists but not reachable from input/UI
- **assist-only** — reused for construction assistance; not TA repair
- **data/UI only** — parsed/loaded from game data; no behavior
- **absent** — does not exist

## 1. Compatibility matrix

| Feature | input/hotkey | GUI/button | cursor | PlayerCommand | network serialization | UnitOrder/sim type | UnitBehaviorService behavior | required data parsed | classification |
|---|---|---|---|---|---|---|---|---|---|
| F1 unit info | absent | UNITINFO.GUI in data, never loaded | n/a | n/a | n/a | n/a | absent | — | **absent** |
| F2 options menu | absent | gameoptions.GUI in data, never loaded | n/a | n/a | n/a | n/a | absent | — | **absent** |
| F3 last-report unit | absent | — | n/a | n/a | n/a | n/a | absent | — | **absent** |
| F4 scores | absent | REPORT.GUI in data, never loaded | n/a | n/a | n/a | n/a | absent | — | **absent** |
| F5–F7 bookmarks | absent | — | n/a | n/a | n/a | n/a | absent | — | **absent** |
| F8 movie capture | absent | — | n/a | n/a | n/a | n/a | absent | — | **absent** |
| F9 screenshot | absent | — | n/a | n/a | n/a | n/a | absent | — | **absent** |
| F10 | SDLK_F10 bound (GameScene.cpp:1241) — RWE debug window | n/a | n/a | n/a | n/a | n/a | n/a | — | **connected** (debug aid; human-confirmed via Fn+F10) |
| F11 | SDLK_F11 bound (SceneManager.cpp:123) — Global Debug | n/a | n/a | n/a | n/a | n/a | n/a | — | **connected** (matches TA debug-toggle role) |
| Reclaim | `e` quickkey on ARMRECLAIM/CORRECLAIM fires ActivateMessage → `onMessage` ignores | button instantiated from ARMGEN.GUI, unwired | `cursorreclamate` in CURSORS.GAF, NOT loaded (no `CursorType::Reclaim`) | absent | absent | absent | absent | `reclaimable`, `autoreclaimable`, `metal`, `energy` on FeatureDefinition — parsed, shown only in hover M:/E: text (GameScene.cpp:481-493) | **absent** (UI data present, everything below it missing) |
| D-Gun | `d` quickkey on ARMBLAST/CORBLAST fires ActivateMessage → ignored | button instantiated, unwired | `cursorairstrike` in CURSORS.GAF, NOT loaded | absent | absent | absent | weapons[2] never targeted: `attackTarget` loops `i < 2` (UnitBehaviorService.cpp:904); `commandFire` only suppresses auto-acquire (:347); `tryFireWeapon` itself is slot-generic | `commandfire` flag, FBI `Weapon3`, ARM_DISINTEGRATOR weapon TDF — parsed | **absent** (fire machinery exists per-slot but no path reaches weapon 3; per-shot `energycost` also not consumed — see §4) |
| Repair | `r` quickkey fires ActivateMessage → `onMessage` → `RepairCursorMode` | button wired; contextual repair when a builder is selected over a damaged friendly | `CursorType::Repair` + contextual precedence | `RepairOrder` | `RepairOrder` serialized | `UnitBehaviorStateRepairing` | `repairExistingUnit` — navigate → shared `inWorkingRange` (footprint-aware) → stance → HP restore + proportional resource drain | `repair` sound emitted on order issue | **connected** (Phase 4, human-QA'd) |
| Patrol | `p` quickkey on ARMPATROL/CORPATROL fires ActivateMessage → ignored | button instantiated, unwired | `cursorpatrol` in CURSORS.GAF, NOT loaded | absent | absent | absent | absent | — | **absent** (button + cursor data only) |

Other unwired ARMGEN orders buttons discovered incidentally (same gap
pattern, out of this audit's scope): `ARMMOVEORD` (v — hold-pos/maneuver/
roam toggle), `ARMCAPTURE` (c), `ARMLOAD` (l), `ARMCLOAK` (k),
`ARMUNLOAD` (u). Cursors for all exist in CURSORS.GAF but are not loaded.

## 2. Repair — layer trace

`CompleteBuildOrder` is **not** TA repair and must not be conflated:

- UI entry today: left-click on a *friendly* `isBeingBuilt` unit issues
  `CompleteBuildOrder` (GameScene.cpp:1453-1463). There is no Repair
  cursor mode; `CursorType::Repair` is shown only in that same context.
- `handleCompleteBuildOrder` → `buildExistingUnit` → returns early
  (`changeState` to Idle, order completes) unless
  `target.isBeingBuilt(def)` — i.e. `buildTimeCompleted < buildTime`
  (UnitState.cpp:109-112). A completed unit always fails this gate, so
  repair of a damaged-but-finished unit is **impossible today**.
- `deployBuildArm` repeats the same `isBeingBuilt` gate (:1381) and then
  drives the existing build mechanics: in-range check → `inBuildStance`
  wait → `getBuildCostInfo`/`addResourceDelta` resource drain →
  build progress. The *machinery* (navigate to target, deploy nanolathe,
  StartBuilding/StopBuilding COB threads, resource drain, piece update)
  is exactly what TA repair needs.
- Original TA repair semantics: builders nanolathe a damaged friendly
  unit and restore hit points over time while consuming resources — same
  mechanics family as construction, different completion condition
  (HP → max) and different cost basis (damage proportion, not remaining
  build time). **Do not make damaged units satisfy `isBeingBuilt()`** —
  keep a distinct `RepairOrder`/repair path that shares the infra.
- Repair sound (`UnitSoundType::Repair` → `c.repair`): parsed from
  ALLSOUND.TDF, preloaded (LoadingScene.cpp:588), mapped
  (GameScene.cpp:2130), but no call site ever passes `Repair` — hook it
  up when repair order issuance lands.

Missing for TA-compatible repair:
`RepairOrder` UnitOrder + proto field + serialization, `RepairCursorMode`
(target selection), REPAIR button wiring, repair behavior branch
(accept friendly unit with `health < maxHealth && !isBeingBuilt()`),
HP restoration + resource-cost semantics, repair sound emission.

**Phase 4 addendum — implemented and human-verified.** All of the above
now exists: `RepairOrder` (proto + serialization round-trip),
`RepairCursorMode`, REPAIR button + `r` quickkey wiring,
`UnitBehaviorStateRepairing` (navigate → shared footprint-aware
`inWorkingRange` → build stance → HP restore at proportional resource
cost → complete at full HP), repair sound, self-target rejection, and
guard-assist for a target already being repaired. Contextual repair:
builder selected + damaged friendly completed target yields the Repair
cursor/click before the Select branch (units remain selectable);
under-construction targets keep CompleteBuild/assist. Human QA PASS:
commander/T1/T2 constructors on damaged buildings and mobile units,
contextual + explicit activation, full-HP completion. Two latent defects
found and fixed at source: `buildExistingUnit` evaluated
`isBeingBuilt` with the builder's definition (broke ordinary-builder
construction resumption), and center-distance range checks (broke T1
repair of large buildings) — both now regression-tested.

## 3. Patrol — bottom-up audit

- `PatrolOrder` or equivalent: **absent** — `UnitOrder` variant is
  {Move, Attack, Build, BuggerOff, CompleteBuild, Guard}.
- Patrol UI: `ARMPATROL` gadget exists in ARMGEN.GUI (quickkey `p`),
  instantiated but its ActivateMessage is ignored.
- P hotkey: reaches the button via the GUI quickkey system
  (`UiStagedButton::keyDown` matches `quickKey`), then dies in
  `onMessage` — the hotkey *delivery* path works end-to-end.
- Order queue: `std::deque<UnitOrder> orders` on UnitState; front order
  runs via `handleOrder` each tick, `pop_front()` on completion;
  `IssueKind::Queued` (shift) appends. Infrastructure is reusable —
  patrol needs *looping* semantics (re-queue nodes instead of popping).
- Movement between nodes: `MoveOrder`/`navigateTo`/`followPath` reusable.
- Looping behavior: absent — nothing re-cycles a completed order.
- Engagement while patrolling: fire-at-will auto-acquire exists in
  `updateWeapon`; TA semantics (engage, then *resume* the patrol route)
  require the patrol state to persist alongside combat — new logic.
- Builder patrol: TA-era behavior (whether cons on patrol auto-assist/
  repair nearby) is **unverified against the original** — must be checked
  before implementing; do not guess.
- Network: proto `IssueOrder` oneof lacks a patrol entry; needs a field
  (e.g. `PatrolOrder patrol = 7;`) + read/write visitors.
- Reclaim-adjacent note: `cursorpatrol` exists in data but isn't loaded;
  a `CursorType::Patrol` + load line in main.cpp is needed.

Do not force Patrol through `MoveOrder`: a patrol is a persistent
*route* (node list + loop), not a destination. What genuinely needs
adding: `PatrolOrder` order type carrying waypoints, patrol state on the
unit (current node, direction), completion-requeue behavior, patrol
cursor mode + button wiring, proto serialization, and engagement-resume
rules.

## 4. D-Gun — trace

- Real-data confirmation: `ARMCOM.FBI` has `Weapon1=ARMCOMLASER`,
  `Weapon3=ARM_DISINTEGRATOR` — the D-Gun is weapon slot index 2
  (`std::array<optional<UnitWeapon>,3> weapons`, UnitState.h:261).
- `attackTarget` drives only `weapons[0]`/`weapons[1]` (`i < 2`,
  UnitBehaviorService.cpp:904); `commandFire` suppresses idle-state
  auto-acquire (:347). Result: **no path ever assigns a target to
  weapon[2]** — D-Gun can neither auto-fire nor be ordered.
- `updateWeapon` does run for all 3 slots (:157-159) and `tryFireWeapon`
  is slot-generic, so the firing machinery itself would work *if* a
  target/state were set — implemented but unreachable.
- `ARMBLAST` button (`d`) exists, instantiated, unwired.
- Additional gap relevant to D-Gun: `tryFireWeapon` deducts **no
  energy** per shot — TA's disintegrator has a large `energycost` that
  gates firing; weapon energy-cost-on-fire is unimplemented for all
  weapons (would matter to TA-accurate D-Gun and energy weapons in
  general).
- TA D-Gun semantics: player manually aims at a point/unit and fires one
  disintegration shot; needs an aim-cursor mode, a target-assignment
  path for commandFire weapons, and energycost consumption.

## 5. Reclaim — trace

- Data present: `FeatureDefinition.{reclaimable, autoreclaimable, metal,
  energy}` parsed (LoadingScene.cpp:514-515); hover text shows the
  feature's M/E value — the only place reclaimability surfaces.
- No `ReclaimOrder`, no reclaim cursor mode, no `CursorType::Reclaim`
  (though `cursorreclamate` exists in CURSORS.GAF), no proto field, no
  `UnitBehaviorService` path, `ARMRECLAIM` (`e`) button unwired.
- TA semantics: builders nanolathe features/wrecks, draining the
  feature's metal/energy into the player stockpile over time — i.e.
  resource *income* through the same build-arm mechanics. Target domain
  is features/corpses, not units.

## 6. F-keys — macOS Fn vs RWE bindings

Fn+F10 demonstrably delivers `SDLK_F10` (debug window opened — human
confirmed). Therefore **no F-key failure should be attributed to
macOS**. F1–F9 simply have no RWE bindings:

| Key | TA semantic (manual) | RWE binding |
|---|---|---|
| F1 | unit info panel | none — `UNITINFO.GUI` exists in data, never loaded |
| F2 | in-game options menu | none — `gameoptions.GUI` in data |
| F3 | jump to last-reporting unit | none — no last-report tracking exists |
| F4 | kills/losses scores | none — `REPORT.GUI` in data |
| F5–F7 | map bookmarks | none |
| F8 | movie capture | none |
| F9 | screenshot to SHOT####.pcx | none |
| F10 | (RWE debug window) | bound — GameScene.cpp:1241 |
| F11 | debug toggle (TA) / Global Debug (RWE) | bound — SceneManager.cpp:123 |

## 7. Dependency-aware implementation sequence

Shared spine every order needs: GUI button wiring (`onMessage` +
`matchesWithSidePrefix`) → cursor mode → `PlayerUnitCommand::IssueOrder`
→ proto field + serialization visitors → `UnitOrder` variant →
`UnitBehaviorService` handler.

1. **Repair** — ~~most shared infrastructure, most gameplay-critical:
   nanolathe pipeline (navigate→stance→deployBuildArm→resource drain)
   exists end-to-end; needs a `RepairOrder` + damaged-unit acceptance +
   HP-restore semantics + cursor mode + button wiring + sound hookup.
   Establishes the "targeted support order" pattern.~~ **DONE (Phase 4).**
2. **Reclaim** — same pattern as Repair (cursor mode → targeted order →
   nanolathe) applied to the feature domain; adds resource *income* and
   the `cursorreclamate` load. Builds directly on the Repair-established
   pattern.
3. **D-Gun** — narrow but touches the weapon-targeting model
   (commandFire path for weapon[2], manual aim-at-point order, energycost
   on fire). Self-contained; could swap with Reclaim if
   energy-cost-on-fire is split out first.
4. **Patrol** — largest new machinery: looping order semantics, route
   persistence through combat, waypoint path display. Reuses move/
   navigate/queue infra but is the biggest behavioral surface.
5. **F-keys** — independent UX-parity track, lowest priority; each key is
   a self-contained UI/dialog feature (F1 unit-info panel, F2 options,
   F4 scores are the most visible). macOS Fn is not a factor.

Do not implement multiple orders in one phase — each warrants its own
focused phase with regression coverage.

## 8. Nanolathe visual fidelity (cross-cutting, non-command)

Human-observed (Phase 4 QA): RWE draws the nanolathe as a single solid
green line (`drawNanoLine`, `src/rwe/game/GameScene_util.cpp:677` →
`pushLine(..., Vector3f(0,1,0))`). Original TA renders a spray/stream of
small green particles. Pre-existing upstream simplification shared by
Build, CompleteBuild, Repair, and future Reclaim — all paths funnel
through `UnitState::getActiveNanolatheTarget` → one draw call site
(`src/rwe/game/GameScene.cpp:841`). Repair functionally activates the
existing effect correctly. Tracked as backlog P4-7; a future shared
particle-spray implementation can reuse the existing `Particle`/quad
batch infrastructure.
