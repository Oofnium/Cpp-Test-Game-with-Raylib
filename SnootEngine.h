// SnootEngine.h - the data model for the visual novel engine.
// No raylib in here on purpose: story data and stat logic never touch the screen.
#pragma once
#include <set>
#include <string>
#include <vector>

// ================= stats and flags =================

enum class Stat { AnonMaturity, FangStability };

// Everything the story remembers. Stats CAN go negative (bad choices push them down).
struct StoryState {
    int anonMaturity = 0;     // Anon's growth: accountability, knowing when NOT to push
    int fangStability = 0;    // Fang's emotional stability, self-worth and trust
    std::set<std::string> flags;
};

// ================= conditions and effects =================

enum class Compare { GreaterEq, Greater, LessEq, Less, Equal };

// "[REQ: maturity>=3]", "[REQ: flag:told_truth]" or "[REQ: !flag:pushed_too_hard]"
struct Requirement {
    enum class Kind { None, StatCompare, FlagSet, FlagNotSet };
    Kind kind = Kind::None;       // None = no requirement, always available
    Stat stat = Stat::AnonMaturity;
    Compare compare = Compare::GreaterEq;
    int value = 0;
    std::string flag;
};

// "+1 MATURITY" or "-2 STABILITY"
struct StatEffect {
    Stat stat = Stat::AnonMaturity;
    int amount = 0;
};

// ================= script data =================

// ? [REQ: ...] Choice text | +1 STAT | FLAG: name | JUMP: scene_id
struct Choice {
    std::string text;
    Requirement requirement;               // kind None = always shown
    std::vector<StatEffect> effects;
    std::vector<std::string> flagsToSet;
    std::string jumpTo;                    // empty = just continue with the next step
};

enum class SpritePos { Left, Center, Right };
enum class StepType { Background, Show, Music, Dialogue, Choices, Ending };

// One thing that happens in a scene, in order.
struct Step {
    StepType type = StepType::Dialogue;
    std::string key;                       // Background / Show / Music: the asset key
    SpritePos pos = SpritePos::Center;     // Show: where the sprite stands
    std::string speaker;                   // Dialogue
    std::string text;                      // Dialogue
    std::vector<Choice> choices;           // Choices
    // Ending (proposed tag [ENDING]): no data, it just asks DecideEnding() for the result
};

struct Scene {
    std::string id;
    std::vector<Step> steps;
};

// ================= narrative logic (Narrative.cpp) =================

int  GetStat(const StoryState& state, Stat stat);
void AddStat(StoryState& state, Stat stat, int amount);
void SetFlag(StoryState& state, const std::string& flag);
bool HasFlag(const StoryState& state, const std::string& flag);

// text parsing helpers for the script parser. They return false and fill 'error' on bad input.
bool ParseStatName(const std::string& text, Stat& out);                              // "maturity", "STABILITY"...
bool ParseRequirement(const std::string& text, Requirement& out, std::string& error);
bool ParseEffect(const std::string& text, StatEffect& out, std::string& error);      // "+1 MATURITY"

bool RequirementMet(const StoryState& state, const Requirement& req);
void ApplyChoice(StoryState& state, const Choice& choice);     // applies stats and sets flags

// Which of the 4 endings (1 to 4) does this state earn?
struct EndingRules {
    int trueMaturity = 4;      // Ending 4 needs BOTH of these...
    int trueStability = 4;
    int midMaturity = 3;       // Ending 3: Anon grew, Fang's stability lagged
    int midStability = 3;      // Ending 2: Fang stabilised, Anon didn't grow enough
};
int DecideEnding(const StoryState& state, const EndingRules& rules = EndingRules());
