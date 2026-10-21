#include "SavedGame.h"

#include <rwe/cob/CobEnvironment.h>
#include <rwe/sim/UnitBehaviorService.h>
#include <rwe/util/Index.h>
#include <rwe/util/match.h>
#include <rwe/util/OpaqueId.h>
#include <rwe/util/rwe_string.h>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace rwe
{
    namespace
    {
        // ------------------------------------------------------------------
        // primitive field codecs
        // ------------------------------------------------------------------

        template <typename Tag>
        void writeId(SaveWriter& w, const OpaqueId<unsigned int, Tag>& id)
        {
            w.u32(id.value);
        }

        template <typename Tag>
        OpaqueId<unsigned int, Tag> readId(SaveReader& r)
        {
            return OpaqueId<unsigned int, Tag>(r.u32());
        }

        void writeGameTime(SaveWriter& w, GameTime t)
        {
            w.u32(t.value);
        }

        GameTime readGameTime(SaveReader& r)
        {
            return GameTime(r.u32());
        }

        void writeSimAngle(SaveWriter& w, SimAngle a)
        {
            w.u16(a.value);
        }

        SimAngle readSimAngle(SaveReader& r)
        {
            return SimAngle(r.u16());
        }

        void writeSimScalar(SaveWriter& w, SimScalar s)
        {
            w.f32bits(s.value);
        }

        SimScalar readSimScalar(SaveReader& r)
        {
            return SimScalar(r.f32bits());
        }

        void writeSimVector(SaveWriter& w, const SimVector& v)
        {
            writeSimScalar(w, v.x);
            writeSimScalar(w, v.y);
            writeSimScalar(w, v.z);
        }

        SimVector readSimVector(SaveReader& r)
        {
            SimVector v;
            v.x = readSimScalar(r);
            v.y = readSimScalar(r);
            v.z = readSimScalar(r);
            return v;
        }

        /**
         * Guard a declared element count before an upfront allocation.
         * Every serialized element costs at least one byte, so a count larger
         * than the remaining payload is necessarily malformed.
         */
        void requireReadableElements(const SaveReader& r, std::uint64_t count)
        {
            if (count > r.remaining())
            {
                throw SaveFormatException("element count exceeds remaining save data");
            }
        }

        void writeDiscreteRect(SaveWriter& w, const DiscreteRect& d)
        {
            w.i32(d.x);
            w.i32(d.y);
            w.i32(d.width);
            w.i32(d.height);
        }

        DiscreteRect readDiscreteRect(SaveReader& r)
        {
            auto x = r.i32();
            auto y = r.i32();
            auto w = r.i32();
            auto h = r.i32();
            if (w < 0 || h < 0)
            {
                throw SaveFormatException("negative DiscreteRect extent in save");
            }
            return DiscreteRect(x, y, w, h);
        }

        template <typename T, typename Tag>
        void writeOpaqueUnit(SaveWriter& w, const OpaqueUnit<T, Tag>& v)
        {
            if constexpr (std::is_same_v<T, float>)
            {
                w.f32bits(v.value);
            }
            else
            {
                w.i32(static_cast<std::int32_t>(v.value));
            }
        }

        template <typename T, typename Tag>
        OpaqueUnit<T, Tag> readOpaqueUnit(SaveReader& r)
        {
            OpaqueUnit<T, Tag> v;
            if constexpr (std::is_same_v<T, float>)
            {
                v.value = r.f32bits();
            }
            else
            {
                v.value = static_cast<T>(r.i32());
            }
            return v;
        }

        void writeOptionalString(SaveWriter& w, const std::optional<std::string>& v)
        {
            w.boolean(v.has_value());
            if (v)
            {
                w.string(*v);
            }
        }

        std::optional<std::string> readOptionalString(SaveReader& r)
        {
            if (!r.boolean())
            {
                return std::nullopt;
            }
            return r.string();
        }

        void writeOptionalId(SaveWriter& w, const std::optional<unsigned int>& v)
        {
            w.boolean(v.has_value());
            if (v)
            {
                w.u32(*v);
            }
        }

        std::optional<unsigned int> readOptionalU32(SaveReader& r)
        {
            if (!r.boolean())
            {
                return std::nullopt;
            }
            return r.u32();
        }

        void writeOptionalGameTime(SaveWriter& w, const std::optional<GameTime>& v)
        {
            w.boolean(v.has_value());
            if (v)
            {
                writeGameTime(w, *v);
            }
        }

        std::optional<GameTime> readOptionalGameTime(SaveReader& r)
        {
            if (!r.boolean())
            {
                return std::nullopt;
            }
            return readGameTime(r);
        }

        void writeOptionalSimVector(SaveWriter& w, const std::optional<SimVector>& v)
        {
            w.boolean(v.has_value());
            if (v)
            {
                writeSimVector(w, *v);
            }
        }

        std::optional<SimVector> readOptionalSimVector(SaveReader& r)
        {
            if (!r.boolean())
            {
                return std::nullopt;
            }
            return readSimVector(r);
        }

        void writeU8Grid(SaveWriter& w, const Grid<unsigned char>& g)
        {
            w.i32(g.getWidth());
            w.i32(g.getHeight());
            const auto& data = g.getVector();
            w.u32(static_cast<std::uint32_t>(data.size()));
            w.raw(data.data(), data.size());
        }

        Grid<unsigned char> readU8Grid(SaveReader& r)
        {
            auto width = r.i32();
            auto height = r.i32();
            auto size = r.u32();
            if (width < 0 || height < 0 || static_cast<long long>(width) * height != size)
            {
                throw SaveFormatException("grid dimensions mismatch in save");
            }
            requireReadableElements(r, size);
            std::vector<unsigned char> data(size);
            r.raw(data.data(), size);
            return Grid<unsigned char>(width, height, std::move(data));
        }

        // ------------------------------------------------------------------
        // unit orders
        // ------------------------------------------------------------------

        void writeAttackTarget(SaveWriter& w, const AttackTarget& t)
        {
            match(
                t,
                [&](const UnitId& id) {
                    w.u8(0);
                    writeId(w, id);
                },
                [&](const SimVector& v) {
                    w.u8(1);
                    writeSimVector(w, v);
                });
        }

        AttackTarget readAttackTarget(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return readId<UnitIdTag>(r);
                case 1:
                    return readSimVector(r);
                default:
                    throw SaveFormatException("bad AttackTarget tag");
            }
        }

        void writeReclaimTarget(SaveWriter& w, const ReclaimTarget& t)
        {
            match(
                t,
                [&](const UnitId& id) {
                    w.u8(0);
                    writeId(w, id);
                },
                [&](const FeatureId& id) {
                    w.u8(1);
                    writeId(w, id);
                });
        }

        ReclaimTarget readReclaimTarget(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return ReclaimTarget(readId<UnitIdTag>(r));
                case 1:
                    return ReclaimTarget(readId<FeatureIdTag>(r));
                default:
                    throw SaveFormatException("bad ReclaimTarget tag");
            }
        }

        void writePatrolDivertTarget(SaveWriter& w, const PatrolDivertTarget& t)
        {
            match(
                t,
                [&](const PatrolAssistTarget& v) {
                    w.u8(0);
                    writeId(w, v.target);
                },
                [&](const PatrolRepairTarget& v) {
                    w.u8(1);
                    writeId(w, v.target);
                },
                [&](const ReclaimTarget& v) {
                    w.u8(2);
                    writeReclaimTarget(w, v);
                });
        }

        PatrolDivertTarget readPatrolDivertTarget(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return PatrolDivertTarget(PatrolAssistTarget{readId<UnitIdTag>(r)});
                case 1:
                    return PatrolDivertTarget(PatrolRepairTarget{readId<UnitIdTag>(r)});
                case 2:
                    return PatrolDivertTarget(readReclaimTarget(r));
                default:
                    throw SaveFormatException("bad PatrolDivertTarget tag");
            }
        }

        void writeUnitOrder(SaveWriter& w, const UnitOrder& o, std::uint32_t version)
        {
            match(
                o,
                [&](const MoveOrder& v) {
                    w.u8(0);
                    writeSimVector(w, v.destination);
                },
                [&](const AttackOrder& v) {
                    w.u8(1);
                    writeAttackTarget(w, v.target);
                },
                [&](const BuildOrder& v) {
                    w.u8(2);
                    w.string(v.unitType);
                    writeSimVector(w, v.position);
                },
                [&](const BuggerOffOrder& v) {
                    w.u8(3);
                    writeDiscreteRect(w, v.rect);
                },
                [&](const CompleteBuildOrder& v) {
                    w.u8(4);
                    writeId(w, v.target);
                },
                [&](const GuardOrder& v) {
                    w.u8(5);
                    writeId(w, v.target);
                },
                [&](const RepairOrder& v) {
                    w.u8(6);
                    writeId(w, v.target);
                },
                [&](const ReclaimOrder& v) {
                    w.u8(7);
                    writeReclaimTarget(w, v.target);
                },
                [&](const PatrolOrder& v) {
                    w.u8(8);
                    w.u32(static_cast<std::uint32_t>(v.waypoints.size()));
                    for (const auto& wp : v.waypoints)
                    {
                        writeSimVector(w, wp);
                    }
                    w.u32(v.nextWaypointIndex);
                    if (version >= 7)
                    {
                        w.u32(v.scanTicks);
                        w.boolean(v.divertTarget.has_value());
                        if (v.divertTarget)
                        {
                            writePatrolDivertTarget(w, *v.divertTarget);
                        }
                    }
                },
                [&](const WaitOrder& v) {
                    w.u8(9);
                    w.u32(v.ticksRemaining);
                },
                [&](const WaitForAttackOrder& v) {
                    w.u8(10);
                    w.boolean(v.target.has_value());
                    if (v.target)
                    {
                        writeId(w, *v.target);
                    }
                    w.boolean(v.armed);
                    writeGameTime(w, v.observedDamagedAt);
                },
                [&](const MakeSelectableOrder&) {
                    w.u8(11);
                },
                [&](const AttackUnitTypeOrder& v) {
                    w.u8(12);
                    w.string(v.unitType);
                    writeGameTime(w, v.nextScanTime);
                    w.boolean(v.currentTarget.has_value());
                    if (v.currentTarget)
                    {
                        writeId(w, *v.currentTarget);
                    }
                },
                [&](const MissionBuildOrder& v) {
                    w.u8(13);
                    w.string(v.unitType);
                    writeSimVector(w, v.position);
                    w.u32(v.attemptsRemaining);
                    writeGameTime(w, v.retryAt);
                },
                [&](const MissionFactoryBuildOrder& v) {
                    w.u8(14);
                    w.string(v.unitType);
                    w.u32(v.count);
                    w.boolean(v.enqueued);
                },
                [&](const SelfDestructOrder& v) {
                    w.u8(15);
                    w.u32(v.countdownTicks);
                    w.boolean(v.fuseTicks.has_value());
                    if (v.fuseTicks)
                    {
                        w.u32(*v.fuseTicks);
                    }
                },
                [&](const UnloadOrder& v) {
                    w.u8(16);
                    writeSimVector(w, v.destination);
                    w.u32(v.pops);
                    w.boolean(v.claimActive);
                    w.u32(v.cycleTicks);
                    writeSimScalar(w, v.dropRadius);
                },
                [&](const BuildWeaponOrder& v) {
                    w.u8(17);
                    w.u32(v.remaining);
                    w.u32(v.progressTicks);
                },
                [&](const CommandFireOrder& v) {
                    w.u8(18);
                    writeAttackTarget(w, v.target);
                    writeOptionalGameTime(w, v.observedReadyTime);
                },
                [&](const CaptureOrder& v) {
                    w.u8(19);
                    writeId(w, v.target);
                    w.u32(v.progress);
                    w.boolean(v.workRequired.has_value());
                    if (v.workRequired)
                    {
                        w.u32(*v.workRequired);
                    }
                    w.u32(v.workTicks);
                },
                [&](const LoadOrder& v) {
                    w.u8(20);
                    writeId(w, v.target);
                    w.u32(v.attempts);
                    w.u32(v.waitTicks);
                    w.boolean(v.pickupThreadStarted);
                    w.u32(v.phase);
                    w.i32(v.slot);
                },
                [&](const LandOrder& v) {
                    if (version < 12) { throw SaveFormatException("landing order requires save version 12"); }
                    w.u8(22);
                    writeId(w, v.target);
                },
                [&](const ParalyzeOrder& v) {
                    if (version < 9)
                    {
                        throw SaveFormatException("paralysis requires save format v9");
                    }
                    w.u8(21);
                    w.u32(v.credit);
                    writeOptionalGameTime(w, v.resumeAt);
                });
        }

        UnitOrder readUnitOrder(SaveReader& r, std::uint32_t version)
        {
            switch (r.u8())
            {
                case 0:
                    return UnitOrder(MoveOrder(readSimVector(r)));
                case 1:
                {
                    auto target = readAttackTarget(r);
                    return match(
                        target,
                        [&](UnitId id) { return UnitOrder(AttackOrder(id)); },
                        [&](const SimVector& pos) { return UnitOrder(AttackOrder(pos)); });
                }
                case 2:
                {
                    auto type = r.string();
                    auto pos = readSimVector(r);
                    return UnitOrder(BuildOrder(type, pos));
                }
                case 3:
                    return UnitOrder(BuggerOffOrder(readDiscreteRect(r)));
                case 4:
                    return UnitOrder(CompleteBuildOrder(readId<UnitIdTag>(r)));
                case 5:
                    return UnitOrder(GuardOrder(readId<UnitIdTag>(r)));
                case 6:
                    return UnitOrder(RepairOrder(readId<UnitIdTag>(r)));
                case 7:
                {
                    auto target = readReclaimTarget(r);
                    return match(
                        target,
                        [&](UnitId id) { return UnitOrder(ReclaimOrder(id)); },
                        [&](FeatureId id) { return UnitOrder(ReclaimOrder(id)); });
                }
                case 8:
                {
                    auto count = r.u32();
                    requireReadableElements(r, count);
                    std::vector<SimVector> waypoints;
                    waypoints.reserve(count);
                    for (unsigned int i = 0; i < count; ++i)
                    {
                        waypoints.push_back(readSimVector(r));
                    }
                    PatrolOrder order(std::move(waypoints));
                    order.nextWaypointIndex = r.u32();
                    if (order.nextWaypointIndex > order.waypoints.size())
                    {
                        throw SaveFormatException("patrol waypoint index out of range");
                    }
                    if (version >= 7)
                    {
                        order.scanTicks = r.u32();
                        if (r.boolean())
                        {
                            order.divertTarget = readPatrolDivertTarget(r);
                        }
                    }
                    return UnitOrder(std::move(order));
                }
                case 9:
                    return UnitOrder(WaitOrder(r.u32()));
                case 10:
                {
                    WaitForAttackOrder order(r.boolean() ? std::optional<UnitId>(readId<UnitIdTag>(r)) : std::nullopt);
                    order.armed = r.boolean();
                    order.observedDamagedAt = readGameTime(r);
                    return UnitOrder(std::move(order));
                }
                case 11:
                    return UnitOrder(MakeSelectableOrder());
                case 12:
                {
                    AttackUnitTypeOrder order(r.string());
                    order.nextScanTime = readGameTime(r);
                    if (r.boolean())
                    {
                        order.currentTarget = readId<UnitIdTag>(r);
                    }
                    return UnitOrder(std::move(order));
                }
                case 13:
                {
                    auto type = r.string();
                    auto pos = readSimVector(r);
                    MissionBuildOrder order(type, pos);
                    order.attemptsRemaining = r.u32();
                    order.retryAt = readGameTime(r);
                    return UnitOrder(std::move(order));
                }
                case 14:
                {
                    auto type = r.string();
                    MissionFactoryBuildOrder order(type, r.u32());
                    order.enqueued = r.boolean();
                    return UnitOrder(std::move(order));
                }
                case 15:
                {
                    SelfDestructOrder order(r.u32());
                    if (r.boolean())
                    {
                        order.fuseTicks = r.u32();
                    }
                    return UnitOrder(std::move(order));
                }
                case 16:
                {
                    UnloadOrder order(readSimVector(r));
                    order.pops = r.u32();
                    order.claimActive = r.boolean();
                    order.cycleTicks = r.u32();
                    order.dropRadius = readSimScalar(r);
                    return UnitOrder(std::move(order));
                }
                case 17:
                {
                    BuildWeaponOrder order(r.u32());
                    order.progressTicks = r.u32();
                    return UnitOrder(std::move(order));
                }
                case 18:
                {
                    auto target = readAttackTarget(r);
                    CommandFireOrder order = match(
                        target,
                        [](const UnitId& id) { return CommandFireOrder(id); },
                        [](const SimVector& v) { return CommandFireOrder(v); });
                    order.observedReadyTime = readOptionalGameTime(r);
                    return UnitOrder(std::move(order));
                }
                case 19:
                {
                    CaptureOrder order(readId<UnitIdTag>(r));
                    order.progress = r.u32();
                    if (r.boolean())
                    {
                        order.workRequired = r.u32();
                    }
                    order.workTicks = r.u32();
                    return UnitOrder(std::move(order));
                }
                case 20:
                {
                    LoadOrder order(readId<UnitIdTag>(r));
                    order.attempts = r.u32();
                    order.waitTicks = r.u32();
                    order.pickupThreadStarted = r.boolean();
                    order.phase = r.u32();
                    order.slot = r.i32();
                    return UnitOrder(std::move(order));
                }
                case 21:
                {
                    if (version < 9)
                    {
                        throw SaveFormatException("paralysis is not valid before save format v9");
                    }
                    ParalyzeOrder order;
                    order.credit = r.u32();
                    order.resumeAt = readOptionalGameTime(r);
                    return UnitOrder(order);
                }
                case 22:
                    if (version < 12) { throw SaveFormatException("landing order is not valid before save version 12"); }
                    return LandOrder(readId<UnitIdTag>(r));
                default:
                    throw SaveFormatException("bad UnitOrder tag");
            }
        }

        void writeOrderQueue(SaveWriter& w, const std::deque<UnitOrder>& orders, std::uint32_t version)
        {
            w.u32(static_cast<std::uint32_t>(orders.size()));
            for (const auto& o : orders)
            {
                writeUnitOrder(w, o, version);
            }
        }

        void readOrderQueue(SaveReader& r, std::deque<UnitOrder>& orders, std::uint32_t version)
        {
            auto count = r.u32();
            orders.clear();
            for (unsigned int i = 0; i < count; ++i)
            {
                orders.push_back(readUnitOrder(r, version));
            }
        }

        // ------------------------------------------------------------------
        // COB virtual-machine state
        // ------------------------------------------------------------------

        /** Serialize a std::stack bottom-to-top so a reader can push in order. */
        template <typename T, typename F>
        void writeStack(SaveWriter& w, std::stack<T> stack, F&& writeElement)
        {
            // std::stack has no forward iteration; copy-pop collects
            // top-to-bottom, so reverse before writing.
            std::vector<T> elements;
            while (!stack.empty())
            {
                elements.push_back(std::move(stack.top()));
                stack.pop();
            }
            w.u32(static_cast<std::uint32_t>(elements.size()));
            for (auto it = elements.rbegin(); it != elements.rend(); ++it)
            {
                writeElement(*it);
            }
        }

        void writeCobThread(SaveWriter& w, const CobThread& t)
        {
            w.string(t.name);
            w.u32(t.signalMask);
            writeStack<int>(w, t.stack, [&w](int v) { w.i32(v); });
            writeStack<CobFunction>(w, t.callStack, [&w](const CobFunction& f) {
                w.u32(f.instructionIndex);
                w.u32(f.localCount);
                w.u32(static_cast<std::uint32_t>(f.locals.size()));
                for (int v : f.locals)
                {
                    w.i32(v);
                }
            });
            w.i32(t.returnValue);
            w.u32(static_cast<std::uint32_t>(t.returnLocals.size()));
            for (int v : t.returnLocals)
            {
                w.i32(v);
            }
        }

        CobThread readCobThread(SaveReader& r)
        {
            CobThread t(r.string());
            t.signalMask = r.u32();
            auto valueDepth = r.u32();
            for (unsigned int i = 0; i < valueDepth; ++i)
            {
                t.stack.push(r.i32());
            }
            auto frameCount = r.u32();
            for (unsigned int i = 0; i < frameCount; ++i)
            {
                auto instructionIndex = r.u32();
                auto localCount = r.u32();
                auto localsSize = r.u32();
                requireReadableElements(r, localsSize);
                std::vector<int> locals(localsSize);
                for (unsigned int j = 0; j < localsSize; ++j)
                {
                    locals[j] = r.i32();
                }
                CobFunction f(instructionIndex, locals);
                f.localCount = localCount;
                t.callStack.push(std::move(f));
            }
            t.returnValue = r.i32();
            auto returnLocalsSize = r.u32();
            requireReadableElements(r, returnLocalsSize);
            t.returnLocals.resize(returnLocalsSize);
            for (unsigned int i = 0; i < returnLocalsSize; ++i)
            {
                t.returnLocals[i] = r.i32();
            }
            return t;
        }

        void writeCobBlockedCondition(SaveWriter& w, const CobEnvironment::BlockedStatus::Condition& c)
        {
            match(
                c,
                [&](const CobEnvironment::BlockedStatus::Move& m) {
                    w.u8(0);
                    w.u32(m.object);
                    w.u8(static_cast<std::uint8_t>(m.axis));
                },
                [&](const CobEnvironment::BlockedStatus::Turn& t) {
                    w.u8(1);
                    w.u32(t.object);
                    w.u8(static_cast<std::uint8_t>(t.axis));
                });
        }

        CobEnvironment::BlockedStatus::Condition readCobBlockedCondition(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return CobEnvironment::BlockedStatus::Condition(
                        CobEnvironment::BlockedStatus::Move(r.u32(), static_cast<CobAxis>(r.u8())));
                case 1:
                    return CobEnvironment::BlockedStatus::Condition(
                        CobEnvironment::BlockedStatus::Turn(r.u32(), static_cast<CobAxis>(r.u8())));
                default:
                    throw SaveFormatException("bad Cob blocked-condition tag");
            }
        }

        void writeCobEnvironment(SaveWriter& w, const CobEnvironment& env)
        {
            w.u32(static_cast<std::uint32_t>(env._statics.size()));
            for (int v : env._statics)
            {
                w.i32(v);
            }

            // thread index = position in env.threads; queues serialize as
            // ordered index lists so raw pointers never enter the file
            std::unordered_map<const CobThread*, std::uint32_t> threadIndex;
            w.u32(static_cast<std::uint32_t>(env.threads.size()));
            for (Index i = 0; i < getSize(env.threads); ++i)
            {
                threadIndex[env.threads[i].get()] = static_cast<std::uint32_t>(i);
                writeCobThread(w, *env.threads[i]);
            }

            w.u32(static_cast<std::uint32_t>(env.readyQueue.size()));
            for (auto* t : env.readyQueue)
            {
                w.u32(threadIndex.at(t));
            }

            w.u32(static_cast<std::uint32_t>(env.blockedQueue.size()));
            for (const auto& [status, t] : env.blockedQueue)
            {
                w.u32(threadIndex.at(t));
                writeCobBlockedCondition(w, status.condition);
            }

            w.u32(static_cast<std::uint32_t>(env.sleepingQueue.size()));
            for (const auto& [wakeTime, t] : env.sleepingQueue)
            {
                w.u32(threadIndex.at(t));
                w.i32(wakeTime.value);
            }

            w.u32(static_cast<std::uint32_t>(env.finishedQueue.size()));
            for (auto* t : env.finishedQueue)
            {
                w.u32(threadIndex.at(t));
            }
        }

        void readCobEnvironment(SaveReader& r, CobEnvironment& env)
        {
            auto staticsSize = r.u32();
            requireReadableElements(r, staticsSize);
            env._statics.resize(staticsSize);
            for (unsigned int i = 0; i < staticsSize; ++i)
            {
                env._statics[i] = r.i32();
            }

            auto threadCount = r.u32();
            requireReadableElements(r, threadCount);
            env.threads.clear();
            env.threads.reserve(threadCount);
            for (unsigned int i = 0; i < threadCount; ++i)
            {
                env.threads.push_back(std::make_unique<CobThread>(readCobThread(r)));
            }

            auto threadAt = [&](std::uint32_t index) -> CobThread* {
                if (index >= env.threads.size())
                {
                    throw SaveFormatException("cob queue references out-of-range thread");
                }
                return env.threads[index].get();
            };

            env.readyQueue.clear();
            auto readyCount = r.u32();
            for (unsigned int i = 0; i < readyCount; ++i)
            {
                env.readyQueue.push_back(threadAt(r.u32()));
            }

            env.blockedQueue.clear();
            auto blockedCount = r.u32();
            for (unsigned int i = 0; i < blockedCount; ++i)
            {
                auto* t = threadAt(r.u32());
                auto condition = readCobBlockedCondition(r);
                env.blockedQueue.emplace_back(CobEnvironment::BlockedStatus(condition), t);
            }

            env.sleepingQueue.clear();
            auto sleepingCount = r.u32();
            for (unsigned int i = 0; i < sleepingCount; ++i)
            {
                auto* t = threadAt(r.u32());
                env.sleepingQueue.emplace_back(CobTime(r.i32()), t);
            }

            env.finishedQueue.clear();
            auto finishedCount = r.u32();
            for (unsigned int i = 0; i < finishedCount; ++i)
            {
                env.finishedQueue.push_back(threadAt(r.u32()));
            }
        }

        // ------------------------------------------------------------------
        // weapons
        // ------------------------------------------------------------------

        void writeWeaponAttackTarget(SaveWriter& w, const UnitWeaponAttackTarget& t)
        {
            match(
                t,
                [&](const UnitId& id) {
                    w.u8(0);
                    writeId(w, id);
                },
                [&](const SimVector& v) {
                    w.u8(1);
                    writeSimVector(w, v);
                },
                [&](const ProjectileId& id) {
                    w.u8(2);
                    writeId(w, id);
                });
        }

        UnitWeaponAttackTarget readWeaponAttackTarget(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return UnitWeaponAttackTarget(readId<UnitIdTag>(r));
                case 1:
                    return UnitWeaponAttackTarget(readSimVector(r));
                case 2:
                    return UnitWeaponAttackTarget(readId<ProjectileIdTag>(r));
                default:
                    throw SaveFormatException("bad weapon attack-target tag");
            }
        }

        void writeUnitWeapon(SaveWriter& w, const UnitWeapon& weapon, const std::unordered_map<const CobThread*, std::uint32_t>& threadIndex)
        {
            w.string(toUpper(weapon.weaponType));
            writeGameTime(w, weapon.readyTime);
            writeSimScalar(w, weapon.ballisticZOffset);
            w.u32(weapon.stockpileCount);

            match(
                weapon.state,
                [&](const UnitWeaponStateIdle&) {
                    w.u8(0);
                },
                [&](const UnitWeaponStateAttacking& s) {
                    w.u8(1);
                    writeWeaponAttackTarget(w, s.target);
                    match(
                        s.attackInfo,
                        [&](const UnitWeaponStateAttacking::IdleInfo&) {
                            w.u8(0);
                        },
                        [&](const UnitWeaponStateAttacking::AimInfo& a) {
                            w.u8(1);
                            auto it = threadIndex.find(a.thread);
                            if (it == threadIndex.end())
                            {
                                throw SaveFormatException("aim thread not owned by this unit's cob environment");
                            }
                            w.u32(it->second);
                            writeSimAngle(w, a.lastHeading);
                            writeSimAngle(w, a.lastPitch);
                        },
                        [&](const UnitWeaponStateAttacking::FireInfo& f) {
                            w.u8(2);
                            writeSimAngle(w, f.heading);
                            writeSimAngle(w, f.pitch);
                            writeSimVector(w, f.targetPosition);
                            w.boolean(f.firingPiece.has_value());
                            if (f.firingPiece)
                            {
                                w.i32(*f.firingPiece);
                            }
                            w.i32(f.burstsFired);
                            writeGameTime(w, f.readyTime);
                        });
                });
        }

        UnitWeapon readUnitWeapon(SaveReader& r, const GameSimulation& sim, const std::vector<std::unique_ptr<CobThread>>& threads)
        {
            UnitWeapon weapon;
            // weaponDefinitions keys are uppercase
            weapon.weaponType = toUpper(r.string());
            if (sim.weaponDefinitions.find(weapon.weaponType) == sim.weaponDefinitions.end())
            {
                throw SaveFormatException("save references unknown weapon '" + weapon.weaponType + "'");
            }
            weapon.readyTime = readGameTime(r);
            weapon.ballisticZOffset = readSimScalar(r);
            weapon.stockpileCount = r.u32();

            switch (r.u8())
            {
                case 0:
                    weapon.state = UnitWeaponStateIdle();
                    break;
                case 1:
                {
                    UnitWeaponStateAttacking attacking(readWeaponAttackTarget(r));
                    switch (r.u8())
                    {
                        case 0:
                            attacking.attackInfo = UnitWeaponStateAttacking::IdleInfo();
                            break;
                        case 1:
                        {
                            auto threadIdx = r.u32();
                            if (threadIdx >= threads.size())
                            {
                                throw SaveFormatException("weapon aim thread index out of range");
                            }
                            UnitWeaponStateAttacking::AimInfo info;
                            info.thread = threads[threadIdx].get();
                            info.lastHeading = readSimAngle(r);
                            info.lastPitch = readSimAngle(r);
                            attacking.attackInfo = info;
                            break;
                        }
                        case 2:
                        {
                            UnitWeaponStateAttacking::FireInfo info;
                            info.heading = readSimAngle(r);
                            info.pitch = readSimAngle(r);
                            info.targetPosition = readSimVector(r);
                            if (r.boolean())
                            {
                                info.firingPiece = r.i32();
                            }
                            info.burstsFired = r.i32();
                            info.readyTime = readGameTime(r);
                            attacking.attackInfo = info;
                            break;
                        }
                        default:
                            throw SaveFormatException("bad weapon attack-info tag");
                    }
                    weapon.state = std::move(attacking);
                    break;
                }
                default:
                    throw SaveFormatException("bad weapon state tag");
            }
            return weapon;
        }

        // ------------------------------------------------------------------
        // physics / navigation / behaviour
        // ------------------------------------------------------------------

        void writeUnitPhysics(SaveWriter& w, const UnitPhysicsInfo& p)
        {
            match(
                p,
                [&](const UnitPhysicsInfoGround& g) {
                    w.u8(0);
                    writeSimAngle(w, g.steeringInfo.targetAngle);
                    writeSimScalar(w, g.steeringInfo.targetSpeed);
                    w.boolean(g.steeringInfo.shouldTakeOff);
                    writeSimScalar(w, g.currentSpeed);
                },
                [&](const UnitPhysicsInfoAir& a) {
                    w.u8(1);
                    match(
                        a.movementState,
                        [&](const AirMovementStateTakingOff&) {
                            w.u8(0);
                        },
                        [&](const AirMovementStateFlying& f) {
                            w.u8(1);
                            writeOptionalSimVector(w, f.targetPosition);
                            w.boolean(f.shouldLand);
                            writeSimVector(w, f.currentVelocity);
                        },
                        [&](const AirMovementStateLanding& l) {
                            w.u8(2);
                            w.boolean(l.landingFailed);
                            w.boolean(l.shouldAbort);
                        });
                });
        }

        UnitPhysicsInfo readUnitPhysics(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                {
                    UnitPhysicsInfoGround g;
                    g.steeringInfo.targetAngle = readSimAngle(r);
                    g.steeringInfo.targetSpeed = readSimScalar(r);
                    g.steeringInfo.shouldTakeOff = r.boolean();
                    g.currentSpeed = readSimScalar(r);
                    return UnitPhysicsInfo(g);
                }
                case 1:
                {
                    UnitPhysicsInfoAir a;
                    switch (r.u8())
                    {
                        case 0:
                            a.movementState = AirMovementStateTakingOff();
                            break;
                        case 1:
                        {
                            AirMovementStateFlying f;
                            f.targetPosition = readOptionalSimVector(r);
                            f.shouldLand = r.boolean();
                            f.currentVelocity = readSimVector(r);
                            a.movementState = f;
                            break;
                        }
                        case 2:
                        {
                            AirMovementStateLanding l;
                            l.landingFailed = r.boolean();
                            l.shouldAbort = r.boolean();
                            a.movementState = l;
                            break;
                        }
                        default:
                            throw SaveFormatException("bad air movement-state tag");
                    }
                    return UnitPhysicsInfo(a);
                }
                default:
                    throw SaveFormatException("bad physics tag");
            }
        }

        void writeNavigationGoal(SaveWriter& w, const NavigationGoal& g)
        {
            match(
                g,
                [&](const UnitId& id) {
                    w.u8(0);
                    writeId(w, id);
                },
                [&](const SimVector& v) {
                    w.u8(1);
                    writeSimVector(w, v);
                },
                [&](const DiscreteRect& d) {
                    w.u8(2);
                    writeDiscreteRect(w, d);
                },
                [&](const NavigationGoalLandingLocation&) {
                    w.u8(3);
                });
        }

        NavigationGoal readNavigationGoal(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return NavigationGoal(readId<UnitIdTag>(r));
                case 1:
                    return NavigationGoal(readSimVector(r));
                case 2:
                    return NavigationGoal(readDiscreteRect(r));
                case 3:
                    return NavigationGoal(NavigationGoalLandingLocation());
                default:
                    throw SaveFormatException("bad NavigationGoal tag");
            }
        }

        void writeMovingStateGoal(SaveWriter& w, const MovingStateGoal& g)
        {
            match(
                g,
                [&](const UnitId& id) {
                    w.u8(0);
                    writeId(w, id);
                },
                [&](const SimVector& v) {
                    w.u8(1);
                    writeSimVector(w, v);
                },
                [&](const DiscreteRect& d) {
                    w.u8(2);
                    writeDiscreteRect(w, d);
                });
        }

        MovingStateGoal readMovingStateGoal(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return MovingStateGoal(readId<UnitIdTag>(r));
                case 1:
                    return MovingStateGoal(readSimVector(r));
                case 2:
                    return MovingStateGoal(readDiscreteRect(r));
                default:
                    throw SaveFormatException("bad MovingStateGoal tag");
            }
        }

        void writePathDestination(SaveWriter& w, const PathDestination& d)
        {
            match(
                d,
                [&](const SimVector& v) {
                    w.u8(0);
                    writeSimVector(w, v);
                },
                [&](const DiscreteRect& dr) {
                    w.u8(1);
                    writeDiscreteRect(w, dr);
                });
        }

        PathDestination readPathDestination(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return PathDestination(readSimVector(r));
                case 1:
                    return PathDestination(readDiscreteRect(r));
                default:
                    throw SaveFormatException("bad PathDestination tag");
            }
        }

        void writeNavigationState(SaveWriter& w, const NavigationStateInfo& info)
        {
            w.boolean(info.desiredDestination.has_value());
            if (info.desiredDestination)
            {
                writeNavigationGoal(w, *info.desiredDestination);
            }

            w.boolean(info.unitPositionCache.has_value());
            if (info.unitPositionCache)
            {
                writeId(w, info.unitPositionCache->unitId);
                writeSimVector(w, info.unitPositionCache->position);
                writeGameTime(w, info.unitPositionCache->cachedAtTime);
            }

            match(
                info.state,
                [&](const NavigationStateIdle&) {
                    w.u8(0);
                },
                [&](const NavigationStateMoving& m) {
                    w.u8(1);
                    writeMovingStateGoal(w, m.movementGoal);
                    writePathDestination(w, m.pathDestination);
                    w.boolean(m.path.has_value());
                    if (m.path)
                    {
                        writeGameTime(w, m.path->pathCreationTime);
                        const auto& waypoints = m.path->path.waypoints;
                        w.u32(static_cast<std::uint32_t>(waypoints.size()));
                        for (const auto& wp : waypoints)
                        {
                            writeSimVector(w, wp);
                        }
                        w.boolean(m.path->path.isPartial);
                        // currentWaypoint is an iterator into waypoints;
                        // serialize it as an index
                        auto offset = std::distance(waypoints.begin(), m.path->currentWaypoint);
                        w.u32(static_cast<std::uint32_t>(offset));
                    }
                    w.boolean(m.pathRequested);
                    w.boolean(m.pathExhausted);
                    writeGameTime(w, m.pathExhaustedAt);
                },
                [&](const NavigationStateMovingToLandingSpot& m) {
                    w.u8(2);
                    writeSimVector(w, m.landingLocation);
                });
        }

        NavigationStateInfo readNavigationState(SaveReader& r, std::uint32_t version)
        {
            NavigationStateInfo info;

            if (r.boolean())
            {
                info.desiredDestination = readNavigationGoal(r);
            }

            if (r.boolean())
            {
                UnitPositionCache c;
                c.unitId = readId<UnitIdTag>(r);
                c.position = readSimVector(r);
                c.cachedAtTime = readGameTime(r);
                info.unitPositionCache = c;
            }

            switch (r.u8())
            {
                case 0:
                    info.state = NavigationStateIdle();
                    break;
                case 1:
                {
                    NavigationStateMoving m;
                    m.movementGoal = readMovingStateGoal(r);
                    m.pathDestination = readPathDestination(r);
                    if (r.boolean())
                    {
                        auto pathCreationTime = readGameTime(r);
                        auto waypointCount = r.u32();
                        requireReadableElements(r, waypointCount);
                        UnitPath path;
                        path.waypoints.reserve(waypointCount);
                        for (unsigned int i = 0; i < waypointCount; ++i)
                        {
                            path.waypoints.push_back(readSimVector(r));
                        }
                        path.isPartial = r.boolean();
                        auto waypointIndex = r.u32();
                        if (waypointIndex > waypointCount)
                        {
                            throw SaveFormatException("path waypoint index out of range");
                        }
                        PathFollowingInfo pfi(std::move(path), pathCreationTime);
                        pfi.currentWaypoint = pfi.path.waypoints.begin() + waypointIndex;
                        m.path = std::move(pfi);
                    }
                    m.pathRequested = r.boolean();
                    m.pathExhausted = r.boolean();
                    m.pathExhaustedAt = version >= 3 ? readGameTime(r) : GameTime(0);
                    info.state = std::move(m);
                    break;
                }
                case 2:
                {
                    NavigationStateMovingToLandingSpot m;
                    m.landingLocation = readSimVector(r);
                    info.state = m;
                    break;
                }
                default:
                    throw SaveFormatException("bad NavigationState tag");
            }
            return info;
        }

        void writeUnitCreationStatus(SaveWriter& w, const UnitCreationStatus& s)
        {
            match(
                s,
                [&](const UnitCreationStatusPending&) {
                    w.u8(0);
                },
                [&](const UnitCreationStatusFailed&) {
                    w.u8(1);
                },
                [&](const UnitCreationStatusDone& d) {
                    w.u8(2);
                    writeId(w, d.unitId);
                });
        }

        UnitCreationStatus readUnitCreationStatus(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return UnitCreationStatus(UnitCreationStatusPending());
                case 1:
                    return UnitCreationStatus(UnitCreationStatusFailed());
                case 2:
                    return UnitCreationStatus(UnitCreationStatusDone{readId<UnitIdTag>(r)});
                default:
                    throw SaveFormatException("bad UnitCreationStatus tag");
            }
        }

        void writeBehaviourState(SaveWriter& w, const UnitBehaviorState& s)
        {
            match(
                s,
                [&](const UnitBehaviorStateIdle&) {
                    w.u8(0);
                },
                [&](const UnitBehaviorStateCreatingUnit& c) {
                    w.u8(1);
                    w.string(c.unitType);
                    writeId(w, c.owner);
                    writeSimVector(w, c.position);
                    writeUnitCreationStatus(w, c.status);
                },
                [&](const UnitBehaviorStateBuilding& b) {
                    w.u8(2);
                    writeId(w, b.targetUnit);
                    writeOptionalSimVector(w, b.nanoParticleOrigin);
                },
                [&](const UnitBehaviorStateRepairing& rp) {
                    w.u8(3);
                    writeId(w, rp.targetUnit);
                    writeOptionalSimVector(w, rp.nanoParticleOrigin);
                    w.u32(rp.repairProgress);
                },
                [&](const UnitBehaviorStateReclaiming& rc) {
                    w.u8(4);
                    writeReclaimTarget(w, rc.target);
                    writeSimVector(w, rc.targetPosition);
                    writeOptionalSimVector(w, rc.nanoParticleOrigin);
                },
                [&](const UnitBehaviorStateCapturing& c) {
                    w.u8(5);
                    writeId(w, c.target);
                    writeSimVector(w, c.targetPosition);
                    writeOptionalSimVector(w, c.nanoParticleOrigin);
                });
        }

        UnitBehaviorState readBehaviourState(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return UnitBehaviorState(UnitBehaviorStateIdle());
                case 1:
                {
                    UnitBehaviorStateCreatingUnit c;
                    c.unitType = r.string();
                    c.owner = readId<PlayerIdTag>(r);
                    c.position = readSimVector(r);
                    c.status = readUnitCreationStatus(r);
                    return UnitBehaviorState(c);
                }
                case 2:
                {
                    UnitBehaviorStateBuilding b;
                    b.targetUnit = readId<UnitIdTag>(r);
                    b.nanoParticleOrigin = readOptionalSimVector(r);
                    return UnitBehaviorState(b);
                }
                case 3:
                {
                    UnitBehaviorStateRepairing rp;
                    rp.targetUnit = readId<UnitIdTag>(r);
                    rp.nanoParticleOrigin = readOptionalSimVector(r);
                    rp.repairProgress = r.u32();
                    return UnitBehaviorState(rp);
                }
                case 4:
                {
                    UnitBehaviorStateReclaiming rc;
                    rc.target = readReclaimTarget(r);
                    rc.targetPosition = readSimVector(r);
                    rc.nanoParticleOrigin = readOptionalSimVector(r);
                    return UnitBehaviorState(rc);
                }
                case 5:
                {
                    UnitBehaviorStateCapturing c;
                    c.target = readId<UnitIdTag>(r);
                    c.targetPosition = readSimVector(r);
                    c.nanoParticleOrigin = readOptionalSimVector(r);
                    return UnitBehaviorState(c);
                }
                default:
                    throw SaveFormatException("bad UnitBehaviorState tag");
            }
        }

        void writeFactoryState(SaveWriter& w, const FactoryBehaviorState& s)
        {
            match(
                s,
                [&](const FactoryBehaviorStateIdle&) {
                    w.u8(0);
                },
                [&](const FactoryBehaviorStateCreatingUnit& c) {
                    w.u8(1);
                    w.string(c.unitType);
                    writeId(w, c.owner);
                    writeSimVector(w, c.position);
                    writeSimAngle(w, c.rotation);
                    writeUnitCreationStatus(w, c.status);
                },
                [&](const FactoryBehaviorStateBuilding& b) {
                    w.u8(2);
                    w.boolean(b.targetUnit.has_value());
                    if (b.targetUnit)
                    {
                        writeId(w, b.targetUnit->first);
                        writeOptionalSimVector(w, b.targetUnit->second);
                    }
                });
        }

        FactoryBehaviorState readFactoryState(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return FactoryBehaviorState(FactoryBehaviorStateIdle());
                case 1:
                {
                    FactoryBehaviorStateCreatingUnit c;
                    c.unitType = r.string();
                    c.owner = readId<PlayerIdTag>(r);
                    c.position = readSimVector(r);
                    c.rotation = readSimAngle(r);
                    c.status = readUnitCreationStatus(r);
                    return FactoryBehaviorState(c);
                }
                case 2:
                {
                    FactoryBehaviorStateBuilding b;
                    if (r.boolean())
                    {
                        auto id = readId<UnitIdTag>(r);
                        auto origin = readOptionalSimVector(r);
                        b.targetUnit = std::make_pair(id, origin);
                    }
                    return FactoryBehaviorState(b);
                }
                default:
                    throw SaveFormatException("bad FactoryBehaviorState tag");
            }
        }

        // ------------------------------------------------------------------
        // unit meshes (piece transforms + in-flight move/turn ops)
        // ------------------------------------------------------------------

        void writeMoveOperation(SaveWriter& w, const UnitMesh::MoveOperation& op)
        {
            writeSimScalar(w, op.targetPosition);
            writeSimScalar(w, op.speed);
        }

        UnitMesh::MoveOperation readMoveOperation(SaveReader& r)
        {
            return UnitMesh::MoveOperation(readSimScalar(r), readSimScalar(r));
        }

        void writeOptionalMoveOperation(SaveWriter& w, const std::optional<UnitMesh::MoveOperation>& op)
        {
            w.boolean(op.has_value());
            if (op)
            {
                writeMoveOperation(w, *op);
            }
        }

        std::optional<UnitMesh::MoveOperation> readOptionalMoveOperation(SaveReader& r)
        {
            if (!r.boolean())
            {
                return std::nullopt;
            }
            return readMoveOperation(r);
        }

        void writeTurnOperationUnion(SaveWriter& w, const UnitMesh::TurnOperationUnion& op)
        {
            match(
                op,
                [&](const UnitMesh::TurnOperation& t) {
                    w.u8(0);
                    writeSimAngle(w, t.targetAngle);
                    writeSimScalar(w, t.speed);
                },
                [&](const UnitMesh::SpinOperation& s) {
                    w.u8(1);
                    writeSimScalar(w, s.currentSpeed);
                    writeSimScalar(w, s.targetSpeed);
                    writeSimScalar(w, s.acceleration);
                },
                [&](const UnitMesh::StopSpinOperation& s) {
                    w.u8(2);
                    writeSimScalar(w, s.currentSpeed);
                    writeSimScalar(w, s.deceleration);
                });
        }

        UnitMesh::TurnOperationUnion readTurnOperationUnion(SaveReader& r)
        {
            switch (r.u8())
            {
                case 0:
                    return UnitMesh::TurnOperationUnion(
                        UnitMesh::TurnOperation(readSimAngle(r), readSimScalar(r)));
                case 1:
                    return UnitMesh::TurnOperationUnion(
                        UnitMesh::SpinOperation(readSimScalar(r), readSimScalar(r), readSimScalar(r)));
                case 2:
                    return UnitMesh::TurnOperationUnion(
                        UnitMesh::StopSpinOperation(readSimScalar(r), readSimScalar(r)));
                default:
                    throw SaveFormatException("bad turn-operation tag");
            }
        }

        void writeOptionalTurnOperation(SaveWriter& w, const std::optional<UnitMesh::TurnOperationUnion>& op)
        {
            w.boolean(op.has_value());
            if (op)
            {
                writeTurnOperationUnion(w, *op);
            }
        }

        std::optional<UnitMesh::TurnOperationUnion> readOptionalTurnOperation(SaveReader& r)
        {
            if (!r.boolean())
            {
                return std::nullopt;
            }
            return readTurnOperationUnion(r);
        }

        void writeUnitMesh(SaveWriter& w, const UnitMesh& m)
        {
            w.string(m.name);
            w.boolean(m.visible);
            w.boolean(m.shaded);
            writeSimVector(w, m.offset);
            writeSimVector(w, m.previousOffset);
            writeSimAngle(w, m.rotationX);
            writeSimAngle(w, m.rotationY);
            writeSimAngle(w, m.rotationZ);
            writeSimAngle(w, m.previousRotationX);
            writeSimAngle(w, m.previousRotationY);
            writeSimAngle(w, m.previousRotationZ);
            writeOptionalMoveOperation(w, m.xMoveOperation);
            writeOptionalMoveOperation(w, m.yMoveOperation);
            writeOptionalMoveOperation(w, m.zMoveOperation);
            writeOptionalTurnOperation(w, m.xTurnOperation);
            writeOptionalTurnOperation(w, m.yTurnOperation);
            writeOptionalTurnOperation(w, m.zTurnOperation);
        }

        void readUnitMesh(SaveReader& r, UnitMesh& m)
        {
            m.name = r.string();
            m.visible = r.boolean();
            m.shaded = r.boolean();
            m.offset = readSimVector(r);
            m.previousOffset = readSimVector(r);
            m.rotationX = readSimAngle(r);
            m.rotationY = readSimAngle(r);
            m.rotationZ = readSimAngle(r);
            m.previousRotationX = readSimAngle(r);
            m.previousRotationY = readSimAngle(r);
            m.previousRotationZ = readSimAngle(r);
            m.xMoveOperation = readOptionalMoveOperation(r);
            m.yMoveOperation = readOptionalMoveOperation(r);
            m.zMoveOperation = readOptionalMoveOperation(r);
            m.xTurnOperation = readOptionalTurnOperation(r);
            m.yTurnOperation = readOptionalTurnOperation(r);
            m.zTurnOperation = readOptionalTurnOperation(r);
        }

        // ------------------------------------------------------------------
        // unit state
        // ------------------------------------------------------------------

        void writeUnitSlotHeader(SaveWriter& w, const UnitState& u)
        {
            // type/owner/position head the unit record so the loader can
            // construct the unit (pieces + script env come from data defs)
            // before reading the body
            w.string(u.unitType);
            writeId(w, u.owner);
            writeSimVector(w, u.position);
        }

        void writeUnitStateBody(SaveWriter& w, const UnitState& u, std::uint32_t version)
        {
            writeSimVector(w, u.previousPosition);
            writeSimAngle(w, u.rotation);
            writeSimAngle(w, u.previousRotation);
            writeUnitPhysics(w, u.physics);
            w.u32(u.hitPoints);
            match(
                u.lifeState,
                [&](const UnitState::LifeStateAlive&) {
                    w.u8(0);
                },
                [&](const UnitState::LifeStateDead& d) {
                    w.u8(1);
                    w.boolean(d.leaveCorpse);
                });

            writeOrderQueue(w, u.orders, version);
            writeBehaviourState(w, u.behaviourState);
            writeNavigationState(w, u.navigationState);

            writeOptionalId(w, u.buildOrderUnitId ? std::optional<unsigned int>(u.buildOrderUnitId->value) : std::nullopt);
            w.boolean(u.inBuildStance);
            w.boolean(u.yardOpen);
            w.boolean(u.inCollision);

            if (version >= 7)
            {
                w.boolean(u.armored);
                w.boolean(u.busy);
                w.boolean(u.buggerOff);
            }

            // cob state precedes weapons: weapon AimInfo references cob
            // threads by index, so the thread table must be restored first
            w.boolean(u.cobEnvironment != nullptr);
            if (u.cobEnvironment)
            {
                writeCobEnvironment(w, *u.cobEnvironment);
            }

            // weapons: all 3 slots written (absent slots as a flag)
            for (const auto& weaponSlot : u.weapons)
            {
                w.boolean(weaponSlot.has_value());
                if (weaponSlot)
                {
                    std::unordered_map<const CobThread*, std::uint32_t> threadIndex;
                    if (u.cobEnvironment)
                    {
                        for (Index i = 0; i < getSize(u.cobEnvironment->threads); ++i)
                        {
                            threadIndex[u.cobEnvironment->threads[i].get()] = static_cast<std::uint32_t>(i);
                        }
                    }
                    writeUnitWeapon(w, *weaponSlot, threadIndex);
                }
            }

            w.u8(static_cast<std::uint8_t>(u.fireOrders));
            w.u8(static_cast<std::uint8_t>(u.moveOrders));
            writeGameTime(w, u.lastDamagedAt);
            writeOptionalId(w, u.lastDamagedBy ? std::optional<unsigned int>(u.lastDamagedBy->value) : std::nullopt);
            writeOptionalId(w, u.lastDamagedByPlayer ? std::optional<unsigned int>(u.lastDamagedByPlayer->value) : std::nullopt);
            w.u32(u.buildTimeCompleted);

            w.boolean(u.missionState.has_value());
            if (u.missionState)
            {
                const auto& ms = *u.missionState;
                w.string(ms.initialMission);
                writeOptionalString(w, ms.ident);
                w.boolean(ms.selectable);
                writeOptionalId(w, ms.initialGroup);
                writeOptionalId(w, ms.creationCountdown);
                writeOptionalId(w, ms.kills);
                w.boolean(ms.immunity);
                w.boolean(ms.aiIgnore);
            }

            w.boolean(u.activated);
            w.boolean(u.isSufficientlyPowered);
            w.boolean(u.cloakRequested);
            w.boolean(u.cloaked);
            writeGameTime(w, u.cloakBlockedUntil);
            writeOpaqueUnit(w, u.energyProductionBuffer);
            writeOpaqueUnit(w, u.metalProductionBuffer);
            writeOpaqueUnit(w, u.previousEnergyConsumptionBuffer);
            writeOpaqueUnit(w, u.previousMetalConsumptionBuffer);
            writeOpaqueUnit(w, u.energyConsumptionBuffer);
            writeOpaqueUnit(w, u.metalConsumptionBuffer);

            writeOrderQueue(w, u.backgroundOrders, version);

            writeOptionalId(w, u.carriedBy ? std::optional<unsigned int>(u.carriedBy->value) : std::nullopt);
            w.i32(u.attachSlot);
            w.u32(static_cast<std::uint32_t>(u.passengers.size()));
            for (auto id : u.passengers)
            {
                writeId(w, id);
            }

            if (version >= 6)
            {
                w.boolean(u.selfDestruct.has_value());
                if (u.selfDestruct)
                {
                    w.u32(u.selfDestruct->countdownTicks);
                    w.boolean(u.selfDestruct->fuseTicks.has_value());
                    if (u.selfDestruct->fuseTicks)
                    {
                        w.u32(*u.selfDestruct->fuseTicks);
                    }
                }
            }

            if (version >= 10)
            {
                w.boolean(u.nextKamikazeScan.has_value());
                if (u.nextKamikazeScan) { writeGameTime(w, *u.nextKamikazeScan); }
            }
            else if (u.nextKamikazeScan)
            {
                throw SaveFormatException("kamikaze scan deadline requires save version 10");
            }

            if (version >= 11)
            {
                w.boolean(static_cast<bool>(u.bomberState));
                if (u.bomberState)
                {
                    const auto& b = *u.bomberState;
                    w.boolean(b.target.has_value());
                    if (b.target) { writeAttackTarget(w, *b.target); }
                    w.u32(static_cast<unsigned int>(b.phase));
                    writeSimVector(w, b.goal);
                    writeSimVector(w, b.releaseTarget);
                    w.boolean(b.nextScan.has_value());
                    if (b.nextScan) { writeGameTime(w, *b.nextScan); }
                }
            }
            else if (u.bomberState)
            {
                throw SaveFormatException("bomber state requires save version 11");
            }

            if (version >= 12)
            {
                writeOptionalGameTime(w, u.nextServiceScan);
                w.boolean(static_cast<bool>(u.aircraftService));
                if (u.aircraftService)
                {
                    const auto& s = *u.aircraftService;
                    writeId(w, s.facility);
                    w.i32(s.piece);
                    w.u32(static_cast<unsigned int>(s.phase));
                    w.u32(s.loiterBearing.value);
                    writeSimVector(w, s.goal);
                    w.boolean(s.automatic);
                }
            }
            else if (u.aircraftService || u.nextServiceScan)
            {
                throw SaveFormatException("aircraft service state requires save version 12");
            }

            if (version >= 14) { w.u16(u.kills); }
            else if (u.kills != 0) { throw SaveFormatException("unit veterancy requires save version 14"); }

            w.u32(static_cast<std::uint32_t>(u.buildQueue.size()));
            for (const auto& [type, count] : u.buildQueue)
            {
                w.string(type);
                w.i32(count);
            }

            writeFactoryState(w, u.factoryState);

            // pieces: order is the piece index; names are validated on load
            w.u32(static_cast<std::uint32_t>(u.pieces.size()));
            for (const auto& p : u.pieces)
            {
                writeUnitMesh(w, p);
            }
        }

        void readUnitStateBody(SaveReader& r, const GameSimulation& sim, UnitState& u, std::uint32_t version)
        {
            // unitType/owner/position were already consumed as the slot
            // header and applied by createUnit
            u.previousPosition = readSimVector(r);
            u.rotation = readSimAngle(r);
            u.previousRotation = readSimAngle(r);
            u.physics = readUnitPhysics(r);
            u.hitPoints = r.u32();
            switch (r.u8())
            {
                case 0:
                    u.lifeState = UnitState::LifeStateAlive();
                    break;
                case 1:
                    u.lifeState = UnitState::LifeStateDead{r.boolean()};
                    break;
                default:
                    throw SaveFormatException("bad unit life-state tag");
            }

            readOrderQueue(r, u.orders, version);
            u.behaviourState = readBehaviourState(r);
            u.navigationState = readNavigationState(r, version);

            auto buildOrderUnit = readOptionalU32(r);
            u.buildOrderUnitId = buildOrderUnit ? std::optional<UnitId>(UnitId(*buildOrderUnit)) : std::nullopt;
            u.inBuildStance = r.boolean();
            u.yardOpen = r.boolean();
            u.inCollision = r.boolean();

            if (version >= 7)
            {
                u.armored = r.boolean();
                u.busy = r.boolean();
                u.buggerOff = r.boolean();
            }

            // cob state precedes weapons (AimInfo thread indices)
            if (r.boolean())
            {
                if (!u.cobEnvironment)
                {
                    throw SaveFormatException("cob state present but unit has no script");
                }
                readCobEnvironment(r, *u.cobEnvironment);
            }
            else
            {
                u.cobEnvironment.reset();
            }

            for (auto& weaponSlot : u.weapons)
            {
                if (r.boolean())
                {
                    if (!u.cobEnvironment)
                    {
                        // weapons may reference cob threads; the env must
                        // already exist — created up-front by createUnit
                        throw SaveFormatException("unit weapon present but cob environment missing");
                    }
                    weaponSlot = readUnitWeapon(r, sim, u.cobEnvironment->threads);
                }
                else
                {
                    weaponSlot = std::nullopt;
                }
            }

            u.fireOrders = static_cast<UnitFireOrders>(r.u8());
            u.moveOrders = static_cast<UnitMoveOrders>(r.u8());
            u.lastDamagedAt = readGameTime(r);
            auto damagedBy = readOptionalU32(r);
            u.lastDamagedBy = damagedBy ? std::optional<UnitId>(UnitId(*damagedBy)) : std::nullopt;
            auto damagedByPlayer = readOptionalU32(r);
            u.lastDamagedByPlayer = damagedByPlayer ? std::optional<PlayerId>(PlayerId(*damagedByPlayer)) : std::nullopt;
            u.buildTimeCompleted = r.u32();

            if (r.boolean())
            {
                MissionUnitState ms;
                ms.initialMission = r.string();
                ms.ident = readOptionalString(r);
                ms.selectable = r.boolean();
                ms.initialGroup = readOptionalU32(r);
                ms.creationCountdown = readOptionalU32(r);
                ms.kills = readOptionalU32(r);
                ms.immunity = r.boolean();
                ms.aiIgnore = r.boolean();
                u.missionState = ms;
            }
            else
            {
                u.missionState = std::nullopt;
            }

            u.activated = r.boolean();
            u.isSufficientlyPowered = r.boolean();
            u.cloakRequested = r.boolean();
            u.cloaked = r.boolean();
            u.cloakBlockedUntil = readGameTime(r);
            u.energyProductionBuffer = readOpaqueUnit<float, EnergyTag>(r);
            u.metalProductionBuffer = readOpaqueUnit<float, MetalTag>(r);
            u.previousEnergyConsumptionBuffer = readOpaqueUnit<float, EnergyTag>(r);
            u.previousMetalConsumptionBuffer = readOpaqueUnit<float, MetalTag>(r);
            u.energyConsumptionBuffer = readOpaqueUnit<float, EnergyTag>(r);
            u.metalConsumptionBuffer = readOpaqueUnit<float, MetalTag>(r);

            readOrderQueue(r, u.backgroundOrders, version);

            auto carriedBy = readOptionalU32(r);
            u.carriedBy = carriedBy ? std::optional<UnitId>(UnitId(*carriedBy)) : std::nullopt;
            u.attachSlot = r.i32();
            auto passengerCount = r.u32();
            u.passengers.clear();
            for (unsigned int i = 0; i < passengerCount; ++i)
            {
                u.passengers.push_back(readId<UnitIdTag>(r));
            }

            if (version >= 6)
            {
                if (r.boolean())
                {
                    u.selfDestruct = SelfDestructOrder(r.u32());
                    if (r.boolean())
                    {
                        u.selfDestruct->fuseTicks = r.u32();
                    }
                }
            }

            u.nextKamikazeScan = std::nullopt;
            if (version >= 10 && r.boolean()) { u.nextKamikazeScan = readGameTime(r); }

            u.bomberState.reset();
            if (version >= 11 && r.boolean())
            {
                u.bomberState = std::make_unique<BomberState>();
                auto& b = *u.bomberState;
                if (r.boolean()) { b.target = readAttackTarget(r); }
                const auto phase = r.u32();
                if (phase > static_cast<unsigned int>(BomberState::Phase::BreakAway))
                {
                    throw SaveFormatException("invalid bomber phase");
                }
                b.phase = static_cast<BomberState::Phase>(phase);
                b.goal = readSimVector(r);
                b.releaseTarget = readSimVector(r);
                if (r.boolean()) { b.nextScan = readGameTime(r); }
            }

            u.aircraftService.reset();
            u.nextServiceScan = std::nullopt;
            if (version >= 12)
            {
                u.nextServiceScan = readOptionalGameTime(r);
                if (r.boolean())
                {
                    u.aircraftService = std::make_unique<AircraftServiceState>();
                    auto& s = *u.aircraftService;
                    s.facility = readId<UnitIdTag>(r);
                    s.piece = r.i32();
                    const auto phase = r.u32();
                    if (phase > static_cast<unsigned int>(AircraftServiceState::Phase::Depart))
                    { throw SaveFormatException("invalid aircraft service phase"); }
                    s.phase = static_cast<AircraftServiceState::Phase>(phase);
                    s.loiterBearing = SimAngle(r.u32());
                    s.goal = readSimVector(r);
                    s.automatic = r.boolean();
                }
            }

            u.kills = version >= 14 ? r.u16() : 0;
            auto buildQueueSize = r.u32();
            u.buildQueue.clear();
            for (unsigned int i = 0; i < buildQueueSize; ++i)
            {
                auto type = r.string();
                u.buildQueue.emplace_back(type, r.i32());
            }

            u.factoryState = readFactoryState(r);

            auto pieceCount = r.u32();
            if (pieceCount != u.pieces.size())
            {
                throw SaveFormatException("unit piece count does not match definition (data mismatch?)");
            }
            u.pieceNameToIndices.clear();
            for (Index i = 0; i < getSize(u.pieces); ++i)
            {
                readUnitMesh(r, u.pieces[i]);
                // keys must be uppercase: findPiece() queries
                // toUpper(name) and the UnitState ctor builds this map
                // through createPieceIndex() which uppercases too.
                // Restoring raw (lowercase 3DO) names here silently
                // breaks every COB piece command on restored units.
                u.pieceNameToIndices[toUpper(u.pieces[i].name)] = static_cast<int>(i);
            }
        }

        // ------------------------------------------------------------------
        // projectiles / features / players
        // ------------------------------------------------------------------

        void writeProjectile(SaveWriter& w, const Projectile& p, std::uint32_t version)
        {
            w.string(toUpper(p.weaponType));
            writeId(w, p.owner);
            writeSimVector(w, p.position);
            writeSimVector(w, p.previousPosition);
            writeSimVector(w, p.origin);
            writeSimVector(w, p.velocity);
            writeGameTime(w, p.lastSmoke);
            w.u32(static_cast<std::uint32_t>(p.damage.size()));
            for (const auto& [type, amount] : std::map<std::string, unsigned int>(p.damage.begin(), p.damage.end()))
            {
                w.string(type);
                w.u32(amount);
            }
            writeOptionalId(w, p.dieOnFrame ? std::optional<unsigned int>(p.dieOnFrame->value) : std::nullopt);
            writeSimScalar(w, p.damageRadius);
            w.boolean(p.groundBounce);
            w.boolean(p.isDead);
            writeGameTime(w, p.createdAt);
            writeOptionalId(w, p.targetUnit ? std::optional<unsigned int>(p.targetUnit->value) : std::nullopt);
            writeOptionalId(w, p.firedBy ? std::optional<unsigned int>(p.firedBy->value) : std::nullopt);
            if (version >= 7)
            {
                writeOptionalId(w, p.targetProjectile ? std::optional<unsigned int>(p.targetProjectile->value) : std::nullopt);
            }
            if (version >= 8)
            {
                w.boolean(p.targetPosition.has_value());
                if (p.targetPosition)
                {
                    writeSimVector(w, *p.targetPosition);
                }
            }
        }

        /** restoreSlots throws runtime_error; save format errors are SaveFormatException. */
        template <typename Map>
        void restoreSlotsChecked(Map& map, std::vector<typename Map::SlotState>&& slots, std::optional<unsigned int> firstFree)
        {
            try
            {
                map.restoreSlots(std::move(slots), firstFree);
            }
            catch (const std::runtime_error& e)
            {
                throw SaveFormatException(e.what());
            }
        }

        Projectile readProjectile(SaveReader& r, const GameSimulation& sim, std::uint32_t version)
        {
            Projectile p;
            // weaponDefinitions keys are uppercase
            p.weaponType = toUpper(r.string());
            if (sim.weaponDefinitions.find(p.weaponType) == sim.weaponDefinitions.end())
            {
                throw SaveFormatException("save references unknown weapon '" + p.weaponType + "'");
            }
            p.owner = readId<PlayerIdTag>(r);
            // PlayerIdNone is the environmental (meteor-shower) owner
            // sentinel and is intentionally not a player index
            if (p.owner != PlayerIdNone && p.owner.value >= sim.players.size())
            {
                throw SaveFormatException("projectile owner out of range");
            }
            p.position = readSimVector(r);
            p.previousPosition = readSimVector(r);
            p.origin = readSimVector(r);
            p.velocity = readSimVector(r);
            p.lastSmoke = readGameTime(r);
            auto damageCount = r.u32();
            for (unsigned int i = 0; i < damageCount; ++i)
            {
                auto type = r.string();
                p.damage[type] = r.u32();
            }
            auto dieOnFrame = readOptionalU32(r);
            p.dieOnFrame = dieOnFrame ? std::optional<GameTime>(GameTime(*dieOnFrame)) : std::nullopt;
            p.damageRadius = readSimScalar(r);
            p.groundBounce = r.boolean();
            p.isDead = r.boolean();
            p.createdAt = readGameTime(r);
            auto targetUnit = readOptionalU32(r);
            p.targetUnit = targetUnit ? std::optional<UnitId>(UnitId(*targetUnit)) : std::nullopt;
            auto firedBy = readOptionalU32(r);
            p.firedBy = firedBy ? std::optional<UnitId>(UnitId(*firedBy)) : std::nullopt;
            if (version >= 7)
            {
                auto targetProjectile = readOptionalU32(r);
                p.targetProjectile = targetProjectile ? std::optional<ProjectileId>(ProjectileId(*targetProjectile)) : std::nullopt;
            }
            if (version >= 8 && r.boolean())
            {
                p.targetPosition = readSimVector(r);
            }
            return p;
        }

        void writeMapFeature(SaveWriter& w, const MapFeature& f, const GameSimulation& sim, uint32_t version)
        {
            // features are identified by definition NAME, not slot id:
            // FeatureDefinitionId indexes a load-order-dependent table.
            // The name is canonicalized to upper case: the definition
            // table records authored case (e.g. featuredead=Tree2Dead)
            // while featureNameIndex keys are toUpper'd.
            w.string(toUpper(sim.getFeatureDefinition(f.featureName).name));
            writeSimVector(w, f.position);
            writeSimAngle(w, f.rotation);
            w.u32(f.reclaimWorkDone);
            w.u32(f.reclaimMetalGiven);
            w.u32(f.reclaimEnergyGiven);
            if (version >= 13)
            {
                w.u16(f.damageAccumulated);
                w.u8(static_cast<uint8_t>(f.event));
                w.u32(f.animationFrame); w.u32(f.animationDelay);
                w.u32(f.shadowFrame); w.u32(f.shadowDelay);
                w.u8(f.sparkCountdown);
            }
            else if (f.damageAccumulated != 0 || f.event != FeatureEvent::None)
            {
                throw SaveFormatException("feature damage and events require save version 13");
            }
        }

        MapFeature readMapFeature(SaveReader& r, GameSimulation& sim, uint32_t version)
        {
            auto name = r.string();
            // index keys are uppercase; saves written before
            // canonicalization carry authored-case names
            auto it = sim.featureNameIndex.find(toUpper(name));
            if (it == sim.featureNameIndex.end())
            {
                throw SaveFormatException("save references unknown feature '" + name + "'");
            }
            MapFeature f;
            f.featureName = it->second;
            f.position = readSimVector(r);
            f.rotation = readSimAngle(r);
            f.reclaimWorkDone = r.u32();
            f.reclaimMetalGiven = r.u32();
            f.reclaimEnergyGiven = r.u32();
            if (version >= 13)
            {
                f.damageAccumulated = r.u16();
                auto event = r.u8();
                if (event > static_cast<uint8_t>(FeatureEvent::Burning)) { throw SaveFormatException("invalid feature event"); }
                f.event = static_cast<FeatureEvent>(event);
                f.animationFrame = r.u32(); f.animationDelay = r.u32();
                f.shadowFrame = r.u32(); f.shadowDelay = r.u32();
                f.sparkCountdown = r.u8();
                if (f.event != FeatureEvent::None)
                {
                    const auto& anim = sim.getFeatureDefinition(f.featureName).animations[event - 1];
                    if (f.animationFrame >= anim.frames.size() || f.animationDelay > 65535
                        || f.shadowFrame > anim.shadows.size() || f.shadowDelay > 65535)
                    {
                        throw SaveFormatException("feature animation does not match loaded assets");
                    }
                }
            }
            return f;
        }

        void writePlayerInfo(SaveWriter& w, const GamePlayerInfo& p)
        {
            writeOptionalString(w, p.name);
            w.u8(static_cast<std::uint8_t>(p.type));
            w.u32(p.color.value);
            w.u8(static_cast<std::uint8_t>(p.status));
            w.string(p.side);
            writeOpaqueUnit(w, p.metal);
            writeOpaqueUnit(w, p.energy);
            writeOpaqueUnit(w, p.maxMetal);
            writeOpaqueUnit(w, p.maxEnergy);
            writeOpaqueUnit(w, p.startingMetal);
            writeOpaqueUnit(w, p.startingEnergy);
            w.boolean(p.metalStalled);
            w.boolean(p.energyStalled);
            writeOpaqueUnit(w, p.desiredMetalConsumptionBuffer);
            writeOpaqueUnit(w, p.desiredEnergyConsumptionBuffer);
            writeOpaqueUnit(w, p.previousDesiredMetalConsumptionBuffer);
            writeOpaqueUnit(w, p.previousDesiredEnergyConsumptionBuffer);
            writeOpaqueUnit(w, p.actualMetalConsumptionBuffer);
            writeOpaqueUnit(w, p.actualEnergyConsumptionBuffer);
            writeOpaqueUnit(w, p.metalProductionBuffer);
            writeOpaqueUnit(w, p.energyProductionBuffer);
            w.u32(p.unitsKilled);
            w.u32(p.unitsLost);
            w.f64bits(p.metalProduced);
            w.f64bits(p.energyProduced);
            w.f64bits(p.metalExcess);
            w.f64bits(p.energyExcess);
        }

        GamePlayerInfo readPlayerInfo(SaveReader& r)
        {
            GamePlayerInfo p;
            p.name = readOptionalString(r);
            auto type = r.u8();
            if (type > 1)
            {
                throw SaveFormatException("bad player type");
            }
            p.type = static_cast<GamePlayerType>(type);
            p.color = PlayerColorIndex(r.u32());
            auto status = r.u8();
            if (status > 1)
            {
                throw SaveFormatException("bad player status");
            }
            p.status = static_cast<GamePlayerStatus>(status);
            p.side = r.string();
            p.metal = readOpaqueUnit<float, MetalTag>(r);
            p.energy = readOpaqueUnit<float, EnergyTag>(r);
            p.maxMetal = readOpaqueUnit<float, MetalTag>(r);
            p.maxEnergy = readOpaqueUnit<float, EnergyTag>(r);
            p.startingMetal = readOpaqueUnit<float, MetalTag>(r);
            p.startingEnergy = readOpaqueUnit<float, EnergyTag>(r);
            p.metalStalled = r.boolean();
            p.energyStalled = r.boolean();
            p.desiredMetalConsumptionBuffer = readOpaqueUnit<float, MetalTag>(r);
            p.desiredEnergyConsumptionBuffer = readOpaqueUnit<float, EnergyTag>(r);
            p.previousDesiredMetalConsumptionBuffer = readOpaqueUnit<float, MetalTag>(r);
            p.previousDesiredEnergyConsumptionBuffer = readOpaqueUnit<float, EnergyTag>(r);
            p.actualMetalConsumptionBuffer = readOpaqueUnit<float, MetalTag>(r);
            p.actualEnergyConsumptionBuffer = readOpaqueUnit<float, EnergyTag>(r);
            p.metalProductionBuffer = readOpaqueUnit<float, MetalTag>(r);
            p.energyProductionBuffer = readOpaqueUnit<float, EnergyTag>(r);
            p.unitsKilled = r.u32();
            p.unitsLost = r.u32();
            p.metalProduced = r.f64bits();
            p.energyProduced = r.f64bits();
            p.metalExcess = r.f64bits();
            p.energyExcess = r.f64bits();
            return p;
        }

        // ------------------------------------------------------------------
        // slot maps (units / features / projectiles)
        // ------------------------------------------------------------------

        template <typename T, typename Tag, typename WriteFn>
        void writeSlotMap(SaveWriter& w, const VectorMap<T, Tag>& map, WriteFn&& writeElement)
        {
            // slot count includes free entries; occupied entries carry their
            // full id so index+generation survive round-trip exactly
            std::uint32_t slotCount = 0;
            map.forEachSlotState([&](auto, auto, auto, auto) { ++slotCount; });
            w.u32(slotCount);

            map.forEachSlotState([&](auto, auto slotId, const T* value, auto nextFree) {
                w.u32(slotId.value);
                w.boolean(value != nullptr);
                if (value)
                {
                    writeElement(*value);
                }
                else
                {
                    writeOptionalId(w, nextFree ? std::optional<unsigned int>(nextFree->value) : std::nullopt);
                }
            });
            writeOptionalId(w, map.firstFreeSlot());
        }

        // ------------------------------------------------------------------
        // mission info + objective evaluator state
        // ------------------------------------------------------------------

        void writeMissionInfo(SaveWriter& w, const MissionInfo& m)
        {
            w.string(m.campaignName);
            w.string(m.campaignSide);
            w.string(m.missionName);
            w.u32(m.missionIndex);
            w.string(m.missionFile);
            w.string(m.schemaType);
            w.string(m.aiProfile);
            writeId(w, m.localPlayerId);
            w.u32(static_cast<std::uint32_t>(m.objectives.size()));
            for (const auto& o : m.objectives)
            {
                w.string(o.type);
                w.u32(static_cast<std::uint32_t>(o.params.size()));
                for (const auto& p : o.params)
                {
                    w.string(p);
                }
            }
            w.string(m.brief);
            w.u32(static_cast<std::uint32_t>(m.useOnlyUnits.size()));
            for (const auto& s : std::set<std::string>(m.useOnlyUnits.begin(), m.useOnlyUnits.end()))
            {
                w.string(s);
            }
            w.i32(m.killMul);
            w.i32(m.timeMul);
        }

        MissionInfo readMissionInfo(SaveReader& r)
        {
            MissionInfo m;
            m.campaignName = r.string();
            m.campaignSide = r.string();
            m.missionName = r.string();
            m.missionIndex = r.u32();
            m.missionFile = r.string();
            m.schemaType = r.string();
            m.aiProfile = r.string();
            m.localPlayerId = readId<PlayerIdTag>(r);
            auto objCount = r.u32();
            for (unsigned int i = 0; i < objCount; ++i)
            {
                MissionObjective o;
                o.type = r.string();
                auto paramCount = r.u32();
                for (unsigned int j = 0; j < paramCount; ++j)
                {
                    o.params.push_back(r.string());
                }
                m.objectives.push_back(std::move(o));
            }
            m.brief = r.string();
            auto useOnlyCount = r.u32();
            for (unsigned int i = 0; i < useOnlyCount; ++i)
            {
                m.useOnlyUnits.insert(r.string());
            }
            m.killMul = r.i32();
            m.timeMul = r.i32();
            return m;
        }

        void writeObjectiveRuntime(SaveWriter& w, const MissionObjectiveRuntime& o)
        {
            w.string(o.key);
            w.u32(static_cast<std::uint32_t>(o.params.size()));
            for (const auto& p : o.params)
            {
                w.string(p);
            }
            w.u8(static_cast<std::uint8_t>(o.polarity));
            w.boolean(o.satisfied);
            w.i32(o.remainingCount);
        }

        MissionObjectiveRuntime readObjectiveRuntime(SaveReader& r)
        {
            MissionObjectiveRuntime o;
            o.key = r.string();
            auto paramCount = r.u32();
            for (unsigned int i = 0; i < paramCount; ++i)
            {
                o.params.push_back(r.string());
            }
            auto polarity = r.u8();
            if (polarity > 1)
            {
                throw SaveFormatException("bad objective polarity");
            }
            o.polarity = static_cast<MissionObjectivePolarity>(polarity);
            o.satisfied = r.boolean();
            o.remainingCount = r.i32();
            return o;
        }

        void writeObjectives(SaveWriter& w, const MissionObjectiveEvaluator& e)
        {
            writeId(w, e.getLocalPlayerId());
            const auto& victory = e.getVictoryObjectives();
            w.u32(static_cast<std::uint32_t>(victory.size()));
            for (const auto& o : victory)
            {
                writeObjectiveRuntime(w, o);
            }
            const auto& defeat = e.getDefeatObjectives();
            w.u32(static_cast<std::uint32_t>(defeat.size()));
            for (const auto& o : defeat)
            {
                writeObjectiveRuntime(w, o);
            }
            w.i32(e.getResultCountdown());
            w.u8(static_cast<std::uint8_t>(e.result()));
        }

        // ------------------------------------------------------------------
        // whole-simulation codec
        // ------------------------------------------------------------------

        void writeSimulationBody(SaveWriter& w, const GameSimulation& sim, const CampaignProgress* progress, const GameParameters& params, std::uint32_t version)
        {
            writeGameTime(w, sim.gameTime);

            // minstd_rand serializes portably via stream state text
            {
                std::ostringstream oss;
                oss << sim.rng;
                w.string(oss.str());
            }

            match(
                sim.gameStatus,
                [&](const WinStatusUndecided&) { w.u8(0); },
                [&](const WinStatusWon& s) {
                    w.u8(1);
                    writeId(w, s.winner);
                },
                [&](const WinStatusDraw&) { w.u8(2); });

            w.u32(static_cast<std::uint32_t>(sim.players.size()));
            for (const auto& p : sim.players)
            {
                writePlayerInfo(w, p);
            }

            w.u32(static_cast<std::uint32_t>(sim.playerVisibility.size()));
            for (const auto& pv : sim.playerVisibility)
            {
                // losVisibleUnits/radar/sonar sets and losGrid rebuild on the
                // first updateVisibility pass post-load; exploredGrid is the
                // accumulated fog memory and must persist
                writeU8Grid(w, pv.exploredGrid);
            }

            writeU8Grid(w, sim.metalGrid);

            {
                const auto& g = sim.geoGrid;
                w.i32(g.getWidth());
                w.i32(g.getHeight());
                const auto& data = g.getVector();
                w.u32(static_cast<std::uint32_t>(data.size()));
                for (bool v : data)
                {
                    w.u8(v ? 1 : 0);
                }
            }

            // features slot map
            writeSlotMap<MapFeature, FeatureIdTag>(w, sim.features, [&](const MapFeature& f) {
                writeMapFeature(w, f, sim, version);
            });

            if (version >= 13)
            {
                w.u32(static_cast<uint32_t>(sim.activeFeatures.size()));
                for (auto id : sim.activeFeatures) { writeId(w, id); }
            }

            // units slot map
            writeSlotMap<UnitState, UnitIdTag>(w, sim.units, [&](const UnitState& u) {
                writeUnitSlotHeader(w, u);
                writeUnitStateBody(w, u, version);
            });

            // projectiles slot map
            writeSlotMap<Projectile, ProjectileIdTag>(w, sim.projectiles, [&](const Projectile& p) {
                writeProjectile(w, p, version);
            });

            w.u32(static_cast<std::uint32_t>(sim.pathRequests.size()));
            for (const auto& req : sim.pathRequests)
            {
                writeId(w, req.unitId);
            }

            w.u32(static_cast<std::uint32_t>(sim.unitCreationRequests.size()));
            for (auto id : sim.unitCreationRequests)
            {
                writeId(w, id);
            }

            writeSimScalar(w, sim.currentWindGenerationFactor);
            writeGameTime(w, sim.nextWindSpeedChange);
            if (version >= 11)
            {
                w.i32(sim.mapGravity);
                writeSimVector(w, sim.projectileWind);
            }
            else if (sim.projectileWind != SimVector(0_ss, 0_ss, 0_ss))
            {
                throw SaveFormatException("projectile wind requires save version 11");
            }

            w.boolean(sim.lineOfSightEnabled);
            w.i32(sim.lineOfSightMode);
            w.boolean(sim.mappingEnabled);

            if (version >= 7)
            {
                w.u32(sim.maxUnitsPerPlayer);
            }

            w.boolean(sim.missionInfo.has_value());
            if (sim.missionInfo)
            {
                writeMissionInfo(w, *sim.missionInfo);
            }

            w.boolean(sim.missionObjectives.has_value());
            if (sim.missionObjectives)
            {
                writeObjectives(w, *sim.missionObjectives);
            }

            // retail `Thumbs`: the active campaign run's W/L/U array.
            // Only embedded when this save is for a campaign mission.
            if (progress != nullptr && params.campaign)
            {
                w.boolean(true);
                w.string(params.campaign->campaignName);
                w.string(params.campaign->campaignSide);
                const auto* run = progress->findRun(params.campaign->campaignName, params.campaign->campaignSide);
                const auto& status = run != nullptr ? run->status : std::vector<CampaignMissionStatus>();
                w.u32(static_cast<std::uint32_t>(status.size()));
                for (auto s : status)
                {
                    w.u8(static_cast<std::uint8_t>(s));
                }
            }
            else
            {
                w.boolean(false);
            }

            // campaign AI states (format v2): resolved weight/limit
            // tables, task timers, group membership and squad target.
            // unitIndex is not stored -- the deterministic sorted index
            // is rebuilt from the loaded definitions.
            w.u32(static_cast<std::uint32_t>(sim.campaignAi.size()));
            for (const auto& st : sim.campaignAi)
            {
                if (version < 16)
                {
                    for (const auto& [id, dl] : st.deadlockedFactories)
                    {
                        if (dl.reclaimStalledAt || dl.reclaimBestDistance || dl.reclaimDeadline
                            || !dl.reclaimAvoid.empty() || !dl.evictAttempts.empty())
                        { throw SaveFormatException("AI recovery history requires save version 16"); }
                    }
                }
                writeId(w, st.player);
                w.u32(static_cast<std::uint32_t>(st.unitWeights.size()));
                for (auto v : st.unitWeights)
                {
                    w.i32(v);
                }
                w.u32(static_cast<std::uint32_t>(st.unitLimits.size()));
                for (auto v : st.unitLimits)
                {
                    w.i32(v);
                }
                writeGameTime(w, st.nextRepartition);
                writeGameTime(w, st.nextFactoryScan);
                writeGameTime(w, st.nextBuilderScan);
                writeGameTime(w, st.nextAttackScan);
                for (const auto& group : st.groups)
                {
                    w.u32(static_cast<std::uint32_t>(group.size()));
                    for (auto id : group)
                    {
                        writeId(w, id);
                    }
                }
                w.boolean(st.attackTarget.has_value());
                if (st.attackTarget)
                {
                    writeId(w, *st.attackTarget);
                }
                if (version >= 8)
                {
                    writeGameTime(w, st.nextDeadlockScan);
                    w.u32(static_cast<std::uint32_t>(st.deadlockedFactories.size()));
                    for (const auto& [factoryId, dl] : st.deadlockedFactories)
                    {
                        writeId(w, factoryId);
                        w.boolean(dl.blockedSince.has_value());
                        if (dl.blockedSince)
                        {
                            writeGameTime(w, *dl.blockedSince);
                        }
                        w.boolean(dl.reclaimTarget.has_value());
                        if (dl.reclaimTarget)
                        {
                            writeId(w, *dl.reclaimTarget);
                        }
                        w.boolean(dl.reclaimBuilder.has_value());
                        if (dl.reclaimBuilder)
                        {
                            writeId(w, *dl.reclaimBuilder);
                        }
                        w.boolean(dl.reclaimIssuedAt.has_value());
                        if (dl.reclaimIssuedAt)
                        {
                            writeGameTime(w, *dl.reclaimIssuedAt);
                        }
                        w.u32(dl.reclaimAttempts);
                        if (version >= 16)
                        {
                            writeOptionalGameTime(w, dl.reclaimStalledAt);
                            w.boolean(dl.reclaimBestDistance.has_value());
                            if (dl.reclaimBestDistance) { writeSimScalar(w, *dl.reclaimBestDistance); }
                            writeOptionalGameTime(w, dl.reclaimDeadline);
                            const std::set<unsigned int> avoid(dl.reclaimAvoid.begin(), dl.reclaimAvoid.end());
                            w.u32(static_cast<std::uint32_t>(avoid.size()));
                            for (auto id : avoid) { w.u32(id); }
                            const std::map<unsigned int, unsigned int> evictions(dl.evictAttempts.begin(), dl.evictAttempts.end());
                            w.u32(static_cast<std::uint32_t>(evictions.size()));
                            for (const auto& [id, count] : evictions) { w.u32(id); w.u32(count); }
                        }
                    }
                }
            }

            // meteor shower scheduler state (format v4) -- the mutable
            // half of the retail [Meteor] save block (TotalA.exe
            // 0x4192c0); the static config is re-derived from the OTA
            // schema when LoadingScene rebuilds the simulation
            {
                const auto& m = sim.meteorShower;
                w.boolean(m.enabled);
                w.boolean(m.active);
                writeGameTime(w, m.nextStrikeTime);
                writeGameTime(w, m.timeStrikeEnds);
                writeGameTime(w, m.nextHitTime);
                w.i32(m.originX);
                w.i32(m.originZ);
                w.i32(m.targetX);
                w.i32(m.targetZ);
            }

            // skirmish runtime settings (format v5): the CommanderDeath
            // mode and computer economy scale steer deterministic
            // future state; ally groups come from GamePlayerInfo and
            // must ride the body, not just the metadata player table
            if (version >= 5)
            {
                w.boolean(sim.commanderDeathContinues);
                w.f32bits(sim.computerResourceScale);
                w.u32(static_cast<std::uint32_t>(sim.players.size()));
                for (const auto& p : sim.players)
                {
                    w.boolean(p.allyGroup.has_value());
                    if (p.allyGroup)
                    {
                        w.u32(*p.allyGroup);
                    }
                }
            }
            if (version >= 15)
            {
                // This is history-bearing state, not a purely derived cache:
                // overlap departure can leave holes or a different cell owner.
                // Persist it only on Save; no simulation tick work is added.
                w.u32(sim.occupiedGrid.getWidth());
                w.u32(sim.occupiedGrid.getHeight());
                const auto& cells = sim.occupiedGrid.getVector();
                std::unordered_set<UnitId> mobileOwners;
                for (const auto& [id, unit] : sim.units)
                { if (sim.unitDefinitions.at(unit.unitType).isMobile) { mobileOwners.insert(id); } }
                std::uint32_t count = 0;
                for (const auto& cell : cells) { if (cell.mobileUnitId) { ++count; } }
                w.u32(count);
                for (std::size_t i = 0; i < cells.size(); ++i)
                {
                    if (!cells[i].mobileUnitId) { continue; }
                    if (mobileOwners.count(*cells[i].mobileUnitId) == 0)
                    { throw SaveFormatException("mobile occupancy references missing unit"); }
                    w.u32(static_cast<std::uint32_t>(i));
                    writeId(w, *cells[i].mobileUnitId);
                }
            }

        }

        void readSimulationBody(SaveReader& r, GameSimulation& sim, CampaignProgress* progress, std::uint32_t version)
        {
            sim.gameTime = readGameTime(r);

            {
                auto rngText = r.string();
                std::istringstream iss(rngText);
                iss >> sim.rng;
                if (iss.fail())
                {
                    throw SaveFormatException("invalid rng state in save");
                }
            }

            switch (r.u8())
            {
                case 0:
                    sim.gameStatus = WinStatusUndecided();
                    break;
                case 1:
                    sim.gameStatus = WinStatusWon{readId<PlayerIdTag>(r)};
                    break;
                case 2:
                    sim.gameStatus = WinStatusDraw();
                    break;
                default:
                    throw SaveFormatException("bad game-status tag");
            }
            auto playerCount = r.u32();
            sim.players.clear();
            for (unsigned int i = 0; i < playerCount; ++i)
            {
                sim.players.push_back(readPlayerInfo(r));
            }
            auto visibilityCount = r.u32();
            // visibility tables are empty until the first updateVisibility
            // pass; any partial count is a corrupt save
            if (visibilityCount != 0 && visibilityCount != sim.players.size())
            {
                throw SaveFormatException("player visibility count does not match player count");
            }
            sim.playerVisibility.clear();
            sim.playerVisibility.resize(visibilityCount);
            for (unsigned int i = 0; i < visibilityCount; ++i)
            {
                sim.playerVisibility[i].exploredGrid = readU8Grid(r);
            }

            sim.metalGrid = readU8Grid(r);

            {
                auto width = r.i32();
                auto height = r.i32();
                auto size = r.u32();
                if (width < 0 || height < 0 || static_cast<long long>(width) * height != size
                    || width != sim.geoGrid.getWidth() || height != sim.geoGrid.getHeight())
                {
                    throw SaveFormatException("geo grid dimensions mismatch");
                }
                for (unsigned int i = 0; i < size; ++i)
                {
                    sim.geoGrid.set(i % width, i / width, r.u8() != 0);
                }
            }
            // ---- features
            {
                using Slot = VectorMap<MapFeature, FeatureIdTag>::SlotState;
                auto slotCount = r.u32();
                requireReadableElements(r, slotCount);
                std::vector<Slot> slots;
                slots.reserve(slotCount);
                for (unsigned int i = 0; i < slotCount; ++i)
                {
                    auto rawId = FeatureId(r.u32());
                    if (r.boolean())
                    {
                        slots.push_back(Slot(std::make_pair(rawId, readMapFeature(r, sim, version))));
                    }
                    else
                    {
                        typename VectorMap<MapFeature, FeatureIdTag>::FreeSlot fs;
                        fs.id = rawId;
                        fs.nextIndex = readOptionalU32(r);
                        slots.push_back(Slot(fs));
                    }
                }
                restoreSlotsChecked(sim.features, std::move(slots), readOptionalU32(r));
            }
            sim.activeFeatures.clear();
            if (version >= 13)
            {
                auto count = r.u32();
                requireReadableElements(r, count);
                std::unordered_set<FeatureId> seen;
                for (uint32_t i = 0; i < count; ++i)
                {
                    auto id = readId<FeatureIdTag>(r);
                    auto f = sim.tryGetFeature(id);
                    if (!f || f->get().event == FeatureEvent::None || !seen.insert(id).second)
                    {
                        throw SaveFormatException("invalid active feature identity");
                    }
                    sim.activeFeatures.push_back(id);
                }
                for (const auto& [id, f] : sim.features)
                {
                    if (f.event != FeatureEvent::None && seen.count(id) == 0)
                    {
                        throw SaveFormatException("missing active feature identity");
                    }
                }
            }
            // ---- units
            {
                using Slot = VectorMap<UnitState, UnitIdTag>::SlotState;
                using FreeSlot = VectorMap<UnitState, UnitIdTag>::FreeSlot;
                auto slotCount = r.u32();
                requireReadableElements(r, slotCount);
                std::vector<Slot> slots;
                slots.reserve(slotCount);
                for (unsigned int i = 0; i < slotCount; ++i)
                {
                    auto rawId = UnitId(r.u32());
                    if (r.boolean())
                    {
                        // slot header: construct pieces + script env from
                        // definitions, then overwrite the body
                        auto unitType = r.string();
                        auto owner = readId<PlayerIdTag>(r);
                        auto position = readSimVector(r);
                        if (owner.value >= sim.players.size())
                        {
                            throw SaveFormatException("unit owner out of range");
                        }
                        if (sim.unitDefinitions.find(unitType) == sim.unitDefinitions.end())
                        {
                            throw SaveFormatException("save references unknown unit '" + unitType + "'");
                        }
                        auto unit = createUnit(sim, unitType, owner, position, std::nullopt);
                        unit.owner = owner;
                        readUnitStateBody(r, sim, unit, version);
                        slots.push_back(Slot(std::make_pair(rawId, std::move(unit))));
                    }
                    else
                    {
                        FreeSlot fs;
                        fs.id = rawId;
                        fs.nextIndex = readOptionalU32(r);
                        slots.push_back(Slot(fs));
                    }
                }
                restoreSlotsChecked(sim.units, std::move(slots), readOptionalU32(r));
            }
            // ---- projectiles
            {
                using Slot = VectorMap<Projectile, ProjectileIdTag>::SlotState;
                using FreeSlot = VectorMap<Projectile, ProjectileIdTag>::FreeSlot;
                auto slotCount = r.u32();
                requireReadableElements(r, slotCount);
                std::vector<Slot> slots;
                slots.reserve(slotCount);
                for (unsigned int i = 0; i < slotCount; ++i)
                {
                    auto rawId = ProjectileId(r.u32());
                    if (r.boolean())
                    {
                        slots.push_back(Slot(std::make_pair(rawId, readProjectile(r, sim, version))));
                    }
                    else
                    {
                        FreeSlot fs;
                        fs.id = rawId;
                        fs.nextIndex = readOptionalU32(r);
                        slots.push_back(Slot(fs));
                    }
                }
                restoreSlotsChecked(sim.projectiles, std::move(slots), readOptionalU32(r));
            }

            {
                auto count = r.u32();
                sim.pathRequests.clear();
                for (unsigned int i = 0; i < count; ++i)
                {
                    sim.pathRequests.push_back(PathRequest{readId<UnitIdTag>(r)});
                }
            }

            {
                auto count = r.u32();
                sim.unitCreationRequests.clear();
                for (unsigned int i = 0; i < count; ++i)
                {
                    sim.unitCreationRequests.push_back(readId<UnitIdTag>(r));
                }
            }
            sim.currentWindGenerationFactor = readSimScalar(r);
            sim.nextWindSpeedChange = readGameTime(r);
            sim.projectileWind = SimVector(0_ss, 0_ss, 0_ss);
            if (version >= 11)
            {
                sim.mapGravity = r.i32();
                sim.projectileWind = readSimVector(r);
            }

            sim.lineOfSightEnabled = r.boolean();
            sim.lineOfSightMode = r.i32();
            sim.mappingEnabled = r.boolean();

            if (version >= 7)
            {
                sim.maxUnitsPerPlayer = r.u32();
            }

            if (r.boolean())
            {
                sim.missionInfo = readMissionInfo(r);
            }
            else
            {
                sim.missionInfo = std::nullopt;
            }

            if (r.boolean())
            {
                auto localPlayerId = readId<PlayerIdTag>(r);
                if (localPlayerId.value >= sim.players.size())
                {
                    throw SaveFormatException("mission objectives reference out-of-range player");
                }
                auto victoryCount = r.u32();
                std::vector<MissionObjectiveRuntime> victory;
                for (unsigned int i = 0; i < victoryCount; ++i)
                {
                    victory.push_back(readObjectiveRuntime(r));
                }
                auto defeatCount = r.u32();
                std::vector<MissionObjectiveRuntime> defeat;
                for (unsigned int i = 0; i < defeatCount; ++i)
                {
                    defeat.push_back(readObjectiveRuntime(r));
                }
                auto countdown = r.i32();
                auto resultTag = r.u8();
                if (resultTag > 2)
                {
                    throw SaveFormatException("bad mission result tag");
                }
                sim.missionObjectives.emplace(std::vector<MissionObjective>{}, localPlayerId);
                sim.missionObjectives->restoreRuntimeState(
                    std::move(victory), std::move(defeat), countdown, static_cast<MissionResult>(resultTag));
            }
            else
            {
                sim.missionObjectives = std::nullopt;
            }
            // campaign thumbs
            if (r.boolean())
            {
                auto campaignName = r.string();
                auto campaignSide = r.string();
                auto count = r.u32();
                requireReadableElements(r, count);
                std::vector<CampaignMissionStatus> statuses(count);
                for (unsigned int i = 0; i < count; ++i)
                {
                    auto v = r.u8();
                    if (v != 'U' && v != 'W' && v != 'L')
                    {
                        throw SaveFormatException("bad campaign mission status byte in save");
                    }
                    statuses[i] = static_cast<CampaignMissionStatus>(v);
                }
                if (progress != nullptr)
                {
                    progress->restoreRun(campaignName, campaignSide, std::move(statuses));
                }
            }

            // campaign AI states (format v2 only; v1 leaves the vector empty)
            sim.campaignAi.clear();
            if (version >= 2)
            {
                auto stateCount = r.u32();
                requireReadableElements(r, stateCount);
                for (unsigned int i = 0; i < stateCount; ++i)
                {
                    CampaignAiPlayerState st;
                    st.player = readId<PlayerIdTag>(r);
                    auto weightCount = r.u32();
                    requireReadableElements(r, weightCount);
                    for (unsigned int j = 0; j < weightCount; ++j)
                    {
                        st.unitWeights.push_back(r.i32());
                    }
                    auto limitCount = r.u32();
                    requireReadableElements(r, limitCount);
                    for (unsigned int j = 0; j < limitCount; ++j)
                    {
                        st.unitLimits.push_back(r.i32());
                    }
                    st.nextRepartition = readGameTime(r);
                    st.nextFactoryScan = readGameTime(r);
                    st.nextBuilderScan = readGameTime(r);
                    st.nextAttackScan = readGameTime(r);
                    for (auto& group : st.groups)
                    {
                        auto groupCount = r.u32();
                        requireReadableElements(r, groupCount);
                        for (unsigned int j = 0; j < groupCount; ++j)
                        {
                            group.push_back(readId<UnitIdTag>(r));
                        }
                    }
                    if (r.boolean())
                    {
                        st.attackTarget = readId<UnitIdTag>(r);
                    }
                    if (version >= 8)
                    {
                        st.nextDeadlockScan = readGameTime(r);
                        auto dlCount = r.u32();
                        requireReadableElements(r, dlCount);
                        for (unsigned int j = 0; j < dlCount; ++j)
                        {
                            auto factoryId = readId<UnitIdTag>(r);
                            CampaignAiPlayerState::DeadlockState dl;
                            if (r.boolean())
                            {
                                dl.blockedSince = readGameTime(r);
                            }
                            if (r.boolean())
                            {
                                dl.reclaimTarget = readId<UnitIdTag>(r);
                            }
                            if (r.boolean())
                            {
                                dl.reclaimBuilder = readId<UnitIdTag>(r);
                            }
                            if (r.boolean())
                            {
                                dl.reclaimIssuedAt = readGameTime(r);
                            }
                            dl.reclaimAttempts = r.u32();
                            if (version >= 16)
                            {
                                dl.reclaimStalledAt = readOptionalGameTime(r);
                                if (r.boolean())
                                {
                                    auto distance = readSimScalar(r);
                                    if (!std::isfinite(distance.value) || distance < 0_ss)
                                    { throw SaveFormatException("invalid AI recovery distance"); }
                                    dl.reclaimBestDistance = distance;
                                }
                                dl.reclaimDeadline = readOptionalGameTime(r);
                                auto avoidCount = r.u32();
                                if (avoidCount > r.remaining() / 4) { throw SaveFormatException("invalid AI recovery avoid count"); }
                                unsigned int previous = 0;
                                for (unsigned int k = 0; k < avoidCount; ++k)
                                {
                                    auto id = r.u32();
                                    if (k && id <= previous) { throw SaveFormatException("unordered AI recovery avoid ids"); }
                                    dl.reclaimAvoid.insert(id); previous = id;
                                }
                                auto evictCount = r.u32();
                                if (evictCount > r.remaining() / 8) { throw SaveFormatException("invalid AI recovery eviction count"); }
                                previous = 0;
                                for (unsigned int k = 0; k < evictCount; ++k)
                                {
                                    auto id = r.u32(); auto count = r.u32();
                                    if (k && id <= previous) { throw SaveFormatException("unordered AI recovery eviction ids"); }
                                    dl.evictAttempts.emplace(id, count); previous = id;
                                }
                            }
                            st.deadlockedFactories[factoryId] = dl;
                        }
                    }
                    sim.campaignAi.push_back(std::move(st));
                }
                // the deterministic sorted unit index is derived from
                // the definitions, not stored
                rebuildCampaignAiIndex(sim);
            }

            // meteor shower scheduler state (format v4); v1-v3 saves
            // keep whatever the fresh simulation configured
            if (version >= 4)
            {
                auto& m = sim.meteorShower;
                m.enabled = r.boolean();
                m.active = r.boolean();
                m.nextStrikeTime = readGameTime(r);
                m.timeStrikeEnds = readGameTime(r);
                m.nextHitTime = readGameTime(r);
                m.originX = r.i32();
                m.originZ = r.i32();
                m.targetX = r.i32();
                m.targetZ = r.i32();
            }

            // skirmish runtime settings (format v5)
            if (version >= 5)
            {
                sim.commanderDeathContinues = r.boolean();
                sim.computerResourceScale = r.f32bits();
                auto allyCount = r.u32();
                if (allyCount != sim.players.size())
                {
                    throw SaveFormatException("save ally-group table does not match player count");
                }
                for (std::size_t i = 0; i < sim.players.size(); ++i)
                {
                    if (r.boolean())
                    {
                        sim.players[i].allyGroup = r.u32();
                    }
                    else
                    {
                        sim.players[i].allyGroup = std::nullopt;
                    }
                }
            }

            // derived caches rebuilt from restored state
            sim.rebuildOccupancyAfterLoad();
            if (version >= 15)
            {
                const auto width = r.u32(), height = r.u32(), count = r.u32();
                if (width != static_cast<unsigned int>(sim.occupiedGrid.getWidth())
                    || height != static_cast<unsigned int>(sim.occupiedGrid.getHeight()))
                { throw SaveFormatException("mobile occupancy dimensions do not match map"); }
                auto& cells = sim.occupiedGrid.getVector();
                if (count > cells.size() || count > r.remaining() / 8)
                { throw SaveFormatException("invalid mobile occupancy count"); }
                sim.occupiedGrid.forEach(sim.occupiedGrid.getRegion(), [](auto& cell) { cell.mobileUnitId = std::nullopt; });
                std::unordered_set<UnitId> mobileOwners;
                for (const auto& [id, unit] : sim.units)
                { if (sim.unitDefinitions.at(unit.unitType).isMobile) { mobileOwners.insert(id); } }
                std::uint32_t previous = 0;
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const auto index = r.u32();
                    const auto owner = readId<UnitIdTag>(r);
                    if (index >= cells.size() || (i > 0 && index <= previous))
                    { throw SaveFormatException("invalid mobile occupancy cell index"); }
                    if (mobileOwners.count(owner) == 0)
                    { throw SaveFormatException("invalid mobile occupancy owner"); }
                    sim.occupiedGrid.mutate(GridCoordinates(index % width, index / width), [&](auto& cell) { cell.mobileUnitId = owner; });
                    previous = index;
                }
            }
        }

        // ------------------------------------------------------------------
        // save metadata block (written first for cheap list reads)
        // ------------------------------------------------------------------

        /** Compact per-player row carried in metadata for list display
            and GameParameters reconstruction. */
        void writeMetaPlayer(SaveWriter& w, const GamePlayerInfo& p, std::uint32_t version)
        {
            writeOptionalString(w, p.name);
            w.boolean(p.type == GamePlayerType::Human);
            w.string(p.side);
            w.u32(p.color.value);
            writeOpaqueUnit(w, p.startingMetal);
            writeOpaqueUnit(w, p.startingEnergy);
            if (version >= 5)
            {
                w.boolean(p.allyGroup.has_value());
                if (p.allyGroup)
                {
                    w.u32(*p.allyGroup);
                }
            }
        }

        SavedPlayerMeta readMetaPlayer(SaveReader& r, std::uint32_t version)
        {
            SavedPlayerMeta p;
            p.name = readOptionalString(r);
            p.isHuman = r.boolean();
            p.side = r.string();
            p.color = PlayerColorIndex(r.u32());
            p.metal = readOpaqueUnit<float, MetalTag>(r);
            p.energy = readOpaqueUnit<float, EnergyTag>(r);
            if (version >= 5 && r.boolean())
            {
                p.allyGroup = r.u32();
            }
            return p;
        }

        void writeMetadata(SaveWriter& w, const SavedGameMetadata& m, const GameSimulation& sim, std::uint32_t version)
        {
            w.string(m.saveName);
            w.u64(m.unixTimeSeconds);
            w.string(m.mapName);
            w.u32(m.schemaIndex);
            w.boolean(m.isCampaign);
            if (m.isCampaign)
            {
                w.string(m.campaignName);
                w.string(m.campaignSide);
                w.u32(m.missionIndex);
                w.string(m.missionFile);
                w.string(m.missionName);
                w.string(m.difficulty);
            }
            writeId(w, m.localPlayerId);
            w.string(m.localSide);
            w.u32(m.playerCount);
            writeGameTime(w, m.gameTime);
            w.boolean(m.commanderDeath);

            // skirmish frontend stages (format v5)
            if (version >= 5)
            {
                w.boolean(m.skirmishStartLocation.has_value());
                if (m.skirmishStartLocation)
                {
                    w.u32(*m.skirmishStartLocation);
                }
                w.boolean(m.skirmishMapping.has_value());
                if (m.skirmishMapping)
                {
                    w.u32(*m.skirmishMapping);
                }
                w.boolean(m.skirmishLineOfSight.has_value());
                if (m.skirmishLineOfSight)
                {
                    w.u32(*m.skirmishLineOfSight);
                }
                w.boolean(m.skirmishDifficulty.has_value());
                if (m.skirmishDifficulty)
                {
                    w.u32(*m.skirmishDifficulty);
                }
            }

            // player table for GameParameters reconstruction
            for (const auto& p : sim.players)
            {
                writeMetaPlayer(w, p, version);
            }
        }

        SavedGameMetadata readMetadata(SaveReader& r, std::uint32_t version)
        {
            SavedGameMetadata m;
            m.saveName = r.string();
            m.unixTimeSeconds = r.u64();
            m.mapName = r.string();
            m.schemaIndex = r.u32();
            m.isCampaign = r.boolean();
            if (m.isCampaign)
            {
                m.campaignName = r.string();
                m.campaignSide = r.string();
                m.missionIndex = r.u32();
                m.missionFile = r.string();
                m.missionName = r.string();
                m.difficulty = r.string();
            }
            m.localPlayerId = readId<PlayerIdTag>(r);
            m.localSide = r.string();
            m.playerCount = r.u32();
            m.gameTime = readGameTime(r);
            m.commanderDeath = r.boolean();

            if (version >= 5)
            {
                if (r.boolean())
                {
                    m.skirmishStartLocation = r.u32();
                }
                if (r.boolean())
                {
                    m.skirmishMapping = r.u32();
                }
                if (r.boolean())
                {
                    m.skirmishLineOfSight = r.u32();
                }
                if (r.boolean())
                {
                    m.skirmishDifficulty = r.u32();
                }
            }

            if (m.playerCount != 0 && m.localPlayerId.value >= m.playerCount)
            {
                throw SaveFormatException("save metadata local player out of range");
            }
            requireReadableElements(r, m.playerCount);
            m.players.reserve(m.playerCount);
            for (unsigned int i = 0; i < m.playerCount; ++i)
            {
                m.players.push_back(readMetaPlayer(r, version));
            }
            return m;
        }
    }

    // ----------------------------------------------------------------------
    // public API
    // ----------------------------------------------------------------------

    std::vector<char> encodeSavedGame(
        const GameSimulation& sim,
        const GameParameters& params,
        const CampaignProgress* progress,
        const std::string& saveName,
        std::uint64_t unixTimeSeconds,
        std::uint32_t formatVersion)
    {
        SavedGameMetadata meta;
        meta.saveName = saveName;
        meta.unixTimeSeconds = unixTimeSeconds;
        meta.mapName = params.mapName;
        meta.schemaIndex = params.schemaIndex;
        meta.isCampaign = params.campaign.has_value();
        if (params.campaign)
        {
            meta.campaignName = params.campaign->campaignName;
            meta.campaignSide = params.campaign->campaignSide;
            meta.missionIndex = params.campaign->missionIndex;
            meta.missionFile = params.campaign->missionFile;
            meta.missionName = params.campaign->missionName;
            meta.difficulty = params.campaign->difficulty;
        }

        // local player = the human one
        for (Index i = 0; i < getSize(sim.players); ++i)
        {
            if (sim.players[i].type == GamePlayerType::Human)
            {
                meta.localPlayerId = PlayerId(i);
                meta.localSide = sim.players[i].side;
                break;
            }
        }
        meta.playerCount = static_cast<unsigned int>(sim.players.size());
        meta.gameTime = sim.gameTime;
        meta.commanderDeath = sim.commanderDeathContinues;
        if (!params.campaign)
        {
            meta.skirmishStartLocation = static_cast<unsigned int>(params.skirmish.startLocation);
            meta.skirmishMapping = static_cast<unsigned int>(params.skirmish.mapping);
            meta.skirmishLineOfSight = static_cast<unsigned int>(params.skirmish.lineOfSight);
            meta.skirmishDifficulty = static_cast<unsigned int>(params.skirmish.difficulty);
        }

        SaveWriter payload;
        writeMetadata(payload, meta, sim, formatVersion);
        writeSimulationBody(payload, sim, progress, params, formatVersion);
        return encodeSaveFile(formatVersion, payload.getData());
    }

    DecodedSaveGame decodeSavedGame(const std::vector<char>& fileBytes)
    {
        std::uint32_t version = 0;
        auto payload = decodeSaveFile(fileBytes, version);
        if (version < OldestSupportedSaveGameFormatVersion || version > SaveGameFormatVersion)
        {
            throw SaveFormatException("unsupported save format version " + std::to_string(version));
        }
        SaveReader r(payload);
        DecodedSaveGame out;
        out.meta = readMetadata(r, version);
        out.simulationBytes.assign(payload.begin() + r.position(), payload.end());
        out.version = version;
        return out;
    }

    SavedGameMetadata readSaveMetadata(const std::vector<char>& fileBytes)
    {
        std::uint32_t version = 0;
        auto payload = decodeSaveFile(fileBytes, version);
        if (version < OldestSupportedSaveGameFormatVersion || version > SaveGameFormatVersion)
        {
            throw SaveFormatException("unsupported save format version " + std::to_string(version));
        }
        SaveReader r(payload);
        return readMetadata(r, version);
    }

    GameParameters gameParametersFromSave(const DecodedSaveGame& save)
    {
        GameParameters params(save.meta.mapName, save.meta.schemaIndex);
        for (Index i = 0; i < getSize(save.meta.players) && static_cast<std::size_t>(i) < params.players.size(); ++i)
        {
            const auto& p = save.meta.players[i];
            PlayerInfo info;
            info.name = p.name;
            info.controller = p.isHuman
                ? PlayerControllerType(PlayerControllerTypeHuman())
                : PlayerControllerType(PlayerControllerTypeComputer());
            info.side = p.side;
            info.color = p.color;
            info.metal = p.metal;
            info.energy = p.energy;
            info.allyGroup = p.allyGroup;
            params.players[i] = info;
        }

        if (!save.meta.isCampaign)
        {
            // restore the skirmish frontend toggles (format v5);
            // older payloads keep the retail defaults
            if (save.meta.skirmishStartLocation)
            {
                params.skirmish.startLocation = static_cast<SkirmishStartLocation>(*save.meta.skirmishStartLocation);
            }
            if (save.meta.skirmishMapping)
            {
                params.skirmish.mapping = static_cast<SkirmishMapping>(*save.meta.skirmishMapping);
            }
            if (save.meta.skirmishLineOfSight)
            {
                params.skirmish.lineOfSight = static_cast<SkirmishLineOfSight>(*save.meta.skirmishLineOfSight);
            }
            if (save.meta.skirmishDifficulty)
            {
                params.skirmish.difficulty = static_cast<SkirmishDifficulty>(*save.meta.skirmishDifficulty);
            }
            params.skirmish.commanderDeath = save.meta.commanderDeath
                ? SkirmishCommanderDeath::Continues
                : SkirmishCommanderDeath::GameEnds;
        }

        if (save.meta.isCampaign)
        {
            CampaignMissionParameters campaign;
            campaign.campaignName = save.meta.campaignName;
            campaign.campaignSide = save.meta.campaignSide;
            campaign.missionIndex = save.meta.missionIndex;
            campaign.missionFile = save.meta.missionFile;
            campaign.missionName = save.meta.missionName;
            campaign.difficulty = save.meta.difficulty;
            params.campaign = campaign;
        }
        return params;
    }

    void applySavedGame(GameSimulation& sim, const DecodedSaveGame& save, CampaignProgress* progress)
    {
        SaveReader r(save.simulationBytes);
        readSimulationBody(r, sim, progress, save.version);
        try { sim.rebuildAirServiceIndexes(); }
        catch (const std::runtime_error& e) { throw SaveFormatException(e.what()); }
        if (!sim.players.empty() && save.meta.localPlayerId.value >= sim.players.size())
        {
            throw SaveFormatException("save local player out of range of restored player table");
        }
        if (!r.atEnd())
        {
            throw SaveFormatException("trailing bytes after simulation body: at " + std::to_string(r.position()) + " of " + std::to_string(save.simulationBytes.size()));
        }
    }
}
