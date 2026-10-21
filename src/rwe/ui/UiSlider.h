#pragma once

#include <memory>
#include <rwe/render/SpriteSeries.h>
#include <rwe/ui/UiComponent.h>

namespace rwe
{
    /**
     * Display-only horizontal slider, drawn from the stock
     * SLIDERS sprite series (commongui.gaf frames 10-19).
     * Used where a stock GUI declares a horizontal scrollbar
     * gadget whose setting has no runtime backend yet: the
     * control is shown dimmed and swallows input.
     */
    class UiSlider : public UiComponent
    {
    private:
        std::shared_ptr<SpriteSeries> sprites;

    public:
        UiSlider(
            int posX,
            int posY,
            unsigned int sizeX,
            unsigned int sizeY,
            std::shared_ptr<SpriteSeries> sprites);

        void render(UiRenderService& context) const override;
    };
}
