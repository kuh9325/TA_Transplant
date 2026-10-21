#pragma once

#include <memory>
#include <rwe/UiRenderService.h>
#include <rwe/events.h>
#include <rwe/observable/Observable.h>
#include <rwe/observable/Subject.h>
#include <rwe/observable/Subscription.h>
#include <rwe/ui/events.h>
#include <vector>

namespace rwe
{
    class UiComponent
    {
    protected:
        int posX;
        int posY;

        unsigned int sizeX;
        unsigned int sizeY;

        std::string name;
        unsigned int group{0};

        Subject<const ControlMessage&> messagesSubject;

    private:
        int lastMouseX{0};
        int lastMouseY{0};

        /**
         * Retail gadget "active" flag, inverted (gadget record +0x29).
         * Hidden components are not rendered and do not receive mouse,
         * key, or message dispatch.
         */
        bool hidden{false};

        std::vector<std::unique_ptr<Subscription>> subscriptions;

    public:
        UiComponent(int posX, int posY, unsigned int sizeX, unsigned int sizeY)
            : posX(posX), posY(posY), sizeX(sizeX), sizeY(sizeY)
        {
        }

        UiComponent(const UiComponent& c) = default;
        UiComponent& operator=(const UiComponent& c) = default;
        UiComponent(UiComponent&& c) = default;
        UiComponent& operator=(UiComponent&& c) = default;

        virtual ~UiComponent();

        unsigned int getWidth() { return sizeX; }

        unsigned int getHeight() { return sizeY; }

        int getX() { return posX; }
        int getY() { return posY; }

        virtual void mouseDown(MouseButtonEvent /*event*/) {}

        virtual void mouseUp(MouseButtonEvent /*event*/) {}

        virtual void mouseEnter() {}

        virtual void mouseLeave() {}

        /**
         * Returns true when the component consumed the key. The panel
         * broadcast still continues in retail (a consumed key simply
         * marks the consuming gadget); the result exists so callers
         * can apply dialog-level defaults (crdefault/escdefault) only
         * when nothing handled the key, and so focused text gadgets
         * can claim exclusive keyboard routing.
         */
        virtual bool keyDown(KeyEvent /*event*/) { return false; }

        virtual void keyUp(KeyEvent /*event*/) {}

        virtual void focus() {}

        virtual void unfocus() {}

        /**
         * True while this component holds keyboard focus. Gadgets that
         * do not track focus return false.
         */
        virtual bool isFocused() const { return false; }

        /**
         * True while this component owns all keyboard input: the panel
         * routes keys to it exclusively and sibling quickkeys are
         * suppressed. Retail dialog rule -- while the focused gadget is
         * a TextBox (type 3) its handler consumes characters and every
         * button's quickkey path is skipped unless Alt is held
         * (TotalA.exe 0x4a70a7 gates the quickkey compare on the
         * focused gadget's type plus GetAsyncKeyState(VK_MENU)).
         */
        virtual bool capturesTextInput() const { return false; }

        virtual void render(UiRenderService& /*graphics*/) const {}

        virtual void mouseMove(MouseMoveEvent event);

        virtual void mouseWheel(MouseWheelEvent /*event*/) {}

        virtual void update(float /*dt*/) {}

        virtual void uiMessage(const GroupMessage& /*message*/) {}

        virtual bool contains(int x, int y) const
        {
            auto minX = posX;
            auto maxX = posX + static_cast<int>(sizeX) - 1;
            auto minY = posY;
            auto maxY = posY + static_cast<int>(sizeY) - 1;
            return x >= minX && x <= maxX && y >= minY && y <= maxY;
        }

        /**
         * True if this component can consume mouse button input.
         * Display-only gadgets (labels, surfaces, light bars) return
         * false so that clicks pass through to interactive gadgets
         * underneath them in the panel's layout.
         */
        virtual bool acceptsMouseInput() const
        {
            return true;
        }

        void addSubscription(std::unique_ptr<Subscription>&& s);

        /**
         * Hides or shows this component. Hidden components are skipped
         * by the panel's render and input dispatch.
         */
        void setHidden(bool newHidden)
        {
            hidden = newHidden;
        }

        bool isHidden() const
        {
            return hidden;
        }

        const std::string& getName() const;
        void setName(const std::string& newName);
        void setName(std::string&& newName);

        unsigned int getGroup() const;
        void setGroup(unsigned int newGroup);

        Observable<const ControlMessage&>& messages();
        const Observable<const ControlMessage&>& messages() const;
    };
}
