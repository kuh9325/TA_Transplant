#include "UiTextBox.h"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_keyboard.h>
#include <algorithm>
#include <cctype>

namespace rwe
{
    namespace
    {
        /**
         * Map an SDL keycode to the text character it produces, honoring
         * Shift for letters and the common shifted symbols. Returns 0 for
         * non-printable keys.
         */
        char keyCodeToChar(int keyCode, bool shift)
        {
            auto kc = static_cast<SDL_Keycode>(keyCode);
            if (kc >= SDLK_A && kc <= SDLK_Z)
            {
                char c = static_cast<char>('a' + static_cast<int>(kc - SDLK_A));
                return shift ? static_cast<char>(std::toupper(c)) : c;
            }
            if (kc == SDLK_SPACE)
            {
                return ' ';
            }
            if (kc >= SDLK_0 && kc <= SDLK_9)
            {
                static const char* shifted = ")!@#$%^&*(";
                return shift ? shifted[kc - SDLK_0] : static_cast<char>('0' + static_cast<int>(kc - SDLK_0));
            }
            switch (kc)
            {
                case SDLK_MINUS:
                    return shift ? '_' : '-';
                case SDLK_EQUALS:
                    return shift ? '+' : '=';
                case SDLK_LEFTBRACKET:
                    return shift ? '{' : '[';
                case SDLK_RIGHTBRACKET:
                    return shift ? '}' : ']';
                case SDLK_SEMICOLON:
                    return shift ? ':' : ';';
                case SDLK_COMMA:
                    return shift ? '<' : ',';
                case SDLK_PERIOD:
                    return shift ? '>' : '.';
                case SDLK_SLASH:
                    return shift ? '?' : '/';
                case SDLK_APOSTROPHE:
                    return shift ? '"' : '\'';
                default:
                    return 0;
            }
        }
    }

    UiTextBox::UiTextBox(
        int posX,
        int posY,
        unsigned int sizeX,
        unsigned int sizeY,
        const std::string& initialText,
        std::shared_ptr<SpriteSeries> font,
        unsigned int maxChars)
        : UiComponent(posX, posY, sizeX, sizeY),
          font(std::move(font)),
          text(initialText.substr(0, maxChars)),
          maxChars(maxChars)
    {
        caret = text.size();
    }

    void UiTextBox::setText(const std::string& newText)
    {
        text = newText.substr(0, maxChars);
        caret = std::min(caret, text.size());
    }

    bool UiTextBox::keyDown(KeyEvent event)
    {
        if (!enabled || !focused)
        {
            return false;
        }

        switch (event.keyCode)
        {
            case SDLK_BACKSPACE:
                if (caret > 0)
                {
                    text.erase(caret - 1, 1);
                    --caret;
                }
                return true;
            case SDLK_DELETE:
                if (caret < text.size())
                {
                    text.erase(caret, 1);
                }
                return true;
            case SDLK_LEFT:
                if (caret > 0)
                {
                    --caret;
                }
                return true;
            case SDLK_RIGHT:
                if (caret < text.size())
                {
                    ++caret;
                }
                return true;
            case SDLK_HOME:
                caret = 0;
                return true;
            case SDLK_END:
                caret = text.size();
                return true;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_ESCAPE:
                // retail TextBox releases keyboard focus on Enter and
                // Escape (TotalA.exe 0x4a74fa/0x4a7515): the key is
                // consumed by the gadget, which keeps crdefault/escdefault
                // and sibling quickkeys from seeing it this press
                focused = false;
                return true;
            default:
                break;
        }

        // chorded input is a shortcut, not text: Ctrl/Cmd/Alt-held keys
        // are never inserted (Alt+quickkey fires sibling buttons even
        // while a TextBox is focused in retail)
        auto mods = SDL_GetModState();
        if ((mods & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0)
        {
            return false;
        }

        bool shift = (mods & SDL_KMOD_SHIFT) != 0;
        char c = keyCodeToChar(event.keyCode, shift);
        if (c == 0)
        {
            return false;
        }
        if (text.size() < maxChars)
        {
            text.insert(caret, 1, c);
            ++caret;
            caretTimer = 0.0f;
        }
        return true;
    }

    void UiTextBox::mouseDown(MouseButtonEvent event)
    {
        if (!enabled)
        {
            return;
        }

        focused = true;
        // place caret at the click position: walk glyph widths
        int relX = event.x; // caller already subtracts component origin
        std::size_t best = 0;
        float acc = 4.0f; // left padding matches render
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            auto ch = static_cast<unsigned char>(text[i]);
            acc += ch < font->sprites.size() ? font->sprites[ch]->bounds.right() : 0.0f;
            if (relX < acc)
            {
                break;
            }
            best = i + 1;
        }
        caret = best;
        caretTimer = 0.0f;
    }

    void UiTextBox::update(float dt)
    {
        caretTimer += dt;
        if (caretTimer >= 1.0f)
        {
            caretTimer -= 1.0f;
        }
    }

    void UiTextBox::render(UiRenderService& context) const
    {
        context.pushClip(Rectangle2f(
            static_cast<float>(posX), static_cast<float>(posY),
            static_cast<float>(sizeX), static_cast<float>(sizeY)));

        // drawText y is the glyph baseline; the stock 20px-high box
        // vertically centers its font
        float baseline = static_cast<float>(posY) + static_cast<float>(sizeY) - 5.0f;
        context.drawText(static_cast<float>(posX) + 4.0f, baseline, text, *font);

        if (focused && caretTimer < 0.5f)
        {
            // underscore caret at the edit position
            float caretX = 4.0f;
            for (std::size_t i = 0; i < caret; ++i)
            {
                auto ch = static_cast<unsigned char>(text[i]);
                caretX += ch < font->sprites.size() ? font->sprites[ch]->bounds.right() : 0.0f;
            }
            context.drawText(static_cast<float>(posX) + caretX, baseline, "_", *font);
        }

        context.popClip();
    }
}
