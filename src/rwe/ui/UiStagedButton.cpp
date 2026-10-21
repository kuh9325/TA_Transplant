#include "UiStagedButton.h"

namespace rwe
{
    ButtonClickEvent::Source mouseButtonToSource(MouseButtonEvent::MouseButton btn)
    {
        switch (btn)
        {
            case MouseButtonEvent::MouseButton::Left:
                return ButtonClickEvent::Source::LeftMouseButton;
            case MouseButtonEvent::MouseButton::Right:
                return ButtonClickEvent::Source::RightMouseButton;
            case MouseButtonEvent::MouseButton::Middle:
                return ButtonClickEvent::Source::MiddleMouseButton;
                break;
            default:
                throw std::logic_error("Unknown mouse button value");
        }
    }

    void UiStagedButton::render(UiRenderService& graphics) const
    {
        const auto& sprite = pressed || toggledOn ? *pressedSprite : *stages[currentStage].sprite;

        graphics.drawSpriteAbs(posX, posY, sprite);

        const auto& label = stages[currentStage].label;
        switch (textAlign)
        {
            case TextAlign::Hidden:
                break;
            case TextAlign::Left:
            {
                float textX = posX + 6.0f;
                float textY = posY + (sizeY / 2.0f) + 6.0f;
                if (pressed)
                {
                    textX += 1.0f;
                    textY += 1.0f;
                }
                graphics.drawText(textX, textY, label, *labelFont);
                break;
            }
            case TextAlign::Center:
            {
                float textX = posX + (sizeX / 2.0f);
                float textY = posY + (sizeY / 2.0f);
                if (pressed)
                {
                    textX += 1.0f;
                    textY += 1.0f;
                }
                graphics.drawTextCentered(textX, textY, label, *labelFont);
                break;
            }
            case TextAlign::BottomCenter:
            {
                float textX = posX + (sizeX / 2.0f);
                float textY = posY + sizeY - 7;
                if (pressed)
                {
                    textX += 1.0f;
                    textY += 1.0f;
                }
                graphics.drawTextCenteredX(textX, textY, label, *labelFont);
                break;
            }
            default:
                throw std::logic_error("Invalid TextAlign value");
        }

        if (!enabled)
        {
            graphics.fillColor(posX, posY, sizeX, sizeY, Color(0, 0, 0, 128));
        }
    }

    UiStagedButton::UiStagedButton(
        int posX,
        int posY,
        unsigned int sizeX,
        unsigned int sizeY,
        std::vector<StageInfo> stages,
        std::shared_ptr<Sprite> pressedSprite,
        std::shared_ptr<SpriteSeries> labelFont)
        : UiComponent(posX, posY, sizeX, sizeY),
          stages(std::move(stages)),
          pressedSprite(std::move(pressedSprite)),
          labelFont(std::move(labelFont))
    {
        if (this->stages.empty())
        {
            throw std::logic_error("No stages provided for staged button");
        }
    }

    void UiStagedButton::mouseDown(MouseButtonEvent event)
    {
        if (!enabled || isHidden())
        {
            return;
        }

        switch (behaviorMode)
        {
            case BehaviorMode::Radio:
            case BehaviorMode::Cycle:
                activateButton({(mouseButtonToSource(event.button))});
                break;
            case BehaviorMode::Toggle:
            case BehaviorMode::Button:
            case BehaviorMode::Staged:
                armed = true;
                pressed = true;
                break;
            default:
                throw std::logic_error("Invalid BehaviorMode");
        }
    }

    void UiStagedButton::mouseUp(MouseButtonEvent event)
    {
        if (!enabled || isHidden())
        {
            return;
        }

        switch (behaviorMode)
        {
            case BehaviorMode::Radio:
            case BehaviorMode::Cycle:
                break;
            case BehaviorMode::Button:
            case BehaviorMode::Toggle:
            case BehaviorMode::Staged:
                armed = false;
                if (pressed)
                {
                    pressed = false;
                    activateButton({(mouseButtonToSource(event.button))});
                }
                break;
            default:
                throw std::logic_error("Invalid BehaviorMode");
        }
    }

    void UiStagedButton::mouseEnter()
    {
        switch (behaviorMode)
        {
            case BehaviorMode::Button:
            case BehaviorMode::Toggle:
            case BehaviorMode::Staged:
                if (armed)
                {
                    pressed = true;
                }
                break;
            default:
                break;
        }
    }

    void UiStagedButton::mouseLeave()
    {
        switch (behaviorMode)
        {
            case BehaviorMode::Button:
            case BehaviorMode::Toggle:
            case BehaviorMode::Staged:
                pressed = false;
                break;
            default:
                break;
        }
    }

    void UiStagedButton::focus()
    {
        focused = true;
    }

    void UiStagedButton::unfocus()
    {
        focused = false;
        switch (behaviorMode)
        {
            case BehaviorMode::Button:
            case BehaviorMode::Toggle:
            case BehaviorMode::Staged:
                armed = false;
                pressed = false;
                break;
            default:
                break;
        }
    }

    Observable<ButtonClickEvent>& UiStagedButton::onClick()
    {
        return clickSubject;
    }

    bool UiStagedButton::keyDown(KeyEvent event)
    {
        if (!enabled || isHidden())
        {
            return false;
        }

        if (quickKey && event.keyCode == quickKey)
        {
            activateButton({ButtonClickEvent::Source::Keyboard});
            return true;
        }
        // SPACE activates the focused control only; panel keyDown
        // broadcasts, so an unfocused button must not fire
        if (event.keyCode == SDLK_SPACE && focused)
        {
            activateButton({ButtonClickEvent::Source::Keyboard});
            return true;
        }
        return false;
    }

    void UiStagedButton::setEnabled(bool newEnabled)
    {
        enabled = newEnabled;
        armed = false;
        pressed = false;
    }

    bool UiStagedButton::isEnabled() const
    {
        return enabled;
    }

    void UiStagedButton::activateButton(const ButtonClickEvent& event)
    {
        // callers that activate a button programmatically (panel
        // crdefault/escdefault dispatch) must honor the disabled state
        // the same way click/key paths do
        if (!enabled)
        {
            return;
        }
        switch (behaviorMode)
        {
            case BehaviorMode::Radio:
                toggledOn = true;
                break;
            case BehaviorMode::Cycle:
                nextStage();
                break;
            case BehaviorMode::Toggle:
                toggledOn = !toggledOn;
                break;
            case BehaviorMode::Button:
            case BehaviorMode::Staged:
                break;
            default:
                throw std::logic_error("Invalid BehaviorMode");
        }

        clickSubject.next(event);
        messagesSubject.next(ActivateMessage{sourceToType(event.source)});
    }

    void UiStagedButton::setStage(unsigned int newStage)
    {
        if (newStage >= stages.size())
        {
            throw std::logic_error("New stage is not in range");
        }
        currentStage = newStage;
    }

    unsigned int UiStagedButton::getStage() const
    {
        return currentStage;
    }

    const std::string& UiStagedButton::getCurrentStageLabel() const
    {
        return stages[currentStage].label;
    }

    void UiStagedButton::setTextAlign(UiStagedButton::TextAlign align)
    {
        textAlign = align;
    }

    void UiStagedButton::setLabel(const std::string& label)
    {
        stages[currentStage].label = label;
    }

    void UiStagedButton::setNormalSprite(const std::shared_ptr<Sprite>& sprite)
    {
        stages[currentStage].sprite = sprite;
    }

    void UiStagedButton::setPressedSprite(const std::shared_ptr<Sprite>& sprite)
    {
        pressedSprite = sprite;
    }

    void UiStagedButton::setToggledOn(bool _toggledOn)
    {
        toggledOn = _toggledOn;
    }

    void UiStagedButton::setBehaviorMode(BehaviorMode mode)
    {
        behaviorMode = mode;
    }

    void UiStagedButton::setQuickKey(int quickKey)
    {
        this->quickKey = quickKey;
    }

    void UiStagedButton::nextStage()
    {
        auto stageCount = stages.size();
        currentStage = (currentStage + 1) % stageCount;
    }
}
