#pragma once

#include <cstdint>
#include <rwe/grid/Grid.h>

namespace rwe
{
    struct NumericCellHash
    {
        template <typename T> std::uint32_t operator()(T value) const { return static_cast<std::uint32_t>(value); }
    };

    /** A simulation grid with an incrementally maintained, position-keyed digest.
     * Read APIs never expose mutable references. Mutations are scoped callbacks;
     * references must not escape them. Only changed cells are rehashed, avoiding
     * a map-wide scan during computeHash. Bulk replacement rebuilds once on load.
     */
    template <typename T, typename CellHash = NumericCellHash>
    class StateHashGrid
    {
        Grid<T> grid;
        std::uint32_t digest{0};

        static std::uint32_t avalanche(std::uint32_t x)
        {
            x ^= x >> 16; x *= 0x7feb352du;
            x ^= x >> 15; x *= 0x846ca68bu;
            return x ^ (x >> 16);
        }
        static std::uint32_t contribution(unsigned int index, const T& value)
        {
            // Empty/default cells contribute zero, but every non-default cell
            // includes its index. Swapping equal-weight occupants is observable.
            auto h = CellHash{}(value);
            return h == 0 ? 0 : avalanche(avalanche(index + 1) ^ h);
        }
    public:
        StateHashGrid() = default;
        StateHashGrid(int width, int height) : StateHashGrid(Grid<T>(width, height)) {}
        StateHashGrid(int width, int height, const T& value) : StateHashGrid(Grid<T>(width, height, value)) {}
        StateHashGrid(Grid<T> value) : grid(std::move(value)) { digest = recomputeDigest(); }
        StateHashGrid& operator=(Grid<T> value) { grid = std::move(value); digest = recomputeDigest(); return *this; }
        operator const Grid<T>&() const { return grid; }
        int getWidth() const { return grid.getWidth(); }
        int getHeight() const { return grid.getHeight(); }
        const auto& getVector() const { return grid.getVector(); }
        typename std::vector<T>::const_reference get(int x, int y) const { return grid.getVector()[grid.toIndex(x, y)]; }
        typename std::vector<T>::const_reference get(const GridCoordinates& c) const { return grid.getVector()[grid.toIndex(c)]; }
        // Reference access is deliberately unavailable for vector<bool> cells.
        auto tryGet(const Point& p) const requires (!std::is_same_v<T, bool>) { return grid.tryGet(p); }
        std::optional<T> tryGetValue(const Point& p) const
        {
            if (p.x < 0 || p.y < 0 || p.x >= getWidth() || p.y >= getHeight()) { return std::nullopt; }
            return get(p.x, p.y);
        }
        auto getRegion() const { return grid.getRegion(); }
        auto clipRegion(const DiscreteRect& r) const { return grid.clipRegion(r); }
        auto tryToRegion(const DiscreteRect& r) const { return grid.tryToRegion(r); }
        auto clampToCoords(const Point& p) const { return grid.clampToCoords(p); }
        bool contains(const DiscreteRect& r) const { return grid.contains(r); }
        template <typename F> bool any(const GridRegion& r, F f) const { return r.any([&](auto c) { return f(get(c)); }); }
        template <typename F> bool any(F f) const { return any(getRegion(), f); }
        template <typename U, typename F> bool any2(int x, int y, const Grid<U>& g, F f) const { return g.anyIndexed([&](auto c, const auto& v) { return f(get(x + c.x, y + c.y), v); }); }
        template <typename U, typename F> U accumulate(const GridRegion& r, U initial, F f) const
        { return r.accumulate(initial, [&](auto value, auto c) { return f(value, get(c)); }); }

        template <typename F> void mutate(const GridCoordinates& c, F f)
        {
            auto index = grid.toIndex(c);
            // Copying one cell also handles vector<bool>'s proxy and ensures a
            // throwing callback cannot leave a stale digest or a partial write.
            T value = grid.getVector()[index];
            auto before = contribution(index, value);
            f(value);
            digest ^= before ^ contribution(index, value);
            grid.getVector()[index] = std::move(value);
        }
        void set(const GridCoordinates& c, const T& value) { mutate(c, [&](T& cell) { cell = value; }); }
        void set(int x, int y, const T& value) { set(GridCoordinates(x, y), value); }
        void set(const GridRegion& r, const T& value) { r.forEach([&](auto c) { set(c, value); }); }
        template <typename F> void forEach(const GridRegion& r, F f) { r.forEach([&](auto c) { mutate(c, f); }); }
        template <typename F> void forEach(const GridRegion& r, F f) const { r.forEach([&](auto c) { f(get(c)); }); }
        template <typename F> void forEachIndexed(F f) const { getRegion().forEach([&](auto c) { f(c, get(c)); }); }
        template <typename U, typename F> void forEach2(int x, int y, const Grid<U>& g, F f)
        { g.forEachIndexed([&](auto c, const auto& value) { mutate(GridCoordinates(x + c.x, y + c.y), [&](auto& cell) { f(cell, value); }); }); }

        std::uint32_t stateHash() const
        { return avalanche(digest ^ avalanche(getWidth()) ^ (avalanche(getHeight()) * 16777619u)); }
        // Independent full recomputation for regressions/diagnostics only.
        std::uint32_t recomputeDigest() const
        {
            std::uint32_t result = 0, index = 0;
            for (const auto& cell : grid.getVector()) { result ^= contribution(index++, cell); }
            return result;
        }
        bool digestIsValid() const { return digest == recomputeDigest(); }
    };
}
