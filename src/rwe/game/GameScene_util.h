#pragma once

#include <deque>
#include <random>
#include <rwe/CursorService.h>
#include <rwe/SkirmishSettings.h>
#include <rwe/RenderService.h>
#include <rwe/collections/VectorMap.h>
#include <rwe/game/GameMediaDatabase.h>
#include <rwe/game/GameScene.h>
#include <rwe/game/Particle.h>
#include <rwe/game/PlayerColorIndex.h>
#include <rwe/math/Matrix4x.h>
#include <rwe/pathfinding/AStarPathFinder.h>
#include <rwe/pathfinding/PathCost.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/MapTerrain.h>
#include <rwe/sim/OccupiedGrid.h>
#include <rwe/sim/Projectile.h>
#include <rwe/sim/ProjectileId.h>
#include <rwe/sim/SimScalar.h>
#include <rwe/sim/UnitDefinition.h>
#include <rwe/sim/UnitMesh.h>
#include <rwe/sim/UnitModelDefinition.h>
#include <rwe/sim/UnitPieceDefinition.h>
#include <rwe/sim/UnitState.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rwe
{
    void
    drawPathfindingVisualisation(const MapTerrain& terrain, const AStarPathInfo<Point, PathCost>& pathInfo, ColoredMeshBatch& batch);

    void
    drawTerrainArrow(const MapTerrain& terrain, const Point& start, const Point& end, const Color& color, ColoredMeshBatch& batch);

    void drawOccupiedGrid(const Vector3f& cameraPosition, float viewportWidth, float viewportHeight, const MapTerrain& terrain, const OccupiedGrid& occupiedGrid, ColoredMeshBatch& batch);

    /**
     * Fills `unexploredBatch` with opaque dark quads over heightmap
     * cells the local player has never seen and `exploredBatch` with
     * translucent dark quads over cells seen before but not currently
     * in LOS (retail `mapping=1` dimming).
     */
    void fillFogOfWarBatches(const Vector3f& cameraPosition, float viewportWidth, float viewportHeight, const MapTerrain& terrain, const PlayerVisibility& visibility, ColoredMeshBatch& unexploredBatch, ColoredMeshBatch& exploredBatch);

    /**
     * Appends coalesced minimap fog quads in minimap pixel space.
     * Horizontal runs of unexplored cells go to `unexploredVerts` and
     * runs of explored-but-not-visible cells go to `exploredVerts`;
     * cells currently in LOS emit nothing. Caller draws the two
     * vertex lists with the appropriate alpha.
     */
    void fillMinimapFogVertices(const Grid<unsigned char>& losGrid, const Grid<unsigned char>& exploredGrid, const Rectangle2f& rect, std::vector<GlColoredVertex>& unexploredVerts, std::vector<GlColoredVertex>& exploredVerts);

    void drawMovementClassCollisionGrid(const MapTerrain& terrain, const Grid<char>& movementClassGrid, const Vector3f& cameraPosition, float viewportWidth, float viewportHeight, ColoredMeshBatch& batch);

    /**
     * The cursor sprite type drawn at an order's map position, if any.
     * Orders without a marker return std::nullopt.
     */
    std::optional<CursorType> orderMarkerCursorType(const UnitOrder& order);

    /**
     * The map position an order's marker is drawn at.
     * Orders without a meaningful position (or whose target can no
     * longer be resolved) return `fallbackPos`.
     */
    SimVector orderMarkerPosition(const GameSimulation& simulation, const UnitOrder& order, const SimVector& fallbackPos);

    /**
     * Fixed UI-space click slop applied around a rendered order
     * marker when hit-testing queued-order removal (shift+click).
     * The marker's rendered rect is expanded by this many UI pixels
     * on every side; the marker center stays tied to the sprite.
     */
    constexpr float OrderMarkerHitSlop = 8.0f;

    /**
     * Returns true when `uiMousePos` lies inside the marker sprite's
     * rendered rect anchored at `uiMarkerPos`, expanded by `hitSlop`
     * UI pixels on every side.
     */
    bool orderMarkerContainsPoint(const Rectangle2f& spriteBounds, const Vector2f& uiMarkerPos, const Vector2f& uiMousePos, float hitSlop);

    /**
     * Returns the index of the point in `uiPoints` nearest to
     * `uiMousePos`, or std::nullopt when the list is empty.
     * Ties keep the earliest entry (stable deterministic choice).
     */
    std::optional<std::size_t> nearestUiPoint(const std::vector<Vector2f>& uiPoints, const Vector2f& uiMousePos);

    /**
     * RWE QoL extension (TA 3.9/Escalation-style linear build, NOT
     * stock TA 3.1): generates the snapped top-left footprint cells
     * for a build-drag line from `anchorTopLeftCell` to
     * `cursorTopLeftCell`.
     *
     * The line is axis-aligned on the dominant axis, chosen by
     * approximate placement count (|dx|/footprintW vs
     * |dz|/footprintH) so long footprints don't bias the gesture;
     * an exact tie picks horizontal. Cells step by the footprint
     * size on that axis so buildings abut edge-to-edge, starting at
     * the anchor and stopping at the last step that does not pass
     * the cursor. Deterministic, no duplicates, anchor included.
     */
    std::vector<Point> linearBuildCells(const Point& anchorTopLeftCell, const Point& cursorTopLeftCell, unsigned int footprintW, unsigned int footprintH);

    /**
     * True when `orders` already contains a BuildOrder for `unitType`
     * at the same snapped build position. Used to suppress duplicate
     * sites when a linear-build drag overlaps existing queue entries.
     */
    bool queuedBuildSiteExists(const std::deque<UnitOrder>& orders, const std::string& unitType, const SimVector& position);

    /**
     * The row set retail draws into the in-game GAMEOPTIONS
     * ("GameSettings") dialog for non-multiplayer games (TotalA.exe
     * builder at 0x45f1d0). Labels render at x=18 width 110, values at
     * x=140 width 120, rows start at y=90 stepping 18. Commander
     * death, starting locations and difficulty report the game's
     * actual skirmish settings; mapping/LOS report the live
     * simulation flags. Max Units reports the live per-player cap
     * (retail UnitLimit/`maxunits`, default 250, clamped [20,500]).
     */
    std::vector<std::pair<std::string, std::string>> makeGameOptionsRows(
        const std::string& mapName,
        SkirmishCommanderDeath commanderDeath,
        SkirmishStartLocation startLocation,
        bool mappingEnabled,
        int lineOfSightMode,
        SkirmishDifficulty difficulty,
        unsigned int startingMetal,
        unsigned int startingEnergy,
        unsigned int maxUnits);

    /**
     * Formats one page of the stock HELP.TDF [Help] block for the
     * in-game help dialog (dhelp/HELP.GUI). `raw` is the file text,
     * `page` is 0-based, `pageCount` matches the dialog's Page staged
     * button (stock: 3). Entries are "key|description" pairs, rendered
     * one per line as "key -- description"; blank entries produce a
     * blank line. Pages split the line list evenly.
     */
    std::string inGameHelpText(const std::string& raw, unsigned int page, unsigned int pageCount);

    /**
     * Appends one line segment (two white vertices, GL_LINES winding)
     * to `vertices` -- the same geometry UiRenderService::drawLine
     * emits, accumulated so a frame can stream all segments in one mesh.
     */
    void appendUiLineVertices(std::vector<GlColoredVertex>& vertices, const Vector2f& start, const Vector2f& end);

    /**
     * Appends the filled quad geometry of UiRenderService::drawBoxOutline
     * to `vertices` (four edge quads, or one solid quad when the box is
     * thinner than twice the thickness). Colors become per-vertex RGB;
     * the caller is responsible for a shared alpha at draw time.
     */
    void appendUiBoxOutlineVertices(std::vector<GlColoredVertex>& vertices, float x, float y, float width, float height, const Color& color, float thickness);

    /**
     * `alpha` below 1 renders the unit translucent -- the owner's
     * effectively cloaked units (retail's ghost blit variant at
     * 0x459779). Enemy-cloaked units never reach this draw: the
     * caller's `isLosVisibleToPlayer` gate removes them.
     */
    void drawUnit(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const UnitState& unit,
        const UnitDefinition& unitDefinition,
        const UnitModelDefinition& modelDefinition,
        PlayerColorIndex playerColorIndex,
        float frac,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitMeshBatch& batch,
        float alpha = 1.0f);

    /**
     * Draws a carried unit under its carrier's attach transform:
     * the slot piece's world matrix when `attachSlot` names a valid
     * script piece, else the carrier's own transform (retail parks
     * slot -1 members at the carrier origin, 0x43def0).
     */
    void drawAttachedUnit(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const UnitState& unit,
        const UnitDefinition& unitDefinition,
        const UnitModelDefinition& modelDefinition,
        const UnitState& carrier,
        const UnitModelDefinition& carrierModelDefinition,
        int attachSlot,
        PlayerColorIndex playerColorIndex,
        float frac,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitMeshBatch& batch,
        float alpha = 1.0f);

    void drawMeshFeature(
        const std::unordered_map<std::string, UnitModelDefinition>& modelDefinitions,
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const MapFeature& feature,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitMeshBatch& batch);

    void drawUnitShadow(
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const UnitState& unit,
        const UnitDefinition& unitDefinition,
        const UnitModelDefinition& modelDefinition,
        float frac,
        float groundHeight,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitShadowMeshBatch& batch);

    void drawFeatureMeshShadow(
        const std::unordered_map<std::string, UnitModelDefinition>& modelDefinitions,
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const MapFeature& feature,
        float groundHeight,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        UnitShadowMeshBatch& batch);

    float featureEventCullRadius(const GameMediaDatabase& media, const MapFeature& feature);

    void drawFeature(
        const GameMediaDatabase& gameMediaDatabase,
        const MapFeature& feature,
        const FeatureDefinition& featureDefinition,
        const Matrix4f& viewProjectionMatrix,
        SpriteBatch& batch);
    void drawFeatureShadow(
        const GameMediaDatabase& gameMediaDatabase,
        const MapFeature& feature,
        const FeatureDefinition& featureDefinition,
        const Matrix4f& viewProjectionMatrix,
        SpriteBatch& batch);

    /**
     * Spawns the per-tick nanolathe burst for every unit with an
     * active nanolathe target (build, assist, repair, reclaim,
     * capture, factory) -- the retail effect-channel-6 behavior
     * (TotalA.exe emitter at 0x4720d0, invoked once per work tick).
     *
     * Each of the five particles per tick spawns exactly at the
     * QueryNanoPiece nozzle -- the retail emit bound is the degenerate
     * box [FROM, FROM] (TotalA.exe 0x4720d0) -- and converges on its
     * own random point inside the central 3/11 band of the target's
     * bound box (unitdef +0x15e..+0x172 / feature +0x94/+0x96/+0xfa
     * bounds, shrunk per-axis in TotalA.exe 0x473b50). A particle
     * lives trunc(distance/4) ticks, dies one tick past its deadline,
     * and cycles the 0xA1-0xA7 palette ramp each tick.
     *
     * Presentation-only: `rng` must be a presentation RNG, never the
     * simulation's. Emission itself is not LOS-gated; retail tests the
     * visibility grid per particle at draw time.
     */
    void emitNanolatheParticles(const GameSimulation& simulation, GameTime currentTime, std::vector<Particle>& particles, std::mt19937& rng);

    /**
     * Appends one nanolathe particle to `batch` as a world-space
     * XZ-flat square colored from the retail palette ramp
     * (0xA1-0xA7). Retail blits an inclusive (x,y)-(x+1,y+1) screen
     * rect -- a fixed 2x2 screen-pixel square; `worldHalfExtent`
     * must be the world size of one screen pixel
     * (camera.scaleDimension(1.0f)) so the quad projects to 2x2 px
     * at any zoom. Per retail, the particle is skipped when its own
     * position falls outside the local player's LOS grid.
     */
    void drawNanolatheParticle(const GameSimulation& simulation, PlayerId localPlayerId, float worldHalfExtent, const Particle& particle, ColoredMeshBatch& batch);

    void drawSpriteParticle(const GameMediaDatabase& gameMediaDatabase, GameTime currentTime, const Matrix4f& viewProjectionMatrix, const Particle& particle, SpriteBatch& batch);

    void drawWakeParticle(const GameMediaDatabase& gameMediaDatabase, GameTime currentTime, const Matrix4f& viewProjectionMatrix, const Particle& particle, ColoredMeshBatch& batch);

    void updateParticles(const GameMediaDatabase& gameMediaDatabase, GameTime currentTime, std::vector<Particle>& particles);

    void drawProjectiles(
        const GameSimulation& sim,
        const GameMediaDatabase& gameMediaDatabase,
        const Matrix4f& viewProjectionMatrix,
        const VectorMap<Projectile, ProjectileIdTag>& projectiles,
        GameTime currentTime,
        float frac,
        TextureIdentifier unitTextureAtlas,
        std::vector<SharedTextureHandle>& unitTeamTextureAtlases,
        ColoredMeshBatch& coloredMeshbatch,
        SpriteBatch& spriteBatch,
        UnitMeshBatch& unitMeshBatch);

    void drawSelectionRect(const GameMediaDatabase& gameMediaDatabase, const Matrix4f& viewProjectionMatrix, const UnitState& unit, const UnitDefinition& unitDefinition, float frac, ColoredMeshesBatch& batch);

    /**
     * Appends the `mincloakdistance` decloak ring for a selected
     * cloaked unit (retail 0x4390e2 draws the radius circle whenever
     * the unit is effectively cloaked): a flat GL_LINES circle of
     * `radius` world units centred on the interpolated unit position.
     */
    void appendDecloakRingVertices(std::vector<GlColoredVertex>& vertices, const UnitState& unit, float radius, float frac);

    /**
     * Appends a screen-space circle (GL_LINES, 32 segments) centred on
     * `center` -- the geometry retail draws at TotalA.exe 0x4c0070 for
     * the selected-unit radar/sonar/jam range rings on the minimap
     * (radius already converted to minimap pixels by the caller).
     */
    void appendMinimapSensorCircleVertices(std::vector<GlColoredVertex>& vertices, const Vector2f& center, float radius, const Vector3f& color);

    class UiPanel;

    /**
     * The orders-panel capability mask for the current selection.
     * Retail aggregates each flag by OR across all selected units
     * (TotalA.exe 0x41b2e0 reads each unit's unitdef flag dword at
     * +0x245 and ORs into the mask at +0x37ec0).
     */
    struct OrdersPanelCapabilities
    {
        bool canMove{false};
        bool canStop{false};
        bool canAttack{false};
        bool canGuard{false};
        bool canPatrol{false};
        bool canReclaim{false};
        bool canCapture{false};
        bool canLoad{false};

        /**
         * Drives the BLAST gadget: true when a selected unit is FBI
         * `candgun` or carries a `commandfire=1` weapon (the
         * stockpile-silo path into the same command).
         */
        bool canDGun{false};
        bool canCloak{false};
    };

    OrdersPanelCapabilities aggregateOrdersPanelCapabilities(
        const std::vector<const UnitDefinition*>& selectedDefinitions,
        const std::unordered_map<std::string, WeaponDefinition>& weaponDefinitions = {});

    /**
     * The retail CLOAK strip stage for the current selection
     * (TotalA.exe 0x41b2e0 aggregation, mask bits 3:4):
     * 3 when no selected unit can cloak (gadget deactivated),
     * otherwise the first cloak-capable unit's requested state
     * (0 off / 1 on) -- a second capable unit latches 2, so any
     * multi-selection of cloakables displays the mixed stage
     * regardless of whether they agree.
     */
    unsigned int cloakPanelStage(const GameSimulation& sim, const std::unordered_set<UnitId>& selectedUnits);

    /**
     * Applies the retail orders-panel exposure rules (TotalA.exe
     * 0x41a130-0x41a48d) to the gadgets of a `*GEN` panel:
     * MOVE/STOP/ATTACK/DEFEND/PATROL/RECLAIM/REPAIR/CAPTURE/UNLOAD are
     * greyed out when no selected unit supports them; LOAD is hidden
     * unless a selected unit can load, in which case the BLAST gadget
     * sharing its slot is hidden instead. Gadgets absent from the
     * panel are skipped.
     */
    void applyOrdersPanelCapabilities(UiPanel& panel, const std::string& sidePrefix, const OrdersPanelCapabilities& caps);

    /**
     * The campaign result-commit gate used by GameScene's per-tick
     * result check. Retail commits `+0x3923b` once when the debounced
     * evaluator output stabilises, then runs the igvictory/igdefeat
     * banner → ENDMSN chain exactly once; the in-scene latch
     * (`missionResultDeclared`) plays the same role.
     *
     * Returns the outcome to declare on the first stable Victory or
     * Defeat; `None` while the mission is still InProgress and on
     * every subsequent tick after declaration, so the transition can
     * never fire twice.
     */
    enum class MissionEndAction
    {
        None,
        DeclareVictory,
        DeclareDefeat,
    };

    MissionEndAction missionEndAction(MissionResult result, bool alreadyDeclared);

    /**
     * Whether any weapon slot on the definition names a stockpile
     * weapon (weapon TDF `stockpile=1` -- missile silos, anti-nukes,
     * EMP/TAC missiles).
     */
    bool unitDefinitionHasStockpileWeapon(const GameSimulation& simulation, const UnitDefinition& unitDefinition);

    /**
     * Whether any weapon slot on the definition names a command-fire
     * weapon (weapon TDF `commandfire=1`) -- the stockpile-silo route
     * into the retail BLAST order alongside FBI `candgun`.
     */
    bool unitDefinitionHasCommandFireWeapon(const std::unordered_map<std::string, WeaponDefinition>& weaponDefinitions, const UnitDefinition& unitDefinition);

    /**
     * The sim effect of a `PlayerUnitCommand::ModifyWeaponQueue`
     * message (the GUI `commonattribs=8` stockpile button): positive
     * counts append rounds to the unit's BuildWeaponOrder background
     * order, merging onto an existing one rather than stacking;
     * negative counts cancel pending rounds, removing the order when
     * it drains to zero. Stockpile production runs in
     * `backgroundOrders`, in parallel with the foreground queue.
     */
    void applyModifyWeaponQueue(UnitState& unit, int count);

    // Authoritative effects shared by GameScene and headless command playback.
    // Admission (owner, visibility, build menu) belongs to the command producer;
    // GUI notifications remain in GameScene. Returns false for other commands.
    bool applyBasicPlayerUnitCommand(UnitState& unit, const PlayerUnitCommand::Command& command);

    /**
     * The initial frontend GUI state for a unit: builders and units
     * carrying a stockpile weapon open on the Build section (the
     * stockpile weapon button lives on the unit's build panel), all
     * others on Orders. Shared by the UnitSpawnedEvent handler and the
     * post-restore rebuild below.
     */
    UnitGuiInfo initialUnitGuiInfo(const UnitDefinition& unitDefinition, bool hasWeaponBuildPanel);

    /**
     * Registers a UnitGuiInfo for every live unit in the simulation.
     * Units spawned while GameScene runs maintain their entry via
     * UnitSpawnedEvent, but units restored by applySavedGame are
     * inserted into the simulation without emitting that event, so the
     * scene must rebuild this transient map from live state before any
     * selection/panel code can query it.
     */
    void rebuildUnitGuiInfos(const GameSimulation& simulation, std::unordered_map<UnitId, UnitGuiInfo>& out);
}
