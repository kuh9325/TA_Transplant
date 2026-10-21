#pragma once

#include <memory>
#include <optional>
#include <rwe/geometry/Rectangle2f.h>
#include <rwe/observable/BehaviorSubject.h>
#include <rwe/render/SpriteSeries.h>
#include <rwe/ui/UiComponent.h>
#include <vector>

namespace rwe
{
    class UiListBox : public UiComponent
    {
    private:
        class ListBoxUiMessageVisitor
        {
        private:
            UiListBox* listBox;

        public:
            explicit ListBoxUiMessageVisitor(UiListBox* listBox) : listBox(listBox)
            {
            }

            void operator()(const ScrollPositionMessage& msg) const
            {
                auto scrollPos = static_cast<unsigned int>(msg.scrollPosition * listBox->maxScrollPosition());
                listBox->scrollPositionSubject.next(scrollPos);
            }

            void operator()(const ScrollUpMessage& /*msg*/) const
            {
                listBox->scrollUp();
            }

            void operator()(const ScrollDownMessage& /*msg*/) const
            {
                listBox->scrollDown();
            }

            template <typename T>
            void operator()(const T& /*msg*/) const
            {
                // do nothing
            }
        };


    private:
        std::vector<std::string> items;

        /**
         * Optional per-item text colors, parallel to `items`.
         * Retail's ENDMSN mission list prefixes each entry with a
         * status colour code (0xfd/0xfe/0xff for U/W/L); RWE fonts do
         * not implement colour escapes, so the colour rides alongside
         * the item instead. Empty entries fall back to white.
         */
        std::vector<std::optional<Color>> itemColors;

        Subject<bool> itemsChangedSubject;
        std::shared_ptr<SpriteSeries> font;
        BehaviorSubject<std::optional<unsigned int>> selectedIndexSubject;
        BehaviorSubject<unsigned int> scrollPositionSubject{0};

        /**
         * The region the list box is actually visible inside,
         * in parent coordinates. GUI layouts can declare list boxes
         * whose rects overlap; this is the exclusive region that
         * remains for this list box. Defaults to the component rect.
         */
        Rectangle2f clipRect;

    public:
        UiListBox(int posX, int posY, unsigned int sizeX, unsigned int sizeY, std::shared_ptr<SpriteSeries> font);
        void appendItem(std::string item);

        /** Appends an item drawn in the given text color. */
        void appendItem(std::string item, const Color& color);

        /** Removes all items and clears the current selection. */
        void clearItems();

        void setSelectedItem(const std::string& item);

        /** Selects an item by index and scrolls it into view. */
        void setSelectedIndex(unsigned int index);

        void clearSelectedItem();

        void render(UiRenderService& context) const override;

        bool contains(int x, int y) const override;

        void mouseDown(MouseButtonEvent event) override;

        void mouseWheel(MouseWheelEvent event) override;

        bool keyDown(KeyEvent event) override;

        void uiMessage(const GroupMessage& message) override;

        Observable<std::optional<unsigned int>>& selectedIndex();

        const Observable<std::optional<unsigned int>>& selectedIndex() const;

        /** Synchronous read of the current selection (none when empty). */
        std::optional<unsigned int> getSelectedIndex() const
        {
            return selectedIndexSubject.getValue();
        }

        Observable<bool>& itemsChanged();

        const Observable<bool>& itemsChanged() const;

        Observable<unsigned int>& scrollPosition();

        const Observable<unsigned int>& scrollPosition() const;

        const std::vector<std::string>& getItems();

        float getScrollPercent() const;

        float getViewportPercent() const;

        void setClipRect(const Rectangle2f& rect);

        const Rectangle2f& getClipRect() const;

    private:
        unsigned int numberOfLines() const;

        std::optional<unsigned int> pixelToLine(int y) const;

        void setScrollPosition(unsigned int newPosition);

        void setScrollPositionCentered(unsigned int newPosition);

        void setScrollPositionLast(unsigned int newPosition);

        unsigned int maxScrollPosition() const;

        void scrollUp();

        void scrollDown();

        void selectNextListItem();

        void selectPreviousListItem();

        void ensureIndexInView(unsigned int index);
    };
}
