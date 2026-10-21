#pragma once

#include <rwe/observable/Subject.h>
#include <rwe/ui/UiComponent.h>
#include <vector>

namespace rwe
{
    class UiStagedButton : public UiComponent
    {
    public:
        struct StageInfo
        {
            std::shared_ptr<Sprite> sprite;
            std::string label;

            StageInfo(const std::shared_ptr<Sprite>& sprite, const std::string& label) : sprite(sprite), label(label)
            {
            }
        };
        enum class TextAlign
        {
            Hidden,
            Left,
            Center,
            BottomCenter,
        };

        enum class BehaviorMode
        {
            Button,
            Toggle,
            Radio,
            Cycle,
            Staged,
        };

    private:
        std::vector<StageInfo> stages;
        std::shared_ptr<Sprite> pressedSprite;
        std::shared_ptr<SpriteSeries> labelFont;

        TextAlign textAlign{TextAlign::Hidden};

        std::optional<int> quickKey;

        /** True if the button is currently pressed down. */
        bool pressed{false};

        /**
         * True if the button is "armed".
         * The button is armed if the mouse cursor was pressed down inside of it
         * and has not yet been released.
         */
        bool armed{false};

        /**
         * True while the panel's focus is on this button. SPACE is the
         * retail focus-activation key -- it fires only here, while the
         * quickkey char fires from anywhere on the panel.
         */
        bool focused{false};

        /** True if the button is a toggle and is toggled on. */
        bool toggledOn{false};

        BehaviorMode behaviorMode{BehaviorMode::Button};

        unsigned int currentStage{0};

        /**
         * True if the button responds to input. Disabled buttons
         * (stock GUI "grayedout") render dimmed and swallow clicks
         * and quick keys.
         */
        bool enabled{true};

        Subject<ButtonClickEvent> clickSubject;

    public:
        UiStagedButton(
            int posX,
            int posY,
            unsigned int sizeX,
            unsigned int sizeY,
            std::vector<StageInfo> stages,
            std::shared_ptr<Sprite> pressedSprite,
            std::shared_ptr<SpriteSeries> labelFont);

        void render(UiRenderService& graphics) const override;

        void mouseDown(MouseButtonEvent event) override;

        void mouseUp(MouseButtonEvent event) override;

        void mouseEnter() override;

        void mouseLeave() override;

        void focus() override;

        void unfocus() override;

        bool isFocused() const override { return focused; }

        bool keyDown(KeyEvent event) override;

        Observable<ButtonClickEvent>& onClick();

        void setStage(unsigned int newStage);

        unsigned int getStage() const;

        unsigned int getStageCount() const { return static_cast<unsigned int>(stages.size()); }

        const std::string& getCurrentStageLabel() const;

        bool autoChangeStage{true};

        void setTextAlign(TextAlign align);

        void setLabel(const std::string& label);

        void setNormalSprite(const std::shared_ptr<Sprite>& sprite);

        void setPressedSprite(const std::shared_ptr<Sprite>& sprite);

        void setToggledOn(bool _toggledOn);

        void setBehaviorMode(BehaviorMode mode);

        void setQuickKey(int quickKey);

        void setEnabled(bool newEnabled);

        bool isEnabled() const;

    private:
        void activateButton(const ButtonClickEvent& event);

        void nextStage();
    };
}
