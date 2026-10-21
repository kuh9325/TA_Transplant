#include "UiPanel.h"
#include <SDL3/SDL_keyboard.h>
#include <algorithm>
#include <rwe/ui/UiListBox.h>
#include <rwe/util/rwe_string.h>

namespace rwe
{
    namespace
    {
        Rectangle2f componentRect(UiComponent& c)
        {
            return Rectangle2f::fromTopLeft(
                static_cast<float>(c.getX()),
                static_cast<float>(c.getY()),
                static_cast<float>(c.getWidth()),
                static_cast<float>(c.getHeight()));
        }

        /**
         * Subtracts sibling rect s from rect a, when s cuts cleanly
         * across one of a's edges. Overlaps that do not span an edge
         * leave a unchanged.
         */
        Rectangle2f subtractClipRect(const Rectangle2f& a, const Rectangle2f& s)
        {
            bool spansX = s.left() <= a.left() && s.right() >= a.right();
            bool spansY = s.top() <= a.top() && s.bottom() >= a.bottom();

            // s covers a's bottom edge
            if (spansX && s.top() > a.top() && s.top() < a.bottom() && s.bottom() >= a.bottom())
            {
                return Rectangle2f::fromTLBR(a.top(), a.left(), s.top(), a.right());
            }
            // s covers a's top edge
            if (spansX && s.top() <= a.top() && s.bottom() > a.top() && s.bottom() < a.bottom())
            {
                return Rectangle2f::fromTLBR(s.bottom(), a.left(), a.bottom(), a.right());
            }
            // s covers a's right edge
            if (spansY && s.left() > a.left() && s.left() < a.right() && s.right() >= a.right())
            {
                return Rectangle2f::fromTLBR(a.top(), a.left(), a.bottom(), s.left());
            }
            // s covers a's left edge
            if (spansY && s.left() <= a.left() && s.right() > a.left() && s.right() < a.right())
            {
                return Rectangle2f::fromTLBR(a.top(), s.right(), a.bottom(), a.right());
            }
            return a;
        }
    }

    UiPanel::UiPanel(int posX, int posY, unsigned int sizeX, unsigned int sizeY)
        : UiComponent(posX, posY, sizeX, sizeY)
    {
    }

    UiPanel::UiPanel(int posX, int posY, unsigned int sizeX, unsigned int sizeY, std::shared_ptr<Sprite> background)
        : UiComponent(posX, posY, sizeX, sizeY),
          background(std::move(background))
    {
    }

    UiPanel::UiPanel(int posX, int posY, unsigned int sizeX, unsigned int sizeY, std::optional<std::shared_ptr<Sprite>> background)
        : UiComponent(posX, posY, sizeX, sizeY),
          background(background)
    {
    }

    UiPanel::UiPanel(UiPanel&& panel) noexcept
        : UiComponent(panel.posX, panel.posY, panel.sizeX, panel.sizeY),
          background(std::move(panel.background)),
          children(std::move(panel.children)),
          focusedChild(std::move(panel.focusedChild))
    {
    }

    UiPanel& UiPanel::operator=(UiPanel&& panel) noexcept
    {
        posX = panel.posX;
        posY = panel.posY;
        sizeX = panel.sizeX;
        sizeY = panel.sizeY;
        background = panel.background;
        children = std::move(panel.children);
        focusedChild = std::move(panel.focusedChild);

        return *this;
    }

    void UiPanel::render(UiRenderService& graphics) const
    {
        if (background)
        {
            graphics.drawSpriteAbs(posX, posY, **background);
        }

        graphics.pushMatrix();
        graphics.multiplyMatrix(Matrix4f::translation(Vector3f(posX, posY, 0.0f)));

        for (const auto& i : children)
        {
            if (!i->isHidden())
            {
                i->render(graphics);
            }
        }

        graphics.popMatrix();
    }

    void UiPanel::mouseDown(MouseButtonEvent event)
    {
        event.x -= posX;
        event.y -= posY;

        auto size = children.size();
        for (std::size_t i = 0; i < size; ++i)
        {
            auto& c = children[i];
            if (!c->isHidden() && c->acceptsMouseInput() && c->contains(event.x, event.y))
            {
                setFocus(i);
                c->mouseDown(event);
                return;
            }
        }
    }

    void UiPanel::mouseUp(MouseButtonEvent event)
    {
        if (!focusedChild)
        {
            return;
        }

        event.x -= posX;
        event.y -= posY;

        (*focusedChild)->mouseUp(event);
    }

    bool UiPanel::keyDown(KeyEvent event)
    {
        // retail dialog rule: while a TextBox owns keyboard focus it
        // receives every key exclusively and sibling button quickkeys
        // are suppressed; Alt-held keys still reach quickkeys
        // (TotalA.exe 0x4a70a7 gates the quickkey compare on the focused
        // gadget's type plus GetAsyncKeyState(VK_MENU)). Gadget kinds
        // that do not track focus report capturesTextInput()==false and
        // fall through to the broadcast unchanged.
        if (focusedChild && (*focusedChild)->capturesTextInput()
            && (SDL_GetModState() & SDL_KMOD_ALT) == 0)
        {
            auto* focused = *focusedChild;
            bool handled = focused->keyDown(event);
            // a TextBox releases focus on Enter/Escape; drop the stale
            // pointer so later keys broadcast normally again
            if (!focused->isFocused())
            {
                focusedChild = std::nullopt;
            }
            return handled;
        }

        // a child's handler may append or remove children mid-broadcast
        // (dialog paging, message-driven rebuilds); advance only when
        // slot i still holds the dispatched child, so removals shift
        // the next unvisited child into the slot rather than skipping it
        bool handled = false;
        for (std::size_t i = 0; i < children.size();)
        {
            auto* e = children[i].get();
            if (!e->isHidden())
            {
                bool childHandled = e->keyDown(event);
                handled = childHandled || handled;
            }
            if (i < children.size() && children[i].get() == e)
            {
                ++i;
            }
        }
        return handled;
    }

    void UiPanel::keyUp(KeyEvent event)
    {
        for (std::size_t i = 0; i < children.size();)
        {
            auto* e = children[i].get();
            if (!e->isHidden())
            {
                e->keyUp(event);
            }
            if (i < children.size() && children[i].get() == e)
            {
                ++i;
            }
        }
    }

    void UiPanel::mouseMove(MouseMoveEvent event)
    {
        UiComponent::mouseMove(event);

        event.x -= posX;
        event.y -= posY;

        for (std::size_t i = 0; i < children.size();)
        {
            auto* e = children[i].get();
            if (!e->isHidden())
            {
                e->mouseMove(event);
            }
            if (i < children.size() && children[i].get() == e)
            {
                ++i;
            }
        }
    }

    void UiPanel::focus()
    {
        if (focusedChild)
        {
            (*focusedChild)->focus();
        }
    }

    void UiPanel::unfocus()
    {
        if (focusedChild)
        {
            (*focusedChild)->unfocus();
        }
    }

    void UiPanel::appendChild(std::unique_ptr<UiComponent>&& c)
    {
        auto& component = children.emplace_back(std::move(c));
        component->messages().subscribe([this, &cc = *component](const auto& message) {
            GroupMessage groupMessage(this->name, cc.getGroup(), cc.getName(), message);
            this->uiMessage(groupMessage);
            this->groupMessagesSubject.next(groupMessage);
        });

        updateListBoxClipRects();
    }

    void UiPanel::updateListBoxClipRects()
    {
        for (std::size_t i = 0; i < children.size(); ++i)
        {
            auto* listBox = dynamic_cast<UiListBox*>(children[i].get());
            if (listBox == nullptr)
            {
                continue;
            }

            auto clip = componentRect(*listBox);
            for (std::size_t j = i + 1; j < children.size(); ++j)
            {
                if (auto* other = dynamic_cast<UiListBox*>(children[j].get()); other != nullptr)
                {
                    clip = subtractClipRect(clip, componentRect(*other));
                }
            }
            listBox->setClipRect(clip);
        }
    }

    void UiPanel::setFocus(std::size_t controlIndex)
    {
        assert(controlIndex < children.size());

        if (focusedChild)
        {
            (*focusedChild)->unfocus();
        }

        focusedChild = children[controlIndex].get();
        (*focusedChild)->focus();
    }

    void UiPanel::clearFocus()
    {
        if (focusedChild)
        {
            (*focusedChild)->unfocus();
        }

        focusedChild = std::nullopt;
    }

    void UiPanel::update(float dt)
    {
        for (std::size_t i = 0; i < children.size();)
        {
            auto* e = children[i].get();
            e->update(dt);
            if (i < children.size() && children[i].get() == e)
            {
                ++i;
            }
        }
    }

    void UiPanel::mouseWheel(MouseWheelEvent event)
    {
        if (!focusedChild)
        {
            return;
        }

        (*focusedChild)->mouseWheel(event);
    }

    void UiPanel::uiMessage(const GroupMessage& message)
    {
        for (std::size_t i = 0; i < children.size();)
        {
            auto* c = children[i].get();
            if (!c->isHidden())
            {
                c->uiMessage(message);
            }
            if (i < children.size() && children[i].get() == c)
            {
                ++i;
            }
        }
    }

    std::vector<std::unique_ptr<UiComponent>>& UiPanel::getChildren()
    {
        return children;
    }

    void UiPanel::removeChildrenWithPrefix(const std::string& prefix)
    {
        children.erase(
            std::remove_if(
                children.begin(),
                children.end(),
                [&prefix](const auto& e) { return startsWith(e->getName(), prefix); }),
            children.end());

        // the focused child may have been among the removed entries;
        // dropping the stale pointer keeps mouseUp/wheel dispatch safe
        if (focusedChild)
        {
            auto* focused = *focusedChild;
            if (std::none_of(children.begin(), children.end(), [focused](const auto& c) { return c.get() == focused; }))
            {
                focusedChild = std::nullopt;
            }
        }

        updateListBoxClipRects();
    }

    void UiPanel::setFocusByName(const std::string& name)
    {
        auto it = std::find_if(children.begin(), children.end(), [&name](const auto& c) { return c->getName() == name; });
        if (it != children.end())
        {
            setFocus(it - children.begin());
        }
    }

    void UiPanel::setEscDefault(const std::string& name)
    {
        escDefault = name;
    }

    void UiPanel::setCrDefault(const std::string& name)
    {
        crDefault = name;
    }

    const std::string& UiPanel::getEscDefault() const
    {
        return escDefault;
    }

    const std::string& UiPanel::getCrDefault() const
    {
        return crDefault;
    }

    Observable<GroupMessage>& UiPanel::groupMessages()
    {
        return groupMessagesSubject;
    }

    const Observable<GroupMessage>& UiPanel::groupMessages() const
    {
        return groupMessagesSubject;
    }
}
