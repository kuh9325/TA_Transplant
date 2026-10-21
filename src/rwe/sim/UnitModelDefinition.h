#pragma once

#include <rwe/sim/SimScalar.h>
#include <rwe/sim/UnitPieceDefinition.h>
#include <vector>

namespace rwe
{
    struct UnitModelDefinition
    {
        SimScalar height;
        std::vector<UnitPieceDefinition> pieces;
        std::unordered_map<std::string, int> pieceIndicesByName;

        /**
         * pieces[i]'s parent index, or -1 when the piece is a root or its
         * parent name is absent from pieceIndicesByName (stock 3DOs
         * occasionally reference missing pieces; retail ignores them).
         * Pieces are stored pre-order, so parent indices always precede
         * their children -- enabling single-pass top-down transform
         * computation without per-piece name lookups.
         */
        std::vector<int> parentPieceIndices;
    };

    UnitModelDefinition createUnitModelDefinition(SimScalar height, std::vector<UnitPieceDefinition>&& pieces);
}
