#include "CampaignAi.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <unordered_set>

#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/movement.h>
#include <rwe/util/SimpleLogger.h>
#include <rwe/util/rwe_string.h>

namespace rwe
{
    namespace
    {
        /**
         * Retail task cadences (TotalA.exe task dispatcher 0x408c40):
         * repartition ~30t, factory ~30t, builder ~90t, attack ~300t.
         */
        constexpr unsigned int RepartitionPeriodTicks = 30;
        constexpr unsigned int FactoryScanPeriodTicks = 30;
        constexpr unsigned int BuilderScanPeriodTicks = 90;
        constexpr unsigned int AttackScanPeriodTicks = 300;

        /**
         * Retail attack squads muster until a minimum force is
         * available before dispatching.
         */
        constexpr unsigned int MinAttackSquadSize = 4;

        /** Builder construction-site spiral radius in heightmap cells. */
        constexpr int SiteSearchRadiusCells = 24;

        /**
         * Idle-builder roam radius in heightmap cells (retail's second
         * builder-task pass issues a random queued move).
         */
        constexpr int BuilderRoamRadiusCells = 8;

        /**
         * Deadlock recovery task cadence and how long sealed egress
         * must persist before the AI removes a blocker. The window is
         * deliberately long so temporary congestion and in-flight
         * traffic cannot trigger structural reclaim.
         */
        constexpr unsigned int DeadlockScanPeriodTicks = 300;
        constexpr unsigned int DeadlockConfirmTicks = 1800;
        // a reclaim order older than this with the blocker still
        // standing is considered stalled (partial path exhausted
        // out of working range, unreachable terrain, etc.)
        constexpr unsigned int DeadlockReclaimWatchdogTicks = 4500;
        // extra grace after confirm before the self-destruct last
        // resort may fire for a blocker no builder can reach
        constexpr unsigned int DeadlockLastResortTicks = 3000;
        constexpr unsigned int DeadlockSelfDestructTicks = 45;
        constexpr unsigned int DeadlockMaxReclaimAttempts = 2;

        /**
         * After a reclaim stall burns the attempt budget, this long the
         * factory waits before the budget refreshes: the verdict
         * "this reclaim path cannot work" is only true for the base
         * geometry that produced it, and a busy AI base keeps changing
         * under the deadlock.
         */
        constexpr unsigned int DeadlockReclaimCooldownTicks = 9000;
        // bugger-off shoves issued per parked mobile before the unit
        // is treated as nav-immobile and becomes a reclaim candidate
        constexpr unsigned int DeadlockEvictMaxAttempts = 3;

        /**
         * Egress probe bounds: a produced unit must be able to reach
         * this far from the factory footprint before the yard counts
         * as open; the flood fill is capped so a pathological pocket
         * cannot scan the whole map.
         */
        constexpr int EgressEscapeRadiusCells = 24;
        constexpr unsigned int EgressVisitCapCells = 8192;

        int unitIndexOf(const CampaignAiPlayerState& st, const std::string& upperName)
        {
            auto it = std::lower_bound(st.unitIndex.begin(), st.unitIndex.end(), upperName);
            if (it != st.unitIndex.end() && *it == upperName)
            {
                return static_cast<int>(it - st.unitIndex.begin());
            }
            return -1;
        }

        /**
         * Retail resolver 0x488d30: an exact unit name selects just
         * that unit; otherwise the token is matched against FBI
         * Category tags.
         */
        std::vector<int> matchSelector(
            const GameSimulation& sim,
            const CampaignAiPlayerState& st,
            const std::string& selector)
        {
            std::vector<int> out;
            auto name = toUpper(selector);

            if (auto idx = unitIndexOf(st, name); idx >= 0)
            {
                out.push_back(idx);
                return out;
            }

            for (std::size_t i = 0; i < st.unitIndex.size(); ++i)
            {
                const auto& def = sim.unitDefinitions.at(st.unitIndex[i]);
                if (def.categories.find(name) != def.categories.end())
                {
                    out.push_back(static_cast<int>(i));
                }
            }
            return out;
        }

        void applyWeightDirectives(
            const GameSimulation& sim,
            CampaignAiPlayerState& st,
            const std::vector<CampaignAiWeightDirective>& directives,
            std::vector<bool>& locked)
        {
            for (const auto& d : directives)
            {
                for (int i : matchSelector(sim, st, d.selector))
                {
                    if (locked[i])
                    {
                        continue;
                    }
                    // retail: byte weight = clamp(int(old * mult), 0, 100)
                    int w = static_cast<int>(st.unitWeights[i] * d.multiplier);
                    st.unitWeights[i] = std::clamp(w, 0, 100);
                    locked[i] = true;
                }
            }
        }

        void applyLimitDirectives(
            const GameSimulation& sim,
            CampaignAiPlayerState& st,
            const std::vector<CampaignAiLimitDirective>& directives,
            std::vector<bool>& locked)
        {
            for (const auto& d : directives)
            {
                for (int i : matchSelector(sim, st, d.selector))
                {
                    if (locked[i])
                    {
                        continue;
                    }
                    st.unitLimits[i] = d.value;
                    locked[i] = true;
                }
            }
        }

        std::vector<int> countOwnedUnits(const GameSimulation& sim, const CampaignAiPlayerState& st)
        {
            std::vector<int> counts(st.unitIndex.size(), 0);
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.owner != st.player || unit.isDead() || unit.carriedBy)
                {
                    continue;
                }
                auto idx = unitIndexOf(st, toUpper(unit.unitType));
                if (idx >= 0)
                {
                    ++counts[idx];
                }
            }
            return counts;
        }

        const UnitState* liveMember(const GameSimulation& sim, const CampaignAiPlayerState& st, UnitId id)
        {
            auto unit = sim.tryGetUnitState(id);
            if (!unit || !unit->get().isAlive() || unit->get().owner != st.player || unit->get().carriedBy)
            {
                return nullptr;
            }
            return &unit->get();
        }

        bool isCombatUnit(const UnitState& unit, const UnitDefinition& def)
        {
            return def.isMobile && !def.builder && def.canAttack;
        }

        /**
         * Retail 0x40bb00 scores candidates by a resource-demand
         * weight vector; the RWE version keeps the same intent —
         * profile weight dominates, resource stall boosts matching
         * producers, unaffordable units are damped.
         */
        int scoreCandidate(const GamePlayerInfo& player, const UnitDefinition& def, int weight)
        {
            int score = weight;
            bool makesEnergy = def.energyMake > Energy(0) || def.windGenerator > Energy(0) || def.tidalGenerator;
            bool makesMetal = def.metalMake > Metal(0) || def.extractsMetal > Metal(0) || def.makesMetal > Metal(0);
            if (makesEnergy && player.energyStalled)
            {
                score *= 3;
            }
            if (makesMetal && player.metalStalled)
            {
                score *= 3;
            }
            if (def.buildCostMetal > player.metal && def.buildCostEnergy > player.energy)
            {
                score /= 4;
            }
            return score;
        }

        /**
         * Retail product selection 0x40bdb0: reservoir-sample the
         * producer's own build list weighted by score; producers
         * cannot build themselves.
         */
        std::optional<std::string> pickProduct(
            GameSimulation& sim,
            CampaignAiPlayerState& st,
            const UnitState& producer,
            const UnitDefinition& producerDef,
            const std::vector<int>& counts)
        {
            if (producerDef.buildOptions.empty())
            {
                return std::nullopt;
            }

            const auto& player = sim.getPlayer(st.player);
            std::optional<std::string> pick;
            int total = 0;
            for (const auto& option : producerDef.buildOptions)
            {
                auto idx = unitIndexOf(st, option);
                if (idx < 0)
                {
                    continue;
                }
                if (option == toUpper(producer.unitType))
                {
                    continue;
                }
                if (st.unitWeights[idx] <= 0)
                {
                    continue;
                }
                if (st.unitLimits[idx] != -1 && counts[idx] >= st.unitLimits[idx])
                {
                    continue;
                }

                int score = scoreCandidate(player, sim.unitDefinitions.at(option), st.unitWeights[idx]);
                if (score <= 0)
                {
                    continue;
                }

                total += score;
                if (sim.rng() % total < static_cast<unsigned int>(score))
                {
                    pick = option;
                }
            }
            return pick;
        }

        /**
         * The movement profile of a factory's produced mobile units --
         * the first mobile build option stands in for the whole
         * production class.
         */
        std::optional<MovementClassDefinition> factoryProductMovementClass(const GameSimulation& sim, const UnitDefinition& factoryDef)
        {
            for (const auto& option : factoryDef.buildOptions)
            {
                auto it = sim.unitDefinitions.find(option);
                if (it == sim.unitDefinitions.end() || !it->second.isMobile)
                {
                    continue;
                }
                return sim.getAdHocMovementClass(it->second.movementCollisionInfo);
            }
            return std::nullopt;
        }


        void repartition(GameSimulation& sim, CampaignAiPlayerState& st)
        {
            for (auto& g : st.groups)
            {
                g.clear();
            }

            std::size_t combatCount = 0;
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.owner != st.player || unit.isDead() || unit.carriedBy)
                {
                    continue;
                }
                const auto& def = sim.unitDefinitions.at(toUpper(unit.unitType));
                if (unit.isBeingBuilt(def))
                {
                    continue;
                }
                if (unit.missionState && unit.missionState->aiIgnore)
                {
                    continue;
                }

                if (def.builder && !def.isMobile)
                {
                    st.groups[AiGroupFactory].push_back(id);
                }
                else if (def.onOffable)
                {
                    st.groups[AiGroupFactory].push_back(id);
                }
                else if (def.builder)
                {
                    st.groups[AiGroupBuilder].push_back(id);
                }
                else if (isCombatUnit(unit, def))
                {
                    st.groups[combatCount % 2 == 0 ? AiGroupReserveA : AiGroupReserveB].push_back(id);
                    ++combatCount;
                }
            }
        }

        void factoryScan(GameSimulation& sim, CampaignAiPlayerState& st)
        {
            auto counts = countOwnedUnits(sim, st);
            const auto& player = sim.getPlayer(st.player);
            bool stalled = player.energyStalled || player.metalStalled;

            for (UnitId id : st.groups[AiGroupFactory])
            {
                const auto* unit = liveMember(sim, st, id);
                if (!unit)
                {
                    continue;
                }
                const auto& def = sim.unitDefinitions.at(toUpper(unit->unitType));

                if (def.onOffable)
                {
                    if (stalled && unit->activated)
                    {
                        sim.deactivateUnit(id);
                    }
                    else if (!stalled && !unit->activated)
                    {
                        sim.activateUnit(id);
                    }
                }

                if (!def.builder || def.isMobile)
                {
                    continue;
                }

                // empty queue + no active order -> enqueue the next product
                if (!unit->orders.empty() || !unit->buildQueue.empty())
                {
                    continue;
                }

                if (auto pick = pickProduct(sim, st, *unit, def, counts))
                {
                    sim.getUnitState(id).buildQueue.emplace_back(*pick, 1);
                }
            }
        }

        void builderScan(GameSimulation& sim, CampaignAiPlayerState& st)
        {
            auto counts = countOwnedUnits(sim, st);

            for (UnitId id : st.groups[AiGroupBuilder])
            {
                const auto* unit = liveMember(sim, st, id);
                if (!unit)
                {
                    continue;
                }
                // a non-empty queue covers scripted InitialMission
                // orders and in-flight AI work alike
                if (!unit->orders.empty() || unit->aircraftService)
                {
                    continue;
                }

                const auto& def = sim.unitDefinitions.at(toUpper(unit->unitType));
                auto pick = pickProduct(sim, st, *unit, def, counts);
                if (pick)
                {
                    auto site = findBuildSite(sim, st, *unit, def, sim.unitDefinitions.at(*pick));
                    if (site)
                    {
                        sim.getUnitState(id).addOrder(MissionBuildOrder{*pick, *site});
                        continue;
                    }
                }

                // retail's second pass roams builders that found no work
                auto& mutableUnit = sim.getUnitState(id);
                int dx = static_cast<int>(sim.rng() % (2 * BuilderRoamRadiusCells + 1)) - BuilderRoamRadiusCells;
                int dz = static_cast<int>(sim.rng() % (2 * BuilderRoamRadiusCells + 1)) - BuilderRoamRadiusCells;
                auto cell = sim.terrain.worldToHeightmapCoordinateNearest(mutableUnit.position);
                auto target = sim.terrain.heightmapIndexToWorldCenter(cell.x + dx, cell.y + dz);
                mutableUnit.addOrder(MoveOrder(target));
            }
        }

        /**
         * Deadlock recovery (TA-TRANSPLANT ROBUSTNESS APPROXIMATION --
         * retail has no equivalent task). A factory whose yard exit
         * stays sealed for DeadlockConfirmTicks while a produced
         * mobile unit is trapped inside has a real base-layout
         * deadlock: pick the lowest-cost own, noncritical boundary
         * structure whose removal re-opens egress and reclaim it via
         * the normal builder reclaim path. Factories, commanders,
         * scripted/mission units and aiIgnore structures are never
         * eligible; mobile traffic and terrain never trigger.
         */
        /**
         * A factory whose build queue is nonempty but which is not
         * actively nanolathing a product is production-stalled: either
         * the yard cannot open (a script retries SET YARD_OPEN forever
         * against an occupant) or the nanoframe spawn point is blocked
         * and creation requests keep failing. A resource-starved build
         * still holds its target unit, so ordinary metal/energy waits
         * are correctly not flagged; Idle with a pending queue counts
         * as stalled because no progress is being made at all.
         */
        bool isFactoryProductionStalled(const UnitState& u)
        {
            if (u.buildQueue.empty())
            {
                return false;
            }
            if (const auto* b = std::get_if<FactoryBehaviorStateBuilding>(&u.factoryState))
            {
                return !b->targetUnit.has_value();
            }
            if (const auto* c = std::get_if<FactoryBehaviorStateCreatingUnit>(&u.factoryState))
            {
                return !std::holds_alternative<UnitCreationStatusDone>(c->status);
            }
            return true;
        }

        void deadlockScan(GameSimulation& sim, CampaignAiPlayerState& st)
        {
            // a factory that died while deadlocked drops out of the
            // group entirely and would never be revisited -- sweep the
            // bookkeeping here and release any builder still holding a
            // recovery order issued on its behalf
            for (auto it = st.deadlockedFactories.begin(); it != st.deadlockedFactories.end();)
            {
                auto factory = sim.tryGetUnitState(it->first);
                if (factory && factory->get().isAlive() && factory->get().owner == st.player)
                {
                    ++it;
                    continue;
                }
                auto& dl = it->second;
                if (dl.reclaimBuilder)
                {
                    if (auto builder = sim.tryGetUnitState(*dl.reclaimBuilder))
                    {
                        if (builder->get().isAlive() && builder->get().owner == st.player)
                        {
                            builder->get().clearOrders();
                        }
                    }
                }
                else if (dl.reclaimTarget)
                {
                    // pending self-destruct on a wall is pointless once
                    // the factory is gone -- cancel the countdown
                    if (auto target = sim.tryGetUnitState(*dl.reclaimTarget))
                    {
                        if (target->get().isAlive() && target->get().owner == st.player)
                        {
                            target->get().clearOrders();
                        }
                    }
                }
                it = st.deadlockedFactories.erase(it);
            }
            for (UnitId factoryId : st.groups[AiGroupFactory])
            {
                auto factory = sim.tryGetUnitState(factoryId);
                if (!factory || !factory->get().isAlive() || factory->get().owner != st.player)
                {
                    continue;
                }
                const auto& factoryDef = sim.unitDefinitions.at(toUpper(factory->get().unitType));
                if (!factoryDef.builder || factoryDef.isMobile)
                {
                    continue;
                }
                auto mc = factoryProductMovementClass(sim, factoryDef);
                if (!mc)
                {
                    continue;
                }

                auto factoryRect = sim.computeFootprintRegion(factory->get().position, factoryDef.movementCollisionInfo);
                auto probe = probeFactoryEgress(sim, factoryRect, *mc, nullptr, nullptr);

                // an open-egress factory can still be deadlocked when a
                // persistent obstruction inside the yard blocks
                // production -- no completed product exists to trip the
                // sealed-egress test, so a build queue that cannot
                // advance is the proof
                if (probe.open && !isFactoryProductionStalled(factory->get()))
                {
                    st.deadlockedFactories.erase(factoryId);
                    continue;
                }

                auto& dl = st.deadlockedFactories[factoryId];
                if (!dl.blockedSince)
                {
                    dl.blockedSince = sim.gameTime;
                    continue;
                }
                if (sim.gameTime - *dl.blockedSince < GameTime(DeadlockConfirmTicks))
                {
                    continue;
                }

                // an in-flight recovery order is left alone unless it
                // dies or stalls out of working range. This check must
                // run BEFORE the candidate-gathering branches below --
                // both can skip the scan entirely (a trapped mobile
                // that escaped, a yard that emptied), which would leave
                // a pinned reclaim unmonitored forever.
                if (dl.reclaimTarget)
                {
                    auto targetUnit = sim.tryGetUnitState(*dl.reclaimTarget);
                    bool targetStands = targetUnit && targetUnit->get().isAlive() && targetUnit->get().owner == st.player;
                    if (!targetStands)
                    {
                        // blocker gone -- the next scan's probe reads
                        // open and erases this entry. A completed
                        // reclaim is progress: reset the attempt
                        // budget so a pocket ringed by several walls
                        // can be opened one removal at a time; the
                        // cap remains for genuinely stalled attempts.
                        // reclaimAvoid intentionally persists: a
                        // builder that already failed to navigate this
                        // pocket has not gotten better at it just
                        // because another builder removed a wall.
                        dl.reclaimAttempts = 0;
                        dl.reclaimStalledAt = std::nullopt;
                        dl.reclaimTarget = std::nullopt;
                        dl.reclaimBuilder = std::nullopt;
                        dl.reclaimIssuedAt = std::nullopt;
                        dl.reclaimBestDistance = std::nullopt;
                        dl.reclaimDeadline = std::nullopt;
                    }
                    else if (!dl.reclaimBuilder)
                    {
                        // self-destruct last resort in flight
                        continue;
                    }
                    else
                    {
                        auto builder = sim.tryGetUnitState(*dl.reclaimBuilder);
                        bool builderLive = builder && builder->get().isAlive() && builder->get().owner == st.player;
                        bool stalled = false;
                        if (builderLive)
                        {
                            // the watchdog measures lack of approach,
                            // not elapsed time: a builder still closing
                            // on its target -- or one whose nanolathe
                            // is actually draining it -- is making
                            // progress however long the work takes.
                            // Only a builder that has stopped
                            // approaching for the whole window is
                            // convicted. The minimum approach delta
                            // keeps congestion jiggle (a pinned unit
                            // nudged back and forth by its neighbours)
                            // from masquerading as progress, and the
                            // distance-scaled hard deadline keeps a
                            // ratcheting jiggle from postponing the
                            // verdict forever. NB the Reclaiming
                            // behaviour state covers the APPROACH as
                            // well as the drain -- nanoParticleOrigin
                            // is what proves the beam is working.
                            bool beaming = false;
                            if (const auto* rs = std::get_if<UnitBehaviorStateReclaiming>(&builder->get().behaviourState))
                            {
                                beaming = rs->nanoParticleOrigin.has_value();
                            }
                            auto dist = builder->get().position.distance(targetUnit->get().position);
                            if (!dl.reclaimDeadline)
                            {
                                // even a heavily detouring approach
                                // beats a quarter world-unit per tick
                                dl.reclaimDeadline = sim.gameTime
                                    + GameTime(DeadlockReclaimWatchdogTicks)
                                    + GameTime(static_cast<long long>(simScalarToFloat(dist) * 4.0f));
                            }
                            if (beaming || !dl.reclaimBestDistance || dist < *dl.reclaimBestDistance - SimScalar(8))
                            {
                                dl.reclaimBestDistance = dist;
                                dl.reclaimIssuedAt = sim.gameTime;
                            }
                            stalled = dl.reclaimIssuedAt
                                && (sim.gameTime - *dl.reclaimIssuedAt >= GameTime(DeadlockReclaimWatchdogTicks)
                                    || (!beaming && sim.gameTime >= *dl.reclaimDeadline));
                        }
                        if (!builderLive || stalled)
                        {
                            if (stalled)
                            {
                                // a stall convicts the BUILDER, not
                                // the recovery: its nav cannot reach
                                // the anchor through whatever crowds
                                // it sits inside. The attempt was
                                // already counted at issue -- this only
                                // rules the builder out of the next
                                // pick so someone else gets a turn.
                                dl.reclaimStalledAt = sim.gameTime;
                                dl.reclaimAvoid.insert(dl.reclaimBuilder->value);
                                // free the builder from the dead
                                // order -- an unreachable reclaim
                                // never completes, and a live builder
                                // is more useful re-tasked than pinned
                                // grinding at a wall it cannot reach
                                builder->get().clearOrders();
                            }
                            dl.reclaimTarget = std::nullopt;
                            dl.reclaimBuilder = std::nullopt;
                            dl.reclaimIssuedAt = std::nullopt;
                            dl.reclaimBestDistance = std::nullopt;
                            dl.reclaimDeadline = std::nullopt;
                        }
                        else
                        {
                            continue;
                        }
                    }
                }

                // eligible blockers: own, alive, noncritical, not
                // scripted -- cheapest removal that re-opens egress
                // wins; equal-cost candidates go to the wall nearest
                // the recovery builder so the reclaim needs little or
                // no navigation inside the pocket
                struct BlockerCandidate
                {
                    UnitId id;
                    float cost;
                    DiscreteRect rect;
                    // when set, removal is only useful if it opens the
                    // egress probe of this region (sealed mode: the
                    // factory; stalled mode: a trapped mobile's
                    // pocket). Unset candidates are proven blockers by
                    // direct occupancy of a production cell.
                    std::optional<std::pair<DiscreteRect, MovementClassDefinition>> openRouteProbe;
                };
                std::vector<BlockerCandidate> candidates;
                // own mobiles parked in the yard that can still leave
                // on their own -- they get a shove rather than a
                // reclaim
                std::vector<UnitId> evictMobiles;

                if (probe.open)
                {
                    // production stall: inventory the yard occupants.
                    // A persistent occupant inside the footprint of a
                    // factory that has not produced for the whole
                    // confirm window is a proven blocker.
                    std::set<UnitId> yardMobiles;
                    std::map<UnitId, std::pair<DiscreteRect, MovementClassDefinition>> pocketCtx;
                    std::set<UnitId> pocketBoundary;
                    if (auto region = sim.occupiedGrid.tryToRegion(factoryRect))
                    {
                        sim.occupiedGrid.forEach(*region, [&](const auto& cell) {
                            if (cell.buildingInfo && cell.buildingInfo->unit != factoryId
                                && !cell.buildingInfo->passable)
                            {
                                auto blocker = sim.tryGetUnitState(cell.buildingInfo->unit);
                                if (blocker && blocker->get().isAlive() && blocker->get().owner == st.player
                                    && !blocker->get().missionState)
                                {
                                    const auto& blockerDef = sim.unitDefinitions.at(toUpper(blocker->get().unitType));
                                    if (!blockerDef.commander && !(blockerDef.builder && !blockerDef.isMobile))
                                    {
                                        candidates.push_back(BlockerCandidate{
                                            cell.buildingInfo->unit,
                                            static_cast<float>(blockerDef.buildCostMetal.value) + static_cast<float>(blockerDef.buildCostEnergy.value),
                                            sim.computeFootprintRegion(blocker->get().position, blockerDef.movementCollisionInfo),
                                            std::nullopt});
                                    }
                                }
                            }
                            if (cell.mobileUnitId)
                            {
                                yardMobiles.insert(*cell.mobileUnitId);
                            }
                        });
                    }

                    for (UnitId mobileId : yardMobiles)
                    {
                        auto mob = sim.tryGetUnitState(mobileId);
                        if (!mob || !mob->get().isAlive() || mob->get().owner != st.player || mob->get().carriedBy)
                        {
                            continue;
                        }
                        const auto& mobDef = sim.unitDefinitions.at(toUpper(mob->get().unitType));
                        if (!mobDef.isMobile || mob->get().isBeingBuilt(mobDef))
                        {
                            continue;
                        }
                        auto mrect = sim.computeFootprintRegion(mob->get().position, mobDef.movementCollisionInfo);
                        auto mmc = sim.getAdHocMovementClass(mobDef.movementCollisionInfo);
                        auto mprobe = probeFactoryEgress(sim, mrect, mmc, nullptr, nullptr);
                        if (mprobe.open)
                        {
                            // it can still leave; wander-type orders
                            // are preempted with a shove, real work is
                            // left alone
                            if (mob->get().orders.empty()
                                || std::holds_alternative<MoveOrder>(mob->get().orders.front())
                                || std::holds_alternative<PatrolOrder>(mob->get().orders.front())
                                || std::holds_alternative<GuardOrder>(mob->get().orders.front()))
                            {
                                // a parked mobile that has already
                                // ignored several shoves is nav-immobile
                                // -- boxed by hard blockers its own
                                // flood ignores because they are
                                // traffic, not walls. Reclaim removes it
                                // cleanly (no corpse) so the production
                                // cell it sits on is actually freed.
                                if (++dl.evictAttempts[mobileId.value] > DeadlockEvictMaxAttempts
                                    && !mobDef.commander && !mob->get().missionState)
                                {
                                    candidates.push_back(BlockerCandidate{
                                        mobileId,
                                        static_cast<float>(mobDef.buildCostMetal.value) + static_cast<float>(mobDef.buildCostEnergy.value),
                                        mrect,
                                        std::nullopt});
                                }
                                else
                                {
                                    evictMobiles.push_back(mobileId);
                                }
                            }
                            continue;
                        }
                        // trapped inside the yard pocket: the mobile
                        // itself is a proven blocker -- its footprint
                        // occupies the exact cells production needs
                        // and a reclaim removes it without leaving a
                        // corpse. Cheaper than demolishing every wall
                        // of its pocket, and it works even when the
                        // pocket can never be opened.
                        if (!mobDef.commander && !mob->get().missionState)
                        {
                            candidates.push_back(BlockerCandidate{
                                mobileId,
                                static_cast<float>(mobDef.buildCostMetal.value) + static_cast<float>(mobDef.buildCostEnergy.value),
                                mrect,
                                std::nullopt});
                        }
                        // opening the pocket's boundary frees the
                        // mobile as well -- the walls stay candidates
                        // so the cheaper of the two removals wins
                        for (UnitId boundaryId : mprobe.boundaryBuildings)
                        {
                            if (pocketBoundary.insert(boundaryId).second)
                            {
                                pocketCtx.emplace(boundaryId, std::make_pair(mrect, mmc));
                            }
                        }
                    }
                    for (UnitId boundaryId : pocketBoundary)
                    {
                        auto blocker = sim.tryGetUnitState(boundaryId);
                        if (!blocker || !blocker->get().isAlive() || blocker->get().owner != st.player
                            || blocker->get().missionState)
                        {
                            continue;
                        }
                        const auto& blockerDef = sim.unitDefinitions.at(toUpper(blocker->get().unitType));
                        if (blockerDef.commander || (blockerDef.builder && !blockerDef.isMobile))
                        {
                            continue;
                        }
                        candidates.push_back(BlockerCandidate{
                            boundaryId,
                            static_cast<float>(blockerDef.buildCostMetal.value) + static_cast<float>(blockerDef.buildCostEnergy.value),
                            sim.computeFootprintRegion(blocker->get().position, blockerDef.movementCollisionInfo),
                            pocketCtx.at(boundaryId)});
                    }

                    for (UnitId mobileId : evictMobiles)
                    {
                        auto& mob = sim.getUnitState(mobileId);
                        mob.clearOrders();
                        mob.addOrder(BuggerOffOrder(factoryRect));
                    }

                    // evictions alone may clear the stall; only a
                    // proven structural blocker justifies reclaim
                    if (candidates.empty())
                    {
                        continue;
                    }
                }
                else
                {
                    // a completed mobile product must actually be
                    // trapped in the sealed region -- a sealed but
                    // empty yard does not justify reclaiming
                    // structures. The pocket only stores *visited*
                    // anchors, so a unit is proven trapped by its own
                    // egress probe: seeds its footprint and checks
                    // whether the flood escapes. Only units inside the
                    // factory's escape box are probed.
                    bool trapped = false;
                    for (const auto& [id, unit] : sim.units)
                    {
                        if (unit.owner != st.player || unit.isDead() || unit.carriedBy)
                        {
                            continue;
                        }
                        const auto& def = sim.unitDefinitions.at(toUpper(unit.unitType));
                        if (!def.isMobile)
                        {
                            continue;
                        }
                        auto urect = sim.computeFootprintRegion(unit.position, def.movementCollisionInfo);
                        const int margin = EgressEscapeRadiusCells + 4;
                        bool near = urect.x < factoryRect.x + factoryRect.width + margin
                            && urect.x + urect.width > factoryRect.x - margin
                            && urect.y < factoryRect.y + factoryRect.height + margin
                            && urect.y + urect.height > factoryRect.y - margin;
                        if (!near)
                        {
                            continue;
                        }
                        if (!probeFactoryEgress(sim, urect, *mc, nullptr, nullptr).open)
                        {
                            trapped = true;
                            break;
                        }
                    }
                    if (!trapped)
                    {
                        continue;
                    }

                    for (UnitId blockerId : probe.boundaryBuildings)
                    {
                        auto blocker = sim.tryGetUnitState(blockerId);
                        if (!blocker || !blocker->get().isAlive() || blocker->get().owner != st.player)
                        {
                            continue;
                        }
                        // scripted/mission structures are never
                        // removed; the AI must route around authored
                        // content
                        if (blocker->get().missionState)
                        {
                            continue;
                        }
                        const auto& blockerDef = sim.unitDefinitions.at(toUpper(blocker->get().unitType));
                        if (blockerDef.commander || (blockerDef.builder && !blockerDef.isMobile))
                        {
                            continue;
                        }
                        candidates.push_back(BlockerCandidate{
                            blockerId,
                            static_cast<float>(blockerDef.buildCostMetal.value) + static_cast<float>(blockerDef.buildCostEnergy.value),
                            sim.computeFootprintRegion(blocker->get().position, blockerDef.movementCollisionInfo),
                            std::make_pair(factoryRect, *mc)});
                    }
                }

                std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                    return a.cost != b.cost ? a.cost < b.cost : a.id < b.id;
                });

                // choose the recovery builder. Prefer a builder whose
                // own egress probe stays open -- it paths toward the
                // target through clear terrain while a pocketed
                // builder must thread the same congestion that caused
                // the deadlock. Trapped builders still matter as the
                // fallback: a wall whose only working-range anchors
                // sit inside the pocket can only be reclaimed from
                // within, and anchorInReach verifies the trapped
                // builder's own movement class can actually reach one
                // -- a vehicle parked in kbot-only terrain can never
                // close on the blocker and would stall forever.
                const int gridW = static_cast<int>(sim.terrain.getHeightMap().getWidth());
                auto builderUsable = [&](const UnitState& u, const UnitDefinition& def, const EgressProbeResult& bprobe, bool allowMission) {
                    if (!(u.owner == st.player && u.isAlive() && !u.carriedBy && !u.aircraftService
                            && def.builder && def.isMobile
                            && !(u.missionState && u.missionState->aiIgnore)))
                    {
                        return false;
                    }
                    // builders mid-construction are doing real work;
                    // only wander-type orders may be preempted
                    if (u.orders.empty())
                    {
                        return true;
                    }
                    if (std::holds_alternative<MoveOrder>(u.orders.front())
                        || std::holds_alternative<PatrolOrder>(u.orders.front())
                        || std::holds_alternative<GuardOrder>(u.orders.front())
                        || std::holds_alternative<WaitOrder>(u.orders.front()))
                    {
                        return true;
                    }
                    // a trapped builder whose build site lies outside
                    // its sealed pocket can never reach it -- the
                    // order is provably-futile work and recovery may
                    // preempt it; a site inside the pocket is real
                    // work and stays untouched
                    if (!bprobe.open)
                    {
                        std::optional<SimVector> site;
                        if (const auto* bo = std::get_if<BuildOrder>(&u.orders.front()))
                        {
                            site = bo->position;
                        }
                        else if (const auto* mbo = std::get_if<MissionBuildOrder>(&u.orders.front()))
                        {
                            site = mbo->position;
                        }
                        else if (const auto* cbo = std::get_if<CompleteBuildOrder>(&u.orders.front()))
                        {
                            if (auto target = sim.tryGetUnitState(cbo->target))
                            {
                                site = target->get().position;
                            }
                        }
                        if (site)
                        {
                            auto cell = sim.terrain.worldToHeightmapCoordinateNearest(*site);
                            if (cell.x >= 0 && cell.y >= 0
                                && cell.x < gridW && cell.y < static_cast<int>(sim.terrain.getHeightMap().getHeight()))
                            {
                                auto idx = static_cast<unsigned int>(cell.y) * static_cast<unsigned int>(gridW)
                                    + static_cast<unsigned int>(cell.x);
                                return bprobe.pocket.find(idx) == bprobe.pocket.end();
                            }
                        }
                    }
                    // escalation: the AI's own construction order is
                    // safe to interrupt -- builderScan re-tasks idle
                    // builders every cycle, so a preempted mission is
                    // only delayed, never lost
                    if (allowMission && std::holds_alternative<MissionBuildOrder>(u.orders.front()))
                    {
                        return true;
                    }
                    return false;
                };
                auto anchorInReach = [&](const EgressProbeResult& p, const DiscreteRect& r, int reach) {
                    for (auto idx : p.pocket)
                    {
                        int ax = static_cast<int>(idx % static_cast<unsigned int>(gridW));
                        int ay = static_cast<int>(idx / static_cast<unsigned int>(gridW));
                        int dx = std::max(static_cast<int>(r.x) - ax, ax - (static_cast<int>(r.x + r.width) - 1));
                        int dy = std::max(static_cast<int>(r.y) - ay, ay - (static_cast<int>(r.y + r.height) - 1));
                        if (std::max(dx, dy) <= reach)
                        {
                            return true;
                        }
                    }
                    return false;
                };

                // a candidate only qualifies when removing it opens the
                // route it was picked for: the factory's egress in
                // sealed mode, or a trapped mobile's pocket in stalled
                // mode. Direct yard occupants carry no probe -- their
                // occupancy of a production cell is itself the proof.
                auto removalOpensRoute = [&](const BlockerCandidate& c) {
                    if (!c.openRouteProbe)
                    {
                        return true;
                    }
                    return !probeFactoryEgress(sim, c.openRouteProbe->first, c.openRouteProbe->second, nullptr, &c.id).open;
                };

                UnitId chosen{0};
                bool sawBuilder = false;
                std::optional<BlockerCandidate> pick;
                // each stall burns one attempt and convicts only the
                // builder that made it; picks continue with other
                // builders until the budget is spent. The budget then
                // refreshes once the verdict is old enough to reflect
                // obsolete geometry
                if (dl.reclaimAttempts >= DeadlockMaxReclaimAttempts
                    && dl.reclaimStalledAt
                    && sim.gameTime - *dl.reclaimStalledAt >= GameTime(DeadlockReclaimCooldownTicks))
                {
                    dl.reclaimAttempts = 0;
                    dl.reclaimStalledAt = std::nullopt;
                    dl.reclaimAvoid.clear();
                }
                bool reclaimViable = dl.reclaimAttempts < DeadlockMaxReclaimAttempts;
                // strict picks require one removal to open the route
                // outright; a pocket sealed by several cooperating
                // walls rejects every candidate that way even though
                // reclaiming the boundary one wall at a time is real
                // progress, so the relaxed pass accepts any member of
                // the probed seal (boundary membership is itself proof
                // the wall helps hold the pocket shut). The last stage
                // additionally lets recovery preempt a builder's own
                // MissionBuildOrder -- a task builderScan re-issues
                // anyway -- when no wander-type or futile-work builder
                // could reach anything.
                for (int stage = 0; stage < 3 && !pick && reclaimViable; ++stage)
                {
                    const bool strictPick = stage == 0;
                    const bool allowMission = stage == 2;
                    for (int pass = 0; pass < 2 && !pick; ++pass)
                    {
                        for (UnitId builderId : st.groups[AiGroupBuilder])
                        {
                            // this builder already proved it cannot
                            // reach a working anchor -- give someone
                            // else the next attempt
                            if (dl.reclaimAvoid.count(builderId.value) != 0)
                            {
                                continue;
                            }
                            auto b = sim.tryGetUnitState(builderId);
                            if (!b)
                            {
                                continue;
                            }
                            const auto& bDef = sim.unitDefinitions.at(toUpper(b->get().unitType));
                            auto brect = sim.computeFootprintRegion(b->get().position, bDef.movementCollisionInfo);
                            // the builder's own movement class decides
                            // whether it is trapped and where it can walk
                            auto bmc = sim.getAdHocMovementClass(bDef.movementCollisionInfo);
                            auto bprobe = probeFactoryEgress(sim, brect, bmc, nullptr, nullptr);
                            if (!builderUsable(b->get(), bDef, bprobe, allowMission))
                            {
                                continue;
                            }
                            sawBuilder = true;
                            // open builders first; pocketed builders
                            // only get a turn when no open builder
                            // could pick a candidate
                            if ((pass == 0) != bprobe.open)
                            {
                                continue;
                            }
                            const auto builderPos = b->get().position;
                            // build range in cells plus footprint-center
                            // offset and slack for the anchor->position
                            // difference
                            int reach = static_cast<int>(simScalarToFloat(bDef.buildDistance) / 16.0f)
                                + static_cast<int>((bmc.footprintX + bmc.footprintZ) / 4) + 2;

                            // equal-cost candidates prefer the wall nearest
                            // this builder
                            const BlockerCandidate* best = nullptr;
                            float bestD2 = 0.0f;
                            for (const auto& c : candidates)
                            {
                                // a unit can never reclaim itself
                                if (c.id == builderId)
                                {
                                    continue;
                                }
                                if (strictPick && !removalOpensRoute(c))
                                {
                                    continue;
                                }
                                // a trapped builder must already have a
                                // working-range anchor inside its pocket;
                                // open-terrain builders can path there
                                if (!bprobe.open && !anchorInReach(bprobe, c.rect, reach))
                                {
                                    continue;
                                }
                                auto cUnit = sim.tryGetUnitState(c.id);
                                if (!cUnit || !cUnit->get().isAlive())
                                {
                                    continue;
                                }
                                auto d2 = cUnit->get().position.distanceSquared(builderPos).value;
                                if (!best || d2 < bestD2)
                                {
                                    best = &c;
                                    bestD2 = d2;
                                }
                            }
                            if (best)
                            {
                                chosen = builderId;
                                pick = *best;
                                break;
                            }
                        }
                    }
                    if (pick)
                    {
                        break;
                    }
                }
                if (pick)
                {
                    // recovery preempts the builder's wander orders: a
                    // queued Move/Patrol would park the reclaim behind
                    // stale navigation state
                    auto& bState = sim.getUnitState(chosen);
                    bState.clearOrders();
                    bState.addOrder(ReclaimOrder{pick->id});
                    dl.reclaimTarget = pick->id;
                    dl.reclaimBuilder = chosen;
                    dl.reclaimIssuedAt = sim.gameTime;
                    ++dl.reclaimAttempts;
                    LOG_DEBUG << "deadlock recover: factory " << factoryId.value
                              << " builder " << chosen.value
                              << " -> reclaim " << pick->id.value;
                    continue;
                }

                // LAST RESORT (TA-TRANSPLANT ROBUSTNESS APPROXIMATION):
                // no builder can reach any blocker -- island/cliff
                // pockets, dead builders, or a reclaim path that has
                // already stalled out of range. Once the seal has
                // persisted past the confirm + last-resort window the
                // cheapest own, noncritical blocker that re-opens
                // egress self-destructs. Scripted structures never
                // reach the candidate list, so authored content is
                // still untouchable.
                bool graceElapsed = sim.gameTime - *dl.blockedSince
                    >= GameTime(DeadlockConfirmTicks + DeadlockLastResortTicks);
                if (graceElapsed && (dl.reclaimAttempts >= DeadlockMaxReclaimAttempts || !sawBuilder))
                {
                    for (const auto& c : candidates)
                    {
                        if (!removalOpensRoute(c))
                        {
                            continue;
                        }
                        // a self-destruct detonates through the normal
                        // damage path and leaves a blocking corpse on
                        // the same cells; for a direct yard occupant
                        // that corpse re-blocks the required cell and
                        // nothing is gained, so the last resort only
                        // fires on route-validated boundary structures
                        if (!c.openRouteProbe)
                        {
                            continue;
                        }
                        auto cUnit = sim.tryGetUnitState(c.id);
                        if (!cUnit || !cUnit->get().isAlive())
                        {
                            continue;
                        }
                        // the wreck stays on the field: removal only
                        // helps if the route opens even with the
                        // corpse's cells still blocked, so the probe
                        // frees the building's cells but re-blocks the
                        // corpse footprint -- a wall whose wreck fills
                        // its own cells is never worth detonating
                        const auto& cDef = sim.unitDefinitions.at(toUpper(cUnit->get().unitType));
                        if (!cDef.corpse.empty())
                        {
                            auto corpseDefId = sim.tryGetFeatureDefinitionId(cDef.corpse);
                            if (corpseDefId)
                            {
                                const auto& corpseDef = sim.getFeatureDefinition(*corpseDefId);
                                auto corpseRect = sim.computeFootprintRegion(cUnit->get().position,
                                    static_cast<unsigned int>(corpseDef.footprintX),
                                    static_cast<unsigned int>(corpseDef.footprintZ));
                                if (!probeFactoryEgress(sim, c.openRouteProbe->first, c.openRouteProbe->second,
                                        &corpseRect, &c.id)
                                        .open)
                                {
                                    continue;
                                }
                            }
                        }
                        cUnit->get().addOrder(SelfDestructOrder{DeadlockSelfDestructTicks});
                        dl.reclaimTarget = c.id;
                        dl.reclaimBuilder = std::nullopt;
                        dl.reclaimIssuedAt = sim.gameTime;
                        break;
                    }
                }
            }
        }

        /**
         * Retail target scan 0x4071f0: iterate every other player's
         * owned units, skipping players in the AI's allied table
         * (aiObj+0x108), dead units (+0x110&3==2), immune units
         * (+0x110&0x8000) and cloaked units (+0x10e&4), then take the
         * candidate nearest the squad centroid by distance squared.
         */
        std::optional<UnitId> pickAttackTarget(GameSimulation& sim, PlayerId aiPlayer, const SimVector& origin)
        {
            std::optional<UnitId> best;
            SimScalar bestDistance(0);
            for (const auto& [id, unit] : sim.units)
            {
                if (unit.owner == aiPlayer || unit.isDead() || unit.carriedBy)
                {
                    continue;
                }
                // retail iterates player records' owned-unit lists, so
                // units without a live player owner are unreachable
                if (unit.owner.value >= sim.players.size())
                {
                    continue;
                }
                if (sim.areAllied(unit.owner, aiPlayer))
                {
                    continue;
                }
                if (unit.cloaked || (unit.missionState && unit.missionState->immunity))
                {
                    continue;
                }
                const auto& def = sim.unitDefinitions.at(toUpper(unit.unitType));
                if (unit.isBeingBuilt(def))
                {
                    continue;
                }

                auto distance = origin.distanceSquared(unit.position);
                if (!best || distance <= bestDistance)
                {
                    best = id;
                    bestDistance = distance;
                }
            }
            return best;
        }

        void attackScan(GameSimulation& sim, CampaignAiPlayerState& st)
        {
            if (st.attackTarget)
            {
                auto target = sim.tryGetUnitState(*st.attackTarget);
                if (!target || !target->get().isAlive())
                {
                    st.attackTarget = std::nullopt;
                }
            }

            std::vector<UnitId> members;
            for (auto groupIndex : {AiGroupReserveA, AiGroupReserveB})
            {
                for (UnitId id : st.groups[groupIndex])
                {
                    const auto* unit = liveMember(sim, st, id);
                    if (!unit || !unit->orders.empty() || unit->aircraftService)
                    {
                        continue;
                    }
                    members.push_back(id);
                }
            }

            if (members.size() >= MinAttackSquadSize)
            {
                SimVector centroid(0_ss, 0_ss, 0_ss);
                for (auto id : members)
                {
                    centroid += sim.getUnitState(id).position;
                }
                centroid.x = centroid.x / SimScalar(static_cast<int>(members.size()));
                centroid.z = centroid.z / SimScalar(static_cast<int>(members.size()));

                if (auto target = pickAttackTarget(sim, st.player, centroid))
                {
                    for (auto id : members)
                    {
                        sim.getUnitState(id).addOrder(AttackOrder(*target));
                    }
                    st.attackTarget = target;
                }
            }
            else if (st.attackTarget)
            {
                // retail parameter tasks trail reserves toward the
                // squad's current target as reinforcement
                const auto& target = sim.getUnitState(*st.attackTarget);
                for (auto id : members)
                {
                    sim.getUnitState(id).addOrder(MoveOrder(target.position));
                }
            }
        }
    }

    /**
     * TA-TRANSPLANT ROBUSTNESS APPROXIMATION -- retail's site
     * finder (TotalA.exe 0x419b00) validates only the candidate
     * footprint; it never checks that a placement preserves
     * factory egress, which is exactly how stock AI walls itself
     * in.
     *
     * The probe flood-fills from the factory's yard ring over
     * cells a produced unit can stand on (same walkability +
     * static-occupancy predicate as the unit pathfinder) and
     * reports whether the fill escapes the base region.
     * `extraBlocked` simulates a hypothetical footprint;
     * `extraOpenId` simulates removing that building: its cells are
     * freed, but features and any other buildings sharing them still
     * block -- a wall built over a rock does not make the rock
     * removable. Mobile units never block -- they are traffic, not
     * walls.
     */
    EgressProbeResult probeFactoryEgress(
        const GameSimulation& sim,
        const DiscreteRect& factoryRect,
        const MovementClassDefinition& mc,
        const DiscreteRect* extraBlocked,
        const UnitId* extraOpenId)
    {
        EgressProbeResult out;

        const auto& heights = sim.terrain.getHeightMap();
        const auto gridW = static_cast<int>(heights.getWidth());
        const auto gridH = static_cast<int>(heights.getHeight());

        auto inRect = [](const DiscreteRect& r, int x, int y) {
            return x >= static_cast<int>(r.x) && y >= static_cast<int>(r.y)
                && x < static_cast<int>(r.x + r.width) && y < static_cast<int>(r.y + r.height);
        };

        // whether the produced unit's footprint anchored at
        // (x,y) is clear of static blockers
        auto footprintStaticBlocked = [&](int x, int y, bool collect, std::unordered_set<unsigned int>* boundary) {
            bool blocked = false;
            for (unsigned int dy = 0; dy < mc.footprintZ; ++dy)
            {
                for (unsigned int dx = 0; dx < mc.footprintX; ++dx)
                {
                    int cx = x + static_cast<int>(dx);
                    int cy = y + static_cast<int>(dy);
                    if (extraBlocked && inRect(*extraBlocked, cx, cy))
                    {
                        blocked = true;
                        continue;
                    }
                    if (inRect(factoryRect, cx, cy))
                    {
                        // the product walks out through the yard
                        continue;
                    }
                    auto cell = sim.occupiedGrid.tryGet(Point(cx, cy));
                    if (!cell)
                    {
                        continue;
                    }
                    const auto& v = cell->get();
                    if (v.featureId)
                    {
                        // only blocking features obstruct the route --
                        // decorative litter (rocks, weeds) is walkable
                        // in the physical collision model, so the
                        // probe must not count it as a wall either
                        const auto& f = sim.getFeature(*v.featureId);
                        if (sim.getFeatureDefinition(f.featureName).blocking)
                        {
                            blocked = true;
                        }
                    }
                    else if (v.buildingInfo && !v.buildingInfo->passable)
                    {
                        if (extraOpenId && v.buildingInfo->unit == *extraOpenId)
                        {
                            // the hypothetically-removed building
                            // frees only its own cells
                            continue;
                        }
                        blocked = true;
                        if (collect)
                        {
                            boundary->insert(static_cast<unsigned int>(v.buildingInfo->unit.value));
                        }
                    }
                    // mobileUnitId is traffic, never a wall
                }
            }
            return blocked;
        };

        auto passable = [&](int x, int y) {
            // getSlope samples one cell past the footprint corner
            if (x < 0 || y < 0
                || x + static_cast<int>(mc.footprintX) >= gridW
                || y + static_cast<int>(mc.footprintZ) >= gridH)
            {
                return false;
            }
            if (!isGridPointWalkable(sim.terrain, mc, x, y))
            {
                return false;
            }
            return !footprintStaticBlocked(x, y, false, nullptr);
        };

        // seeds: every anchor inside the factory yard -- the product
        // physically starts inside and walks out, so a wall flush
        // against the yard must not make the probe unseedable
        const int escapeX0 = static_cast<int>(factoryRect.x) - EgressEscapeRadiusCells;
        const int escapeY0 = static_cast<int>(factoryRect.y) - EgressEscapeRadiusCells;
        const int escapeX1 = static_cast<int>(factoryRect.x + factoryRect.width) + EgressEscapeRadiusCells;
        const int escapeY1 = static_cast<int>(factoryRect.y + factoryRect.height) + EgressEscapeRadiusCells;

        std::deque<Point> queue;
        auto cellIndex = [&](int x, int y) {
            return static_cast<unsigned int>(y) * static_cast<unsigned int>(gridW) + static_cast<unsigned int>(x);
        };
        const int fw = static_cast<int>(factoryRect.width);
        const int fh = static_cast<int>(factoryRect.height);
        for (int x = static_cast<int>(factoryRect.x); x < static_cast<int>(factoryRect.x) + fw; ++x)
        {
            for (int y = static_cast<int>(factoryRect.y); y < static_cast<int>(factoryRect.y) + fh; ++y)
            {
                auto idx = cellIndex(x, y);
                if (out.pocket.find(idx) != out.pocket.end() || !passable(x, y))
                {
                    continue;
                }
                out.pocket.insert(idx);
                queue.push_back(Point(x, y));
            }
        }

        std::unordered_set<unsigned int> boundarySet;
        while (!queue.empty() && out.pocket.size() <= EgressVisitCapCells)
        {
            auto p = queue.front();
            queue.pop_front();
            if (p.x <= escapeX0 || p.x >= escapeX1 || p.y <= escapeY0 || p.y >= escapeY1)
            {
                out.open = true;
                break;
            }
            for (const auto& d : {Point(1, 0), Point(-1, 0), Point(0, 1), Point(0, -1)})
            {
                int nx = p.x + d.x;
                int ny = p.y + d.y;
                if (out.pocket.find(cellIndex(nx, ny)) != out.pocket.end())
                {
                    continue;
                }
                if (!passable(nx, ny))
                {
                    // a boundary failure caused by a building is a
                    // reclaim candidate; terrain is not removable
                    footprintStaticBlocked(nx, ny, true, &boundarySet);
                    continue;
                }
                out.pocket.insert(cellIndex(nx, ny));
                queue.push_back(Point(nx, ny));
            }
        }
        // hitting the visit cap means the "pocket" is huge --
        // effectively open
        if (out.pocket.size() > EgressVisitCapCells)
        {
            out.open = true;
        }

        // resolve the collected boundary cells to units in a
        // deterministic (id) order
        std::vector<unsigned int> ids(boundarySet.begin(), boundarySet.end());
        std::sort(ids.begin(), ids.end());
        for (auto id : ids)
        {
            out.boundaryBuildings.push_back(UnitId(id));
        }
        return out;
    }

    /**
     * True when hypothetically occupying `candidateRect` would
     * disconnect some own factory's yard exit. Only factories
     * whose escape region the candidate can plausibly intersect
     * are probed.
     */
    bool placementSealsFactoryEgress(
        GameSimulation& sim,
        const CampaignAiPlayerState& st,
        const DiscreteRect& candidateRect)
    {
        for (UnitId factoryId : st.groups[AiGroupFactory])
        {
            auto factory = sim.tryGetUnitState(factoryId);
            if (!factory || !factory->get().isAlive() || factory->get().owner != st.player)
            {
                continue;
            }
            const auto& factoryDef = sim.unitDefinitions.at(toUpper(factory->get().unitType));
            if (!factoryDef.builder || factoryDef.isMobile)
            {
                continue;
            }
            auto mc = factoryProductMovementClass(sim, factoryDef);
            if (!mc)
            {
                // no mobile product -- nothing can be trapped
                continue;
            }
            auto factoryRect = sim.computeFootprintRegion(factory->get().position, factoryDef.movementCollisionInfo);

            // cheap reject: the candidate must reach into the
            // factory's escape region to possibly affect egress
            const int margin = EgressEscapeRadiusCells + 2;
            bool near = static_cast<int>(candidateRect.x) < static_cast<int>(factoryRect.x + factoryRect.width) + margin
                && static_cast<int>(candidateRect.x + candidateRect.width) > static_cast<int>(factoryRect.x) - margin
                && static_cast<int>(candidateRect.y) < static_cast<int>(factoryRect.y + factoryRect.height) + margin
                && static_cast<int>(candidateRect.y + candidateRect.height) > static_cast<int>(factoryRect.y) - margin;
            if (!near)
            {
                continue;
            }

            auto probe = probeFactoryEgress(sim, factoryRect, *mc, &candidateRect, nullptr);
            if (!probe.open)
            {
                return true;
            }
        }
        return false;
    }

    std::optional<SimVector> findBuildSite(
        GameSimulation& sim,
        const CampaignAiPlayerState& st,
        const UnitState& builder,
        const UnitDefinition& builderDef,
        const UnitDefinition& targetDef)
    {
        if (targetDef.isMobile)
        {
            // mobile products are not placed by footprint rules;
            // builders in stock data only produce structures
            return std::nullopt;
        }

        auto builderRect = sim.computeFootprintRegion(builder.position, builderDef.movementCollisionInfo);
        auto mc = sim.getAdHocMovementClass(targetDef.movementCollisionInfo);

        for (int r = 1; r <= SiteSearchRadiusCells; ++r)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                for (int dz = -r; dz <= r; ++dz)
                {
                    if (std::max(std::abs(dx), std::abs(dz)) != r)
                    {
                        continue;
                    }
                    auto pos = sim.terrain.heightmapIndexToWorldCenter(builderRect.x + dx, builderRect.y + dz);
                    auto rect = sim.computeFootprintRegion(pos, targetDef.movementCollisionInfo);
                    auto placement = sim.computeBuildingPlacement(mc, targetDef.yardMap, targetDef.yardMapContainsGeo, targetDef.waterline, rect.x, rect.y);
                    if (!placement || !placement->valid)
                    {
                        continue;
                    }
                    if (placementSealsFactoryEgress(sim, st, rect))
                    {
                        continue;
                    }
                    return pos;
                }
            }
        }
        return std::nullopt;
    }

    void initCampaignAiPlayer(GameSimulation& sim, PlayerId player, const CampaignAiProfile& profile)
    {
        auto& playerInfo = sim.getPlayer(player);
        if (playerInfo.type != GamePlayerType::Computer)
        {
            return;
        }

        CampaignAiPlayerState st;
        st.player = player;

        for (const auto& [name, def] : sim.unitDefinitions)
        {
            st.unitIndex.push_back(name);
        }
        std::sort(st.unitIndex.begin(), st.unitIndex.end());

        st.unitWeights.assign(st.unitIndex.size(), 100);
        st.unitLimits.assign(st.unitIndex.size(), -1);
        std::vector<bool> weightLocked(st.unitIndex.size(), false);
        std::vector<bool> limitLocked(st.unitIndex.size(), false);

        applyWeightDirectives(sim, st, profile.weightDirectives, weightLocked);
        applyLimitDirectives(sim, st, profile.limitDirectives, limitLocked);

        // per-unit FBI `ai_limit` self-directives (retail 0x409f80/
        // 0x40a040): re-parsed for unit types the profile left unlocked
        for (std::size_t i = 0; i < st.unitIndex.size(); ++i)
        {
            const auto& def = sim.unitDefinitions.at(st.unitIndex[i]);
            if (def.aiSelfDirective.empty())
            {
                continue;
            }
            CampaignAiProfile self;
            parseCampaignAiSelfDirective(def.aiSelfDirective, self);
            if (!weightLocked[i])
            {
                applyWeightDirectives(sim, st, self.weightDirectives, weightLocked);
            }
            if (!limitLocked[i])
            {
                applyLimitDirectives(sim, st, self.limitDirectives, limitLocked);
            }
        }

        sim.campaignAi.push_back(std::move(st));
    }

    void updateCampaignAi(GameSimulation& sim)
    {
        for (auto& st : sim.campaignAi)
        {
            auto& player = sim.getPlayer(st.player);
            if (player.type != GamePlayerType::Computer || player.status != GamePlayerStatus::Alive)
            {
                continue;
            }

            if (sim.gameTime >= st.nextRepartition)
            {
                repartition(sim, st);
                st.nextRepartition = sim.gameTime + GameTime(RepartitionPeriodTicks);
            }
            if (sim.gameTime >= st.nextFactoryScan)
            {
                factoryScan(sim, st);
                st.nextFactoryScan = sim.gameTime + GameTime(FactoryScanPeriodTicks);
            }
            if (sim.gameTime >= st.nextBuilderScan)
            {
                builderScan(sim, st);
                st.nextBuilderScan = sim.gameTime + GameTime(BuilderScanPeriodTicks);
            }
            if (sim.gameTime >= st.nextAttackScan)
            {
                attackScan(sim, st);
                st.nextAttackScan = sim.gameTime + GameTime(AttackScanPeriodTicks);
            }
            if (sim.gameTime >= st.nextDeadlockScan)
            {
                deadlockScan(sim, st);
                st.nextDeadlockScan = sim.gameTime + GameTime(DeadlockScanPeriodTicks);
            }
        }
    }

    void rebuildCampaignAiIndex(GameSimulation& sim)
    {
        if (sim.campaignAi.empty())
        {
            return;
        }

        std::vector<std::string> index;
        index.reserve(sim.unitDefinitions.size());
        for (const auto& [name, def] : sim.unitDefinitions)
        {
            index.push_back(name);
        }
        std::sort(index.begin(), index.end());
        for (auto& st : sim.campaignAi)
        {
            st.unitIndex = index;
        }
    }
}
