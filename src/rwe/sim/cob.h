#pragma once

#include <rwe/cob/CobAngle.h>
#include <rwe/cob/CobPosition.h>
#include <rwe/sim/GameSimulation.h>
#include <rwe/sim/UnitId.h>

namespace rwe
{
    CobAngle toCobAngle(SimAngle angle);

    CobPosition simScalarToCobPosition(SimScalar v);

    CobSpeed toCobSpeed(SimScalar speed);

    void runUnitCobScripts(GameSimulation& simulation, UnitId unitId);
}
