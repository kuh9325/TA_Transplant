#pragma once

#include <array>
#include <optional>
#include <rwe/SkirmishSettings.h>
#include <rwe/events.h>
#include <rwe/game/PlayerColorIndex.h>
#include <rwe/io/campaigntdf/CampaignTdf.h>
#include <rwe/observable/BehaviorSubject.h>
#include <rwe/render/Sprite.h>
#include <rwe/sim/Energy.h>
#include <rwe/sim/Metal.h>
#include <string>
#include <variant>

namespace rwe
{
    class MainMenuModel
    {
    public:
        struct SelectedMapInfo
        {
            std::string name;
            std::string description;
            std::string size;

            std::shared_ptr<Sprite> minimap;

            SelectedMapInfo(
                const std::string& name,
                const std::string& description,
                const std::string& size,
                const std::shared_ptr<Sprite>& minimap);

            bool operator==(const SelectedMapInfo& rhs) const;

            bool operator!=(const SelectedMapInfo& rhs) const;
        };

        struct PlayerSettings
        {
            enum class Type
            {
                Open,
                Human,
                Computer
            };
            enum class Side
            {
                Arm,
                Core
            };

            BehaviorSubject<Type> type;
            BehaviorSubject<Side> side;
            BehaviorSubject<PlayerColorIndex> colorIndex;
            BehaviorSubject<std::optional<int>> teamIndex;
            BehaviorSubject<Metal> metal;
            BehaviorSubject<Energy> energy;
        };

    public:
        /**
         * Front-end state for the New Campaign flow.
         * The catalog is loaded lazily from stock campaign
         * descriptors; the descriptor files are the source of truth
         * for sides, mission order and mission identifiers.
         */
        struct CampaignMenuState
        {
            /** Every campaign descriptor found under the camps dir. */
            std::vector<CampaignDescriptorFile> campaigns;

            /** Indices into campaigns currently listed, filtered by side. */
            std::vector<unsigned int> displayedCampaigns;

            /** Selected index into displayedCampaigns. */
            std::optional<unsigned int> selectedIndex;

            /** Selected index into the selected campaign's mission list. */
            std::optional<unsigned int> selectedMissionIndex;

            /** Faction the campaign list is filtered to. */
            std::string side{"ARM"};

            /** Schema Type to launch, e.g. "Medium". */
            std::string difficulty{"Medium"};
        };

        Subject<int> teamChanges;

        CampaignMenuState campaign;

        /**
         * Front-end state for the SKIRMISH.GUI toggles. Mirrors the
         * five retail staged buttons; StartLocation/CommanderDeath/
         * Mapping/LineOfSight/Difficulty all stage in retail order.
         */
        SkirmishSettings skirmish;

        /**
         * Resource defaults resolved from the committed map's selected
         * Network schema (HumanMetal/Energy, ComputerMetal/Energy).
         * Occupied slots are reseeded when a map is committed and
         * when a slot is activated afterwards; explicit user edits
         * then act as the per-slot overrides retail persists as
         * Player%dMetal/Energy.
         */
        struct SkirmishResourceDefaults
        {
            bool valid{false};
            Metal humanMetal{1000};
            Energy humanEnergy{1000};
            Metal computerMetal{1000};
            Energy computerEnergy{1000};
        };
        SkirmishResourceDefaults skirmishResourceDefaults;

        BehaviorSubject<std::optional<SelectedMapInfo>> selectedMap;

        BehaviorSubject<std::optional<SelectedMapInfo>> candidateSelectedMap;

        std::array<PlayerSettings, 10> players{{
            {PlayerSettings::Type::Human, PlayerSettings::Side::Arm, PlayerColorIndex(0), std::optional<int>(std::nullopt), Metal(1000), Energy(1000)},
            {PlayerSettings::Type::Computer, PlayerSettings::Side::Core, PlayerColorIndex(1), std::optional<int>(std::nullopt), Metal(1000), Energy(1000)},
            {PlayerSettings::Type::Open, PlayerSettings::Side::Arm, PlayerColorIndex(2), std::optional<int>(std::nullopt), Metal(1000), Energy(1000)},
            {PlayerSettings::Type::Open, PlayerSettings::Side::Core, PlayerColorIndex(3), std::optional<int>(std::nullopt), Metal(1000), Energy(1000)},
            {PlayerSettings::Type::Open, PlayerSettings::Side::Arm, PlayerColorIndex(4), std::optional<int>(std::nullopt), Metal(1000), Energy(1000)},
            {PlayerSettings::Type::Open, PlayerSettings::Side::Core, PlayerColorIndex(5), std::optional<int>(std::nullopt), Metal(1000), Energy(1000)},
            {PlayerSettings::Type::Open, PlayerSettings::Side::Arm, PlayerColorIndex(6), std::optional<int>(std::nullopt), Metal(1000), Energy(1000)},
            {PlayerSettings::Type::Open, PlayerSettings::Side::Core, PlayerColorIndex(7), std::optional<int>(std::nullopt), Metal(1000), Energy(1000)},
            {PlayerSettings::Type::Open, PlayerSettings::Side::Arm, PlayerColorIndex(8), std::optional<int>(std::nullopt), Metal(1000), Energy(1000)},
            {PlayerSettings::Type::Open, PlayerSettings::Side::Core, PlayerColorIndex(9), std::optional<int>(std::nullopt), Metal(1000), Energy(1000)},
        }};

        /**
         * Returns true when the team contains two or more players.
         */
        bool isTeamShared(int index) const;

        /**
         * Returns true if there is a player using the given color index.
         */
        bool isColorInUse(const PlayerColorIndex& colorIndex) const;

        /**
         * Returns the first available player color index.
         */
        std::optional<PlayerColorIndex> getFirstFreeColor() const;
    };
}
