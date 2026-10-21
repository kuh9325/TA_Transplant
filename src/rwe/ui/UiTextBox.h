#pragma once

#include <memory>
#include <rwe/render/SpriteSeries.h>
#include <rwe/ui/UiComponent.h>
#include <string>

namespace rwe
{
    /**
     * Single-line text entry gadget (retail GUI TextBox, gadget id 3 --
     * e.g. SAVELIST.GUI's GAMENAME and SAVEGAME.GUI's name field).
     *
     * The retail gadget takes focus on click and accepts printable
     * characters up to its `maxchars` limit; BACKSPACE/DELETE edit,
     * LEFT/RIGHT move the caret. Rendering is the text plus a blinking
     * underscore caret at the edit position, clipped to the gadget rect.
     */
    class UiTextBox : public UiComponent
    {
    private:
        std::shared_ptr<SpriteSeries> font;
        std::string text;
        std::size_t caret{0};
        unsigned int maxChars{99};
        bool enabled{true};
        bool focused{false};
        float caretTimer{0.0f};

    public:
        UiTextBox(
            int posX,
            int posY,
            unsigned int sizeX,
            unsigned int sizeY,
            const std::string& initialText,
            std::shared_ptr<SpriteSeries> font,
            unsigned int maxChars = 99);

        const std::string& getText() const { return text; }
        void setText(const std::string& newText);

        void setMaxChars(unsigned int chars) { maxChars = chars; }

        /**
         * Disabled boxes keep rendering their text but accept no
         * focus or edits (retail uses the same gadget as a read-only
         * name display in load dialogs).
         */
        void setEnabled(bool value)
        {
            enabled = value;
            if (!enabled)
            {
                focused = false;
            }
        }

        bool isEnabled() const { return enabled; }

        bool isFocused() const override { return focused; }
        void setFocused(bool value) { focused = value; }

        /**
         * A focused, enabled TextBox owns keyboard input: the panel
         * routes keys to it exclusively and suppresses sibling
         * quickkeys (retail type-3 gadget rule).
         */
        bool capturesTextInput() const override { return enabled && focused; }

        void render(UiRenderService& context) const override;

        bool keyDown(KeyEvent event) override;

        void mouseDown(MouseButtonEvent event) override;

        void update(float dt) override;

        void focus() override
        {
            focused = enabled;
        }
        void unfocus() override { focused = false; }
    };
}
