#pragma once

#include <rwe/ui/UiComponent.h>

namespace rwe
{
    class UiLabel : public UiComponent
    {
    public:
        enum class Alignment
        {
            // single line drawn at the 12px baseline offset, no clip;
            // stock label semantics
            Left,
            // word-wrapped into the label rect and clipped to it; used
            // for exe-drawn text regions (help body, briefing pager)
            LeftWrapped,
            Center,
            CenterRect
        };

    private:
        std::string text;
        std::shared_ptr<SpriteSeries> font;
        Alignment alignment{Alignment::Left};

        /**
         * When true the label consumes mouse input and emits an
         * ActivateMessage on release, like a borderless button. Retail
         * uses this for exe-drawn text regions (the in-game briefing's
         * TextRegion/MOREBAR pager).
         */
        bool clickable{false};
        bool pressed{false};

    public:
        UiLabel(int posX, int posY, unsigned int sizeX, unsigned int sizeY, const std::string& text, const std::shared_ptr<SpriteSeries>& font);

        void render(UiRenderService& context) const override;

        bool acceptsMouseInput() const override
        {
            return clickable;
        }

        void setClickable(bool newClickable);

        void mouseDown(MouseButtonEvent event) override;

        void mouseUp(MouseButtonEvent event) override;

        const std::string& getText() const { return text; }

        void setText(const std::string& newText);

        void setAlignment(Alignment newAlignment);
    };
}
