#include "UnitModelDefinition.h"
#include <rwe/util/Index.h>
#include <rwe/util/rwe_string.h>

namespace rwe
{
    std::unordered_map<std::string, int> createPieceNameIndex(const std::vector<UnitPieceDefinition>& pieces)
    {
        std::unordered_map<std::string, int> m;
        for (Index i = 0; i < getSize(pieces); ++i)
        {
            m.insert_or_assign(toUpper(pieces[i].name), i);
        }
        return m;
    }

    UnitModelDefinition createUnitModelDefinition(SimScalar height, std::vector<UnitPieceDefinition>&& pieces)
    {
        UnitModelDefinition d;
        d.height = height;
        d.pieces = std::move(pieces);
        d.pieceIndicesByName = createPieceNameIndex(d.pieces);

        d.parentPieceIndices.reserve(d.pieces.size());
        for (const auto& piece : d.pieces)
        {
            int parent = -1;
            if (piece.parent)
            {
                auto it = d.pieceIndicesByName.find(toUpper(*piece.parent));
                if (it != d.pieceIndicesByName.end())
                {
                    parent = it->second;
                }
            }
            d.parentPieceIndices.push_back(parent);
        }
        return d;
    }
}
