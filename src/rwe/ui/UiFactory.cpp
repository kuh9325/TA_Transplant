#include "UiFactory.h"
#include <algorithm>
#include <rwe/ui/UiSurface.h>
#include <rwe/ui/button_sound_util.h>
#include <rwe/util/Index.h>

#include <memory>
#include <rwe/MainMenuScene.h>
#include <rwe/ui/UiComponent.h>
#include <rwe/util/rwe_string.h>

namespace rwe
{
    /**
     * Converts a quickkey value into the equivalent SDL key code.
     *
     * Quickkey values represent letters with their uppercase ASCII value,
     * but SDL keycodes use the lowercase value.
     */
    int convertQuickKeyToSdlk(int quickKey)
    {
        if (quickKey >= 65 && quickKey <= 132) // is uppercase ASCII
        {
            // set bit 6 ON to shift to lowercase ASCII
            return quickKey | 0x20;
        }

        return quickKey;
    }

    UiFactory::UiFactory(TextureService* textureService, AudioService* audioService, TdfBlock* soundLookup, AbstractVirtualFileSystem* vfs, const PathMapping* const pathMapping, int screenWidth, int screenHeight)
        : textureService(textureService), audioService(audioService), soundLookup(soundLookup), vfs(vfs), pathMapping(pathMapping), screenWidth(screenWidth), screenHeight(screenHeight)
    {
    }

    std::unique_ptr<UiPanel> UiFactory::panelFromGuiFile(const std::string& name, const std::string& background, const std::vector<GuiEntry>& entries)
    {
        // first entry sets up the panel
        assert(entries.size() > 0);
        auto panelEntry = entries[0];

        auto texture = textureService->getBitmapRegion(
            background,
            0,
            0,
            panelEntry.common.width,
            panelEntry.common.height);


        // Adjust x and y pos such that the bottom and right edges of the panel
        // do not go over the edge of the screen.
        auto rightBound = panelEntry.common.xpos + panelEntry.common.width;
        int xPos = panelEntry.common.xpos;
        if (rightBound > 640)
        {
            xPos -= rightBound - 640;
        }

        auto bottomBound = panelEntry.common.ypos + panelEntry.common.height;
        int yPos = panelEntry.common.ypos;
        if (bottomBound > 480)
        {
            yPos -= bottomBound - 480;
        }

        auto panel = std::make_unique<UiPanel>(
            xPos,
            yPos,
            panelEntry.common.width,
            panelEntry.common.height,
            texture);
        panel->setName(name);

        // load panel components
        for (std::size_t i = 1; i < entries.size(); ++i)
        {
            auto& entry = entries[i];

            auto elem = componentFromGuiEntry(name, entry);

            elem->setName(entry.common.name);
            elem->setGroup(entry.common.assoc);

            panel->appendChild(std::move(elem));
        }

        // set the default focused control
        if (panelEntry.defaultFocus)
        {
            const auto& focusName = *panelEntry.defaultFocus;
            panel->setFocusByName(focusName);
        }

        return panel;
    }

    std::unique_ptr<UiPanel> UiFactory::panelFromGuiFile(const std::string& name)
    {
        auto entries = vfs->readGuiOrThrow(pathMapping->guis + "/" + name + ".GUI");
        return panelFromGuiFile(name, entries);
    }

    std::unique_ptr<UiPanel> UiFactory::panelFromGuiFile(const std::string& name, const std::vector<GuiEntry>& entries)
    {
        // first entry sets up the panel
        assert(entries.size() > 0);
        const auto& panelEntry = entries[0];
        auto panel = createPanel(
            panelEntry.common.xpos,
            panelEntry.common.ypos,
            panelEntry.common.width,
            panelEntry.common.height,
            name,
            panelEntry.panel);

        // load panel components
        for (std::size_t i = 1; i < entries.size(); ++i)
        {
            auto& entry = entries[i];

            auto elem = componentFromGuiEntry(name, entry);

            elem->setName(entry.common.name);
            elem->setGroup(entry.common.assoc);

            panel->appendChild(std::move(elem));
        }

        // set the default focused control
        if (panelEntry.defaultFocus)
        {
            const auto& focusName = *panelEntry.defaultFocus;
            panel->setFocusByName(focusName);
        }

        if (panelEntry.escdefault)
        {
            panel->setEscDefault(*panelEntry.escdefault);
        }
        if (panelEntry.crDefault)
        {
            panel->setCrDefault(*panelEntry.crDefault);
        }

        return panel;
    }

    std::unique_ptr<UiPanel> UiFactory::createPanel(int x, int y, int width, int height, const std::string& name)
    {
        return createPanel(x, y, width, height, name, std::nullopt);
    }

    std::unique_ptr<UiPanel> UiFactory::createPanel(int x, int y, int width, int height, const std::string& name, const std::optional<std::string>& background)
    {
        std::optional<std::shared_ptr<Sprite>> backgroundSprite;
        if (background)
        {
            if (auto backgroundSpriteSeries = textureService->getGuiTexture(name, *background))
            {
                backgroundSprite = (*backgroundSpriteSeries)->sprites.at(0);
            }
        }

        // Adjust x and y pos such that the bottom and right edges of the panel
        // do not go over the edge of the screen.
        auto rightBound = x + width;
        if (rightBound > screenWidth)
        {
            x -= rightBound - screenWidth;
        }

        auto bottomBound = y + height;
        if (bottomBound > screenHeight)
        {
            y -= bottomBound - screenHeight;
        }

        auto panel = std::make_unique<UiPanel>(x, y, width, height, backgroundSprite);
        panel->setName(name);
        return panel;
    }

    std::unique_ptr<UiStagedButton>
    UiFactory::createButton(int x, int y, int width, int height, const std::string& guiName, const std::string& name, const std::string& label)
    {
        // hack for SINGLE.GUI buttons
        if (width == 118 && height == 18)
        {
            width = 120;
            height = 20;
        }

        auto sprites = getButtonGraphics(guiName, name, width, height);

        std::vector<UiStagedButton::StageInfo> stages;
        for (const auto& s : sprites.normal)
        {
            stages.emplace_back(s, label);
        }

        auto font = textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");

        auto button = std::make_unique<UiStagedButton>(x, y, width, height, stages, sprites.pressed, font);

        auto sound = deduceButtonSound(guiName, name, width, height);

        if (sound)
        {
            button->onClick().subscribe([as = audioService, s = std::move(*sound)](const auto& /*param*/) {
                as->playSound(s);
            });
        }

        return button;
    }

    std::unique_ptr<UiStagedButton>
    UiFactory::createBasicButton(int x, int y, int width, int height, const std::string& guiName, const std::string& name, const std::string& label)
    {
        // hack for SINGLE.GUI buttons
        if (width == 118 && height == 18)
        {
            width = 120;
            height = 20;
        }

        auto sprites = getBasicButtonGraphics(guiName, name, width, height);

        std::vector<UiStagedButton::StageInfo> stages;
        for (const auto& s : sprites.normal)
        {
            stages.emplace_back(s, label);
        }

        auto font = textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");

        auto button = std::make_unique<UiStagedButton>(x, y, width, height, stages, sprites.pressed, font);

        auto sound = deduceButtonSound(guiName, name, width, height);

        if (sound)
        {
            button->onClick().subscribe([as = audioService, s = std::move(*sound)](const auto& /*param*/) {
                as->playSound(s);
            });
        }

        return button;
    }

    std::unique_ptr<UiStagedButton>
    UiFactory::createStagedButton(int x, int y, int width, int height, const std::string& guiName, const std::string& name, const std::vector<std::string>& labels, unsigned int stages)
    {
        auto sprites = getStagedButtonGraphics(guiName, name, stages);

        auto font = textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");

        if (sprites.normal.size() != labels.size())
        {
            throw std::runtime_error("Number of labels does not match number of sprites");
        }

        std::vector<UiStagedButton::StageInfo> stageInfos;
        for (Index i = 0; i < getSize(sprites.normal); ++i)
        {
            stageInfos.emplace_back(sprites.normal[i], labels[i]);
        }

        auto button = std::make_unique<UiStagedButton>(x, y, width, height, stageInfos, sprites.pressed, font);

        // staged buttons always display labels aligned left,
        // they don't care about text alignment attribs.
        button->setTextAlign(UiStagedButton::TextAlign::Left);

        // staged buttons always use staged mode
        button->setBehaviorMode(UiStagedButton::BehaviorMode::Staged);

        auto sound = deduceButtonSound(guiName, name, width, height);

        if (sound)
        {
            button->onClick().subscribe([as = audioService, s = std::move(*sound)](const auto& /*param*/) {
                as->playSound(s);
            });
        }

        return button;
    }

    std::unique_ptr<UiComponent> UiFactory::componentFromGuiEntry(const std::string& guiName, const GuiEntry& entry)
    {
        switch (entry.common.id)
        {
            case GuiElementType::Button:
            {
                auto stages = entry.stages.value_or(0);
                if (stages >= 1)
                {
                    return stagedButtonFromGuiEntry(guiName, entry);
                }
                else
                {
                    return buttonFromGuiEntry(guiName, entry);
                }
            }
            case GuiElementType::ListBox:
                return listBoxFromGuiEntry(guiName, entry);
            case GuiElementType::Label:
                return labelFromGuiEntry(guiName, entry);
            case GuiElementType::ScrollBar:
                return scrollBarFromGuiEntry(guiName, entry);
            case GuiElementType::TextBox:
                return textBoxFromGuiEntry(guiName, entry);
            case GuiElementType::Surface:
            case GuiElementType::PictureBox:
                // a PictureBox is a named image gadget; its sprite is
                // the same-named GAF entry, assigned by the caller
                return surfaceFromGuiEntry(guiName, entry);
            default:
                return std::make_unique<UiComponent>(0, 0, 1, 1);
        }
    }

    std::unique_ptr<UiStagedButton> UiFactory::buttonFromGuiEntry(const std::string& guiName, const GuiEntry& entry)
    {
        auto text = entry.text ? *(entry.text) : std::string("");
        auto button = createButton(
            entry.common.xpos,
            entry.common.ypos,
            entry.common.width,
            entry.common.height,
            guiName,
            entry.common.name,
            text);

        if (entry.common.attribs & GuiButtonAttrib::LabelDisplayLeft)
        {
            button->setTextAlign(UiStagedButton::TextAlign::Left);
        }
        else if (entry.common.attribs & GuiButtonAttrib::LabelDisplayCenter)
        {
            button->setTextAlign(UiStagedButton::TextAlign::Center);
        }

        bool isToggleStyle = false;
        if (entry.common.attribs & GuiButtonAttrib::BehaviorRadio)
        {
            button->setBehaviorMode(UiStagedButton::BehaviorMode::Radio);
            isToggleStyle = true;
        }
        else if (entry.common.attribs & GuiButtonAttrib::BehaviorBuildButton)
        {
            button->setTextAlign(UiStagedButton::TextAlign::BottomCenter);
        }
        else if (entry.common.attribs & GuiButtonAttrib::BehaviorToggle)
        {
            button->setBehaviorMode(UiStagedButton::BehaviorMode::Toggle);
            isToggleStyle = true;
        }
        else if (entry.common.attribs & GuiButtonAttrib::BehaviorCycle)
        {
            button->setBehaviorMode(UiStagedButton::BehaviorMode::Cycle);
        }

        // radio/toggle buttons carry their initial pressed state in status
        if (isToggleStyle && entry.status && *entry.status != 0)
        {
            button->setToggledOn(true);
        }

        if (entry.quickKey)
        {
            button->setQuickKey(convertQuickKeyToSdlk(*entry.quickKey));
        }

        if (entry.grayedOut && *entry.grayedOut)
        {
            button->setEnabled(false);
        }

        return button;
    }

    std::unique_ptr<UiLabel> UiFactory::labelFromGuiEntry(const std::string& /*guiName*/, const GuiEntry& entry)
    {
        auto font = textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");

        auto label = std::make_unique<UiLabel>(
            entry.common.xpos,
            entry.common.ypos,
            entry.common.width,
            entry.common.height,
            entry.text.value_or(""),
            font);

        // retail labels honor the same display-alignment attrib bit as
        // buttons: set -> centered within the gadget rect (e.g. the
        // ARMOPT "GAME OPTIONS" title), clear -> left-aligned
        if (entry.common.attribs & GuiButtonAttrib::LabelDisplayCenter)
        {
            label->setAlignment(UiLabel::Alignment::CenterRect);
        }

        return label;
    }

    std::unique_ptr<UiStagedButton> UiFactory::stagedButtonFromGuiEntry(
        const std::string& guiName,
        const GuiEntry& entry)
    {
        auto labels = entry.text ? utf8Split(*entry.text, '|') : std::vector<std::string>();
        auto button = createStagedButton(
            entry.common.xpos,
            entry.common.ypos,
            entry.common.width,
            entry.common.height,
            guiName,
            entry.common.name,
            labels,
            entry.stages.value());

        // staged buttons select their initial stage via status
        if (entry.status && *entry.status != 0 && *entry.status < static_cast<int>(entry.stages.value()))
        {
            button->setStage(*entry.status);
        }

        if (entry.quickKey)
        {
            button->setQuickKey(convertQuickKeyToSdlk(*entry.quickKey));
        }

        if (entry.grayedOut && *entry.grayedOut)
        {
            button->setEnabled(false);
        }

        return button;
    }

    std::shared_ptr<SpriteSeries> UiFactory::getDefaultStagedButtonGraphics(const std::string& guiName, unsigned int stages)
    {
        assert(stages >= 1 && stages <= 4);
        std::string entryName("stagebuttn");
        entryName.append(std::to_string(stages));

        auto sprites = textureService->getGuiTexture(guiName, entryName);
        if (sprites)
        {
            return *sprites;
        }

        // default behaviour
        const auto& sprite = textureService->getDefaultSprite();
        auto series = std::make_shared<SpriteSeries>();
        for (unsigned int i = 0; i < stages; ++i)
        {
            series->sprites.push_back(sprite);
        }
        series->sprites.push_back(sprite); // multi-choice
        series->sprites.push_back(sprite); // pressed
        series->sprites.push_back(sprite); // disabled
        return series;
    }

    std::unique_ptr<UiListBox> UiFactory::listBoxFromGuiEntry(const std::string& guiName, const GuiEntry& entry)
    {
        auto font = textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");

        auto listBox = std::make_unique<UiListBox>(
            entry.common.xpos,
            entry.common.ypos,
            entry.common.width,
            entry.common.height,
            font);

        return listBox;
    }

    std::unique_ptr<UiTextBox> UiFactory::textBoxFromGuiEntry(const std::string& /*guiName*/, const GuiEntry& entry)
    {
        auto font = textureService->getGafEntry("anims/hattfont12.gaf", "Haettenschweiler (120)");

        auto textBox = std::make_unique<UiTextBox>(
            entry.common.xpos,
            entry.common.ypos,
            entry.common.width,
            entry.common.height,
            entry.text.value_or(""),
            font,
            entry.maxChars ? static_cast<unsigned int>(*entry.maxChars) : 99u);

        return textBox;
    }

    std::optional<AudioService::SoundHandle> UiFactory::deduceButtonSound(const std::string& guiName, const GuiEntry& entry)
    {
        return deduceButtonSound(guiName, entry.common.name, entry.common.width, entry.common.height);
    }

    std::optional<AudioService::SoundHandle> UiFactory::deduceButtonSound(const std::string& guiName, const std::string& name, int width, int height)
    {
        for (const auto& key : deduceButtonSoundKeys(guiName, name, width, height))
        {
            if (auto sound = getButtonSound(key))
            {
                return sound;
            }
        }

        return std::nullopt;
    }

    std::unique_ptr<UiComponent> UiFactory::scrollBarFromGuiEntry(const std::string& guiName, const GuiEntry& entry)
    {
        auto sprites = textureService->getGuiTexture(guiName, "SLIDERS");
        if (!sprites)
        {
            throw std::runtime_error("Missing SLIDERS gaf entry");
        }

        if (entry.common.attribs & GuiScrollHorizontalAttrib)
        {
            // horizontal option sliders have no runtime backend yet;
            // they are shown as disabled display gadgets
            if ((*sprites)->sprites.size() < 20)
            {
                // the stock horizontal art is frames 10-19 of SLIDERS
                return std::make_unique<UiComponent>(
                    entry.common.xpos,
                    entry.common.ypos,
                    entry.common.width,
                    entry.common.height);
            }
            return std::make_unique<UiSlider>(
                entry.common.xpos,
                entry.common.ypos,
                entry.common.width,
                entry.common.height,
                *sprites);
        }

        return std::make_unique<UiScrollBar>(
            entry.common.xpos,
            entry.common.ypos,
            entry.common.width,
            entry.common.height,
            *sprites);
    }

    std::shared_ptr<SpriteSeries> UiFactory::getDefaultButtonGraphics(const std::string& guiName, int width, int height)
    {
        auto sprites = textureService->getGuiTexture(guiName, "BUTTONS0");
        if (sprites)
        {
            const auto& frames = (*sprites)->sprites;
            auto it = std::find_if(
                frames.begin(),
                frames.end(),
                [width, height](const std::shared_ptr<Sprite>& s) {
                    return s->bounds.width() == width && s->bounds.height() == height;
                });

            // stock gadgets don't always match the BUTTONS0 frame grid
            // exactly (BRIEFING's OK button is 95x20 against 96x20
            // frames); retry with the closest same-height frame
            if (it == frames.end())
            {
                it = std::min_element(
                    frames.begin(),
                    frames.end(),
                    [width, height](const std::shared_ptr<Sprite>& a, const std::shared_ptr<Sprite>& b) {
                        auto score = [width, height](const std::shared_ptr<Sprite>& s) {
                            return std::abs(static_cast<int>(s->bounds.height()) - height) * 10000 + std::abs(static_cast<int>(s->bounds.width()) - width);
                        };
                        return score(a) < score(b);
                    });
            }

            if (it != frames.end())
            {
                if (frames.end() - it < 3)
                {
                    throw std::runtime_error(
                        "Not enough sprites in BUTTONS0 for button of size " + std::to_string(width) + ", " + std::to_string(height));
                }
                auto spritesView = std::make_shared<SpriteSeries>();
                spritesView->sprites.push_back(*(it++));
                spritesView->sprites.push_back(*(it++));
                spritesView->sprites.push_back(*(it++));
                return spritesView;
            }
        }

        // default behaviour
        const auto& sprite = textureService->getDefaultSprite();
        auto series = std::make_shared<SpriteSeries>();
        series->sprites.push_back(sprite);
        series->sprites.push_back(sprite);
        series->sprites.push_back(sprite);
        return series;
    }

    std::optional<AudioService::SoundHandle> UiFactory::getButtonSound(const std::string& buttonName)
    {
        auto soundBlock = soundLookup->findBlock(buttonName);
        if (!soundBlock)
        {
            return std::nullopt;
        }

        auto soundName = soundBlock->get().findValue("sound");
        if (!soundName)
        {
            return std::nullopt;
        }

        return audioService->loadSound(*soundName);
    }

    void UiFactory::playButtonSound(const std::string& buttonName)
    {
        if (auto sound = getButtonSound(buttonName))
        {
            audioService->playSound(*sound);
        }
    }

    std::unique_ptr<UiComponent> UiFactory::surfaceFromGuiEntry(const std::string& /*guiName*/, const GuiEntry& entry)
    {
        auto surface = std::make_unique<UiSurface>(
            entry.common.xpos,
            entry.common.ypos,
            entry.common.width,
            entry.common.height);

        return surface;
    }

    UiFactory::ButtonSprites UiFactory::getButtonGraphics(
        const std::string& guiName,
        const std::string& name,
        int width,
        int height)
    {
        auto graphics = textureService->getGuiTexture(guiName, name);
        if (!graphics)
        {
            graphics = getDefaultButtonGraphics(guiName, width, height);
        }

        const auto& sprites = (*graphics)->sprites;
        auto spriteCount = sprites.size();

        auto defaultSprite = textureService->getDefaultSprite();
        if (spriteCount == 0)
        {
            std::vector<std::shared_ptr<Sprite>> normalSprites{defaultSprite};
            return ButtonSprites{normalSprites, defaultSprite, defaultSprite};
        }

        auto stageCount = spriteCount > 3 ? spriteCount - 2 : 1;

        std::vector<std::shared_ptr<Sprite>> normalSprites;
        for (unsigned int i = 0; i < stageCount; ++i)
        {
            normalSprites.push_back(sprites[i]);
        }

        const auto& pressedSprite = stageCount < spriteCount ? sprites[stageCount] : defaultSprite;
        const auto& disabledSprite = stageCount + 1 < spriteCount ? sprites[stageCount + 1] : defaultSprite;

        return UiFactory::ButtonSprites{normalSprites, pressedSprite, disabledSprite};
    }

    UiFactory::ButtonSprites UiFactory::getBasicButtonGraphics(
        const std::string& guiName,
        const std::string& name,
        int width,
        int height)
    {
        auto graphics = textureService->getGuiTexture(guiName, name);
        if (!graphics)
        {
            graphics = getDefaultButtonGraphics(guiName, width, height);
        }

        const auto& sprites = (*graphics)->sprites;
        auto spriteCount = sprites.size();

        auto defaultSprite = textureService->getDefaultSprite();

        const auto& normalSprite = spriteCount > 0 ? sprites[0] : defaultSprite;
        const auto& pressedSprite = spriteCount > 1 ? sprites[1] : defaultSprite;
        const auto& disabledSprite = spriteCount > 2 ? sprites[2] : defaultSprite;

        return UiFactory::ButtonSprites{std::vector<std::shared_ptr<Sprite>>{normalSprite}, pressedSprite, disabledSprite};
    }

    UiFactory::ButtonSprites UiFactory::getStagedButtonGraphics(const std::string& guiName, const std::string& name, unsigned int stages)
    {
        auto graphics = textureService->getGuiTexture(guiName, name);
        if (!graphics)
        {
            graphics = getDefaultStagedButtonGraphics(guiName, stages);
        }

        auto defaultSprite = textureService->getDefaultSprite();

        const auto& sprites = (*graphics)->sprites;
        auto spriteCount = sprites.size();

        auto stagesPresentCount = spriteCount > 2 ? spriteCount - 2 : 0;
        auto stagesToCopyCount = std::min<unsigned int>(stages, stagesPresentCount);

        std::vector<std::shared_ptr<Sprite>> normalSprites;
        for (unsigned int i = 0; i < stagesToCopyCount; ++i)
        {
            normalSprites.push_back(sprites[i]);
        }
        for (unsigned int i = stagesToCopyCount; i < stages; ++i)
        {
            normalSprites.push_back(defaultSprite);
        }
        assert(normalSprites.size() == stages);

        const auto& pressedSprite = stagesPresentCount < spriteCount ? sprites[stagesPresentCount] : defaultSprite;
        const auto& disabledSprite = stagesPresentCount + 1 < spriteCount ? sprites[stagesPresentCount + 1] : defaultSprite;

        return UiFactory::ButtonSprites{normalSprites, pressedSprite, disabledSprite};
    }
}
