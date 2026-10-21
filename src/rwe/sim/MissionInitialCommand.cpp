#include "MissionInitialCommand.h"

#include <cctype>

namespace rwe
{
    namespace
    {
        bool isWhitespace(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        std::string_view trim(std::string_view s)
        {
            while (!s.empty() && isWhitespace(s.front()))
            {
                s.remove_prefix(1);
            }
            while (!s.empty() && isWhitespace(s.back()))
            {
                s.remove_suffix(1);
            }
            return s;
        }

        /**
         * Parses an integer at the front of `s`, optionally signed.
         * Deterministic and locale-independent. On success, advances `s`
         * past the parsed digits and returns true.
         */
        bool parseInt(std::string_view& s, int& out)
        {
            auto rest = trim(s);
            if (rest.empty())
            {
                return false;
            }

            bool negative = false;
            if (rest.front() == '-' || rest.front() == '+')
            {
                negative = rest.front() == '-';
                rest.remove_prefix(1);
            }

            if (rest.empty() || !std::isdigit(static_cast<unsigned char>(rest.front())))
            {
                return false;
            }

            int value = 0;
            while (!rest.empty() && std::isdigit(static_cast<unsigned char>(rest.front())))
            {
                value = value * 10 + (rest.front() - '0');
                rest.remove_prefix(1);
            }

            out = negative ? -value : value;
            s = rest;
            return true;
        }

        /**
         * Parses a floating-point number like retail's `%f`: optional sign,
         * digits with an optional single decimal point. On success,
         * advances `s` past the number and returns true.
         */
        bool parseFloat(std::string_view& s, float& out)
        {
            auto rest = trim(s);
            if (rest.empty())
            {
                return false;
            }

            bool negative = false;
            if (rest.front() == '-' || rest.front() == '+')
            {
                negative = rest.front() == '-';
                rest.remove_prefix(1);
            }

            float value = 0.0f;
            bool digits = false;
            while (!rest.empty() && std::isdigit(static_cast<unsigned char>(rest.front())))
            {
                digits = true;
                value = value * 10.0f + static_cast<float>(rest.front() - '0');
                rest.remove_prefix(1);
            }
            if (!rest.empty() && rest.front() == '.')
            {
                rest.remove_prefix(1);
                float scale = 0.1f;
                while (!rest.empty() && std::isdigit(static_cast<unsigned char>(rest.front())))
                {
                    digits = true;
                    value += scale * static_cast<float>(rest.front() - '0');
                    scale *= 0.1f;
                    rest.remove_prefix(1);
                }
            }
            if (!digits)
            {
                return false;
            }

            out = negative ? -value : value;
            s = rest;
            return true;
        }

        /** Seconds to simulation ticks, truncating like retail's ftol. */
        unsigned int secondsToTicks(float seconds)
        {
            return seconds <= 0.0f ? 0u : static_cast<unsigned int>(seconds * 30.0f);
        }

        /**
         * Parses an identifier token (mission-object Ident or unit type name):
         * a run of non-whitespace, non-comma characters.
         */
        bool parseName(std::string_view& s, std::string& out)
        {
            auto rest = trim(s);
            auto end = rest.find_first_of(" \t\r\n,");
            auto token = rest.substr(0, end);
            if (token.empty())
            {
                return false;
            }
            out = std::string(token);
            s = rest.substr(token.size());
            return true;
        }

        SimVector coordsToVector(int x, int z)
        {
            return SimVector(SimScalar(x), SimScalar(0), SimScalar(z));
        }

        UnitMoveOrders toMoveOrder(int v)
        {
            switch (v)
            {
                case 0:
                    return UnitMoveOrders::HoldPosition;
                case 1:
                    return UnitMoveOrders::Maneuver;
                default:
                    return UnitMoveOrders::Roam;
            }
        }

        UnitFireOrders toFireOrder(int v)
        {
            switch (v)
            {
                case 0:
                    return UnitFireOrders::HoldFire;
                case 1:
                    return UnitFireOrders::ReturnFire;
                default:
                    return UnitFireOrders::FireAtWill;
            }
        }

        void parseSegment(std::string_view segment, ParsedInitialMission& result)
        {
            auto rest = trim(segment);
            if (rest.empty())
            {
                return;
            }

            const auto verb = static_cast<char>(std::tolower(static_cast<unsigned char>(rest.front())));
            rest.remove_prefix(1);

            switch (verb)
            {
                case 'p':
                {
                    int x = 0;
                    int z = 0;
                    float dwell = 0.0f;
                    auto args = rest;
                    // retail's `p` handler suppresses the implicit
                    // trailing make-selectable for every form
                    result.suppressImplicitSelectable = true;
                    if (parseInt(args, x) && parseInt(args, z))
                    {
                        MissionCommandPatrol cmd;
                        cmd.destination = coordsToVector(x, z);
                        // retail reads a third float: a dwell in seconds.
                        // Further arguments are swallowed by its sscanf.
                        if (parseFloat(args, dwell))
                        {
                            cmd.dwellTicks = secondsToTicks(dwell);
                        }
                        result.commands.push_back(cmd);
                    }
                    else
                    {
                        // bare `p` or unparseable args: patrol in place
                        result.commands.push_back(MissionCommandPatrol{std::nullopt, std::nullopt});
                    }
                    return;
                }
                case 'm':
                {
                    int x = 0;
                    int z = 0;
                    if (!parseInt(rest, x) || !parseInt(rest, z))
                    {
                        result.errors.push_back("malformed m command: " + std::string(segment));
                        return;
                    }
                    result.commands.push_back(MissionCommandMove{coordsToVector(x, z)});
                    return;
                }
                case 'a':
                {
                    auto args = trim(rest);
                    if (!args.empty() && (std::isdigit(static_cast<unsigned char>(args.front())) || args.front() == '-' || args.front() == '+'))
                    {
                        int x = 0;
                        int z = 0;
                        if (!parseInt(rest, x) || !parseInt(rest, z))
                        {
                            result.errors.push_back("malformed a command: " + std::string(segment));
                            return;
                        }
                        // retail's `a x z` path suppresses the implicit
                        // trailing make-selectable; `a <type>` does not
                        result.suppressImplicitSelectable = true;
                        result.commands.push_back(MissionCommandAttackPosition{coordsToVector(x, z)});
                    }
                    else
                    {
                        // `a <unittype>` is ATTACKUTYPE
                        std::string name;
                        if (!parseName(rest, name))
                        {
                            result.errors.push_back("malformed a command: " + std::string(segment));
                            return;
                        }
                        result.commands.push_back(MissionCommandAttackType{name});
                    }
                    return;
                }
                case 'b':
                {
                    auto args = trim(rest);
                    if (!args.empty() && (args.front() == 'w' || args.front() == 'W'))
                    {
                        // `bw <n>`: background weapon-stockpile directive
                        auto argsView = trim(rest);
                        argsView.remove_prefix(1); // consume the 'w'
                        int count = 0;
                        parseInt(argsView, count);
                        MissionCommandBuildWeapon cmd;
                        cmd.count = count;
                        result.commands.push_back(cmd);
                        return;
                    }
                    std::string type;
                    if (!parseName(rest, type))
                    {
                        result.errors.push_back("malformed b command: " + std::string(segment));
                        return;
                    }
                    MissionCommandBuild cmd;
                    cmd.unitType = type;
                    cmd.count = 1;
                    int count = 0;
                    if (parseInt(rest, count))
                    {
                        cmd.count = count;
                    }
                    // retail's sscanf fills the %d slot then two %f
                    // slots; a short argument list leaves the tail
                    // slots untouched
                    float x = 0.0f;
                    float z = 0.0f;
                    if (parseFloat(rest, x))
                    {
                        cmd.coordX = x;
                        if (parseFloat(rest, z))
                        {
                            cmd.coordZ = z;
                        }
                    }
                    result.commands.push_back(cmd);
                    return;
                }
                case 'g':
                {
                    std::string name;
                    if (!parseName(rest, name))
                    {
                        result.errors.push_back("malformed g command: " + std::string(segment));
                        return;
                    }
                    result.commands.push_back(MissionCommandGuard{name});
                    return;
                }
                case 'i':
                {
                    std::string name;
                    if (!parseName(rest, name))
                    {
                        result.errors.push_back("malformed i command: " + std::string(segment));
                        return;
                    }
                    result.commands.push_back(MissionCommandJoinGroup{name});
                    return;
                }
                case 'o':
                {
                    // retail `o <fire> <move>`; arguments that are absent
                    // keep the unit's current standing order
                    MissionCommandSetStance cmd;
                    int fire = 0;
                    if (parseInt(rest, fire))
                    {
                        cmd.fireOrder = toFireOrder(fire);
                    }
                    int move = 0;
                    if (parseInt(rest, move))
                    {
                        cmd.moveOrder = toMoveOrder(move);
                    }
                    if (!cmd.fireOrder && !cmd.moveOrder)
                    {
                        result.errors.push_back("malformed o command: " + std::string(segment));
                        return;
                    }
                    result.commands.push_back(cmd);
                    return;
                }
                case 'u':
                {
                    MissionCommandUnload cmd;
                    int x = 0;
                    int z = 0;
                    auto args = rest;
                    if (parseInt(args, x) && parseInt(args, z))
                    {
                        cmd.destination = coordsToVector(x, z);
                    }
                    result.commands.push_back(cmd);
                    return;
                }
                case 'w':
                {
                    auto args = trim(rest);
                    if (!args.empty() && (args.front() == 'a' || args.front() == 'A'))
                    {
                        // WaitForAttack; optional ident argument
                        args.remove_prefix(1);
                        std::string name;
                        if (parseName(args, name))
                        {
                            result.commands.push_back(MissionCommandWaitForAttack{name});
                        }
                        else
                        {
                            result.commands.push_back(MissionCommandWaitForAttack{std::nullopt});
                        }
                        return;
                    }

                    float seconds = 0.0f;
                    if (!parseFloat(rest, seconds) || seconds < 0.0f)
                    {
                        result.errors.push_back("malformed w command: " + std::string(segment));
                        return;
                    }
                    // retail converts seconds to ticks at 30 ticks/second;
                    // a trailing second integer argument is tolerated
                    result.commands.push_back(MissionCommandWait{secondsToTicks(seconds)});
                    return;
                }
                case 's':
                {
                    result.commands.push_back(MissionCommandMakeSelectable{});
                    result.hasExplicitSelectable = true;
                    return;
                }
                case 'd':
                {
                    // retail queues SELFDESTRUCTFG and suppresses the
                    // implicit trailing make-selectable
                    result.commands.push_back(MissionCommandSelfDestruct{});
                    result.suppressImplicitSelectable = true;
                    return;
                }
                default:
                {
                    result.unsupportedCommands.push_back(std::string(trim(segment)));
                    result.commands.push_back(MissionCommandUnsupported{std::string(trim(segment))});
                    return;
                }
            }
        }
    }

    ParsedInitialMission parseInitialMission(const std::string& initialMission)
    {
        ParsedInitialMission result;

        std::string_view remaining(initialMission);
        while (!remaining.empty())
        {
            auto comma = remaining.find(',');
            auto segment = comma == std::string_view::npos ? remaining : remaining.substr(0, comma);
            parseSegment(segment, result);

            if (comma == std::string_view::npos)
            {
                break;
            }
            remaining.remove_prefix(comma + 1);
        }

        return result;
    }
}
