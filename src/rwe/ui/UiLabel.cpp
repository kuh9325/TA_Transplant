#include "UiLabel.h"

namespace rwe
{

    void UiLabel::render(UiRenderService& context) const
    {
        switch (alignment)
        {
            case Alignment::Left:
                context.drawText(posX, posY + 12.0f, text, *font);
                break;
            case Alignment::LeftWrapped:
                context.drawTextWrapped(Rectangle2f::fromTopLeft(posX, posY, sizeX, sizeY), text, *font);
                break;
            case Alignment::Center:
                context.drawTextCentered(posX, posY, text, *font);
                break;
            case Alignment::CenterRect:
                context.drawTextCentered(posX + sizeX / 2.0f, posY + (sizeY - 10.0f) / 2.0f, text, *font);
                break;
        }
    }

    UiLabel::UiLabel(int posX, int posY, unsigned int sizeX, unsigned int sizeY, const std::string& text, const std::shared_ptr<SpriteSeries>& font) : UiComponent(posX, posY, sizeX, sizeY), text(text), font(font)
    {
    }

    void UiLabel::setClickable(bool newClickable)
    {
        clickable = newClickable;
    }

    void UiLabel::mouseDown(MouseButtonEvent /*event*/)
    {
        if (clickable)
        {
            pressed = true;
        }
    }

    void UiLabel::mouseUp(MouseButtonEvent /*event*/)
    {
        if (clickable && pressed)
        {
            pressed = false;
            messagesSubject.next(ActivateMessage{ActivateMessage::Type::Primary});
        }
    }

    void UiLabel::setText(const std::string& newText)
    {
        text = newText;
    }

    void UiLabel::setAlignment(Alignment newAlignment)
    {
        alignment = newAlignment;
    }
}
