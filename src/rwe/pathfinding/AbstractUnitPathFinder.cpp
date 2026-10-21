#include "AbstractUnitPathFinder.h"

namespace rwe
{
    AbstractUnitPathFinder::AbstractUnitPathFinder(
        const GameSimulation* simulation,
        const MovementClassCollisionService* collisionService,
        UnitId self,
        std::optional<MovementClassId> movementClass,
        unsigned int footprintX,
        unsigned int footprintZ)
        : simulation(simulation),
          collisionService(collisionService),
          self(self),
          movementClass(movementClass),
          footprintX(footprintX),
          footprintZ(footprintZ)
    {
    }

    void AbstractUnitPathFinder::getSuccessors(const VertexInfo& info, std::vector<VertexInfo>& out)
    {
        std::optional<Direction> prevDirection;
        if (info.predecessor)
        {
            prevDirection = pointToDirection(info.vertex - (*info.predecessor)->vertex);
        }

        neighbourBuffer.clear();
        getNeighbours(info.vertex, neighbourBuffer);

        out.clear();
        for (const auto& neighbour : neighbourBuffer)
        {
            auto direction = pointToDirection(neighbour - info.vertex);
            auto distance = octileDistance(info.vertex, neighbour);
            assert(distance.diagonal == 0 || distance.straight == 0);
            if (isRoughTerrain(neighbour))
            {
                // double the cost on rough terrain
                distance = distance + distance;
            }
            unsigned int turns = prevDirection ? directionDistance(*prevDirection, direction) : 0;
            PathCost cost(distance, turns);
            out.push_back(VertexInfo{info.costToReach + cost, neighbour, &info});
        }
    }

    bool AbstractUnitPathFinder::isWalkable(const Point& p) const
    {
        DiscreteRect rect(p.x, p.y, footprintX, footprintZ);
        return (movementClass ? collisionService->isWalkable(*movementClass, p) : true) && !simulation->isCollisionAt(rect, self);
    }

    bool AbstractUnitPathFinder::isWalkable(int x, int y) const
    {
        return isWalkable(Point(x, y));
    }

    bool AbstractUnitPathFinder::isRoughTerrain(const Point& p) const
    {
        DiscreteRect rect(p.x, p.y, footprintX, footprintZ);
        return simulation->isAdjacentToObstacle(rect);
    }

    Point AbstractUnitPathFinder::step(const Point& p, Direction d) const
    {
        auto directionVector = directionToPoint(d);
        return p + directionVector;
    }

    void AbstractUnitPathFinder::getNeighbours(const Point& p, std::vector<Point>& out)
    {
        out.clear();
        out.reserve(Directions.size());

        for (auto d : Directions)
        {
            auto newPosition = step(p, d);

            if (!isWalkable(newPosition))
            {
                continue;
            }

            out.push_back(newPosition);
        }
    }
}
