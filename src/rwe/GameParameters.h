#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <rwe/SkirmishSettings.h>
#include <rwe/game/PlayerColorIndex.h>
#include <rwe/sim/Energy.h>
#include <rwe/sim/Metal.h>

namespace rwe
{
    struct PlayerControllerTypeHuman
    {
    };
    struct PlayerControllerTypeComputer
    {
    };
    struct PlayerControllerTypeNetwork
    {
        std::string host;
        std::string port;
    };

    using PlayerControllerType = std::variant<PlayerControllerTypeHuman, PlayerControllerTypeComputer, PlayerControllerTypeNetwork>;

    class IsHumanVisitor
    {
    public:
        bool operator()(const PlayerControllerTypeHuman&) const { return true; }
        bool operator()(const PlayerControllerTypeComputer&) const { return false; }
        bool operator()(const PlayerControllerTypeNetwork&) const { return false; }
    };

    class IsComputerVisitor
    {
    public:
        bool operator()(const PlayerControllerTypeHuman&) const { return false; }
        bool operator()(const PlayerControllerTypeComputer&) const { return true; }
        bool operator()(const PlayerControllerTypeNetwork&) const { return false; }
    };

    class GetNetworkAddressVisitor
    {
    public:
        std::optional<std::pair<std::reference_wrapper<const std::string>, std::reference_wrapper<const std::string>>> operator()(const PlayerControllerTypeHuman&) const { return std::nullopt; }
        std::optional<std::pair<std::reference_wrapper<const std::string>, std::reference_wrapper<const std::string>>> operator()(const PlayerControllerTypeComputer&) const { return std::nullopt; }
        std::optional<std::pair<std::reference_wrapper<const std::string>, std::reference_wrapper<const std::string>>> operator()(const PlayerControllerTypeNetwork& p) const { return std::make_pair(std::cref(p.host), std::cref(p.port)); }
    };

    struct PlayerInfo
    {
        std::optional<std::string> name;
        PlayerControllerType controller;
        std::string side;
        PlayerColorIndex color;

        Metal metal;
        Energy energy;

        /**
         * Retail ally group (skirmish Player%dAllyGroup), 0-4.
         * Players sharing a group are mutual allies; an absent value
         * means the player belongs to no alliance (retail sentinel 5).
         */
        std::optional<unsigned int> allyGroup;
    };

    /**
     * Launch descriptor for a campaign mission, distinct from the
     * skirmish player model. Presence on GameParameters switches the
     * load path to mission semantics: players and starting units come
     * from the mission schema, no skirmish commanders are spawned,
     * and skirmish victory conditions are suppressed.
     */
    struct CampaignMissionParameters
    {
        /** Display name of the campaign, e.g. "Arm Campaign". */
        std::string campaignName;

        /** Campaign faction the human is playing, e.g. "ARM". */
        std::string campaignSide;

        /** 0-based index into the campaign descriptor's mission list. */
        unsigned int missionIndex;

        /** The descriptor's missionfile entry, e.g. "AC01.ota". */
        std::string missionFile;

        /** Display title of the mission, e.g. "1: A Hero Returns". */
        std::string missionName;

        /** Schema Type to load, e.g. "Medium". Resolved to an index at load time. */
        std::string difficulty;
    };

    struct GameParameters
    {
        std::string mapName;
        unsigned int schemaIndex;
        std::array<std::optional<PlayerInfo>, 10> players;
        std::string localNetworkPort{"1337"};
        std::optional<std::string> stateLogFile;

        /** Present iff this game is a campaign mission, not a skirmish. */
        std::optional<CampaignMissionParameters> campaign;

        /**
         * Retail skirmish frontend toggles (SKIRMISH.GUI). Only
         * meaningful when `campaign` is absent; campaign missions take
         * their settings from the OTA and mission descriptor.
         */
        SkirmishSettings skirmish;

        GameParameters(const std::string& mapName, unsigned int schemaIndex);
    };
}
