# RWE macOS Port — Project Decisions & Constraints

Recorded at Phase 0 baseline (2026-09-16, upstream `b2d8a314b14142d8873400160768777532b1dcab`).

## Compatibility target

- **Original TA behavior is the compatibility target.** The goal is the
  original Total Annihilation experience, faithfully reproduced, running
  natively on modern Apple Silicon macOS.
- **RWE's existing implementation is the starting implementation.** Upstream
  `MHeasell/rwe` is authoritative; deviations require justification.
- **Behavioral modernization is not automatically an improvement.** Changes
  that alter gameplay, simulation, COB execution, pathfinding, or rendering
  output are regressions by default, even if they look like cleanups.

## Platform constraints

- **Native arm64 is preferred over Rosetta.** Never satisfy a dependency via
  x86_64 just to make the build pass. (On the audit machine Rosetta is not
  installed, so this is enforced by the environment.)
- **Original copyrighted game assets must stay outside Git.** No TA data
  files (`.hpi`, `.ufo`, `.ccx`, `.gp3`, `rev31.gp3`, etc.) are ever
  committed; audits must not crawl personal directories for them either.

## Non-goals for the initial compatibility milestone

- **OpenGL → Metal is NOT part of the initial milestone.** Evidence: the
  startup probe obtained a 4.1 Metal-backed core-profile OpenGL context —
  the existing renderer path works.
- **SDL2 → SDL3 is NOT part of the initial milestone.** (Moot — upstream
  already completed the SDL3 migration; nothing to do.)
- No engine redesign, no renderer rewrites, no dependency upgrades "because
  newer exists".

## Working rules

- **Fix one compatibility layer at a time.** Build → start → load data →
  gameplay → packaging. Don't conflate stages.
- **Every later behavioral change must have evidence** — a failing test, a
  logged divergence, a disassembly, or a documented platform contract.
  Speculation is recorded as POTENTIAL ISSUE, never treated as fact.
- Tests encode current (x86-validated) behavior; **never edit test
  expectations to silence failures** — fix the engine's reliance on UB
  instead, preserving the semantics tests assert.
- Source of truth for this port's status: `docs/macos27/`.
