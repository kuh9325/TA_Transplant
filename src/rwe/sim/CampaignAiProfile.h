#pragma once

#include <string>
#include <vector>

namespace rwe
{
    /**
     * A retail `Weight <name> <mult>` directive. `name` resolves to a
     * unit name or a Category token (see UnitDefinition::categories).
     * The multiplier scales the matched units' byte weight
     * (default 100, clamped 0..100) once each -- the first directive
     * to touch a unit wins (TotalA.exe 0x409dc0).
     */
    struct CampaignAiWeightDirective
    {
        std::string selector;
        float multiplier;
    };

    /**
     * A retail `Limit <name> [<cap>]` directive. Missing `<cap>`
     * means 0 (prohibited); -1 means unlimited. The first directive
     * to touch a unit wins (TotalA.exe 0x409e90).
     */
    struct CampaignAiLimitDirective
    {
        std::string selector;
        int value;
    };

    /**
     * The difficulty-filtered directive set of a stock `ai/<name>.TXT`
     * profile, in file order. Directives outside a matching `plan`
     * section are dropped at parse time.
     */
    struct CampaignAiProfile
    {
        std::vector<CampaignAiWeightDirective> weightDirectives;
        std::vector<CampaignAiLimitDirective> limitDirectives;
    };

    /**
     * Retail campaign difficulty index (TotalA.exe ctx+0x37eee):
     * 0 = easy, 1 = medium, 2 = hard. `plan` sections gate on these.
     */
    enum class CampaignAiDifficulty
    {
        Easy = 0,
        Medium = 1,
        Hard = 2,
    };

    /**
     * Parse a stock ai/*.TXT profile for the given difficulty.
     *
     * Grammar (TotalA.exe 0x4b7a30 + handlers 0x406c90/0x406db0/0x406e40):
     *   - `//` comments and blank lines are skipped
     *   - `plan <tok>...` starts a section: it is active iff any token
     *     equals `any` or the current difficulty name (easy/medium/hard);
     *     matching is case-insensitive. A `plan` line with no tokens
     *     deactivates the section.
     *   - `weight <name> <float>` records a weight directive, only while
     *     the last `plan` matched.
     *   - `limit <name> [<int>]` records a limit directive, only while
     *     the last `plan` matched. A missing integer defaults to 0.
     *   - Unknown first tokens and malformed lines are tolerated.
     *   - Keywords, unit names and plan tokens are case-insensitive.
     */
    CampaignAiProfile parseCampaignAiProfile(const std::string& text, CampaignAiDifficulty difficulty);

    /**
     * Parse a single `weight`/`limit` directive line (the form used by
     * the FBI `ai_limit` self-directive on units such as CORDECOM).
     * Applies the directive into `profile` as if it appeared in an
     * active plan section. Unknown/malformed lines are ignored.
     */
    void parseCampaignAiSelfDirective(const std::string& line, CampaignAiProfile& profile);
}
