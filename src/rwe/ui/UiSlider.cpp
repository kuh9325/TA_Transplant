#include "UiSlider.h"

namespace rwe
{
    UiSlider::UiSlider(
        int posX,
        int posY,
        unsigned int sizeX,
        unsigned int sizeY,
        std::shared_ptr<SpriteSeries> sprites)
        : UiComponent(posX, posY, sizeX, sizeY),
          sprites(std::move(sprites))
    {
    }

    void UiSlider::render(UiRenderService& context) const
    {
        // stock SLIDERS layout: 10-12 track left/middle/right,
        // 13-15 knob pieces, 16-19 left/right arrow normal/pressed
        const Sprite& leftArrow = *sprites->sprites[16];
        const Sprite& rightArrow = *sprites->sprites[18];

        context.drawSpriteAbs(posX, posY, leftArrow);
        context.drawSpriteAbs(posX + sizeX - rightArrow.bounds.width(), posY, rightArrow);

        const Sprite& trackLeft = *sprites->sprites[10];
        const Sprite& trackMiddle = *sprites->sprites[11];
        const Sprite& trackRight = *sprites->sprites[12];

        float trackX = posX + leftArrow.bounds.width();
        float trackEnd = posX + sizeX - rightArrow.bounds.width();

        context.drawSpriteAbs(trackX, posY, trackLeft);
        trackX += trackLeft.bounds.width();
        while (trackX < trackEnd - trackRight.bounds.width())
        {
            context.drawSpriteAbs(trackX, posY, trackMiddle);
            trackX += trackMiddle.bounds.width();
        }
        context.drawSpriteAbs(trackEnd - trackRight.bounds.width(), posY, trackRight);

        const Sprite& knob = *sprites->sprites[13];
        float knobX = posX + ((sizeX - knob.bounds.width()) / 2.0f);
        float knobY = posY + ((sizeY - knob.bounds.height()) / 2.0f);
        context.drawSpriteAbs(knobX, knobY, knob);

        // permanently disabled: there is no runtime backend
        context.fillColor(posX, posY, sizeX, sizeY, Color(0, 0, 0, 128));
    }
}
