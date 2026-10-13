// Narrative.cpp - stats, flags, conditions and endings. No raylib, no file reading.
#include "SnootEngine.h"
#include <sstream>

// ---------- small text helpers (only used in this file) ----------
static std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r");
    return s.substr(start, end - start + 1);
}

static std::string Lower(std::string s) {
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);
    }
    return s;
}

static bool ReadInt(const std::string& s, int& out) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (size_t k = i; k < s.size(); k++) {
        if (s[k] < '0' || s[k] > '9') return false;
    }
    out = std::stoi(s[0] == '+' ? s.substr(1) : s);
    return true;
}

// ---------- stats and flags ----------
int GetStat(const StoryState& state, Stat stat) {
    return (stat == Stat::AnonMaturity) ? state.anonMaturity : state.fangStability;
}

void AddStat(StoryState& state, Stat stat, int amount) {
    if (stat == Stat::AnonMaturity) state.anonMaturity += amount;
    else                            state.fangStability += amount;
}

void SetFlag(StoryState& state, const std::string& flag) { state.flags.insert(flag); }
bool HasFlag(const StoryState& state, const std::string& flag) { return state.flags.count(flag) > 0; }

// ---------- parsing ----------
bool ParseStatName(const std::string& text, Stat& out) {
    std::string name = Lower(Trim(text));
    if (name == "maturity" || name == "anonmaturity") { out = Stat::AnonMaturity; return true; }
    if (name == "stability" || name == "fangstability") { out = Stat::FangStability; return true; }
    return false;
}

bool ParseRequirement(const std::string& text, Requirement& out, std::string& error) {
    out = Requirement();
    std::string t = Trim(text);
    if (t.empty()) { error = "empty requirement"; return false; }

    // flag:name  or  !flag:name
    bool negate = false;
    if (t[0] == '!') {
        negate = true;
        t = Trim(t.substr(1));
    }
    if (Lower(t).rfind("flag:", 0) == 0) {
        std::string name = Trim(t.substr(5));
        if (name.empty() || name.find(' ') != std::string::npos) {
            error = "expected flag:name (the name can't be empty or contain spaces)";
            return false;
        }
        out.kind = negate ? Requirement::Kind::FlagNotSet : Requirement::Kind::FlagSet;
        out.flag = name;
        return true;
    }
    if (negate) { error = "'!' only works in front of flag:name"; return false; }

    // stat comparison, like  maturity>=3
    size_t pos = t.find_first_of("<>=");
    if (pos == std::string::npos) {
        error = "expected something like maturity>=3 or flag:name";
        return false;
    }
    std::string op = (pos + 1 < t.size() && t[pos + 1] == '=') ? t.substr(pos, 2) : t.substr(pos, 1);

    if      (op == ">=") out.compare = Compare::GreaterEq;
    else if (op == ">")  out.compare = Compare::Greater;
    else if (op == "<=") out.compare = Compare::LessEq;
    else if (op == "<")  out.compare = Compare::Less;
    else if (op == "==") out.compare = Compare::Equal;
    else { error = "unknown comparison '" + op + "' (use >=, >, <=, <, ==)"; return false; }

    if (!ParseStatName(t.substr(0, pos), out.stat)) {
        error = "unknown stat '" + Trim(t.substr(0, pos)) + "' (use maturity or stability)";
        return false;
    }
    if (!ReadInt(Trim(t.substr(pos + op.size())), out.value)) {
        error = "the number after the comparison is missing or not a whole number";
        return false;
    }
    out.kind = Requirement::Kind::StatCompare;
    return true;
}

bool ParseEffect(const std::string& text, StatEffect& out, std::string& error) {
    out = StatEffect();
    std::istringstream words(Trim(text));
    std::string amountText, statText, extra;
    words >> amountText >> statText >> extra;

    if (statText.empty() || !extra.empty()) { error = "expected something like +1 MATURITY"; return false; }
    if (!ReadInt(amountText, out.amount))   { error = "'" + amountText + "' is not a whole number"; return false; }
    if (!ParseStatName(statText, out.stat)) { error = "unknown stat '" + statText + "' (use MATURITY or STABILITY)"; return false; }
    return true;
}

// ---------- evaluating ----------
bool RequirementMet(const StoryState& state, const Requirement& req) {
    switch (req.kind) {
        case Requirement::Kind::None:       return true;
        case Requirement::Kind::FlagSet:    return HasFlag(state, req.flag);
        case Requirement::Kind::FlagNotSet: return !HasFlag(state, req.flag);
        case Requirement::Kind::StatCompare: {
            int have = GetStat(state, req.stat);
            switch (req.compare) {
                case Compare::GreaterEq: return have >= req.value;
                case Compare::Greater:   return have >  req.value;
                case Compare::LessEq:    return have <= req.value;
                case Compare::Less:      return have <  req.value;
                case Compare::Equal:     return have == req.value;
            }
        }
    }
    return false;
}

void ApplyChoice(StoryState& state, const Choice& choice) {
    for (size_t i = 0; i < choice.effects.size(); i++) {
        AddStat(state, choice.effects[i].stat, choice.effects[i].amount);
    }
    for (size_t i = 0; i < choice.flagsToSet.size(); i++) {
        SetFlag(state, choice.flagsToSet[i]);
    }
}

// Thresholds follow the community wiki for Snoot Game; the edge cases are my own guess.
// "blocked_true_ending" is a hook: a story choice can set it to lock Ending 4 out for good.
int DecideEnding(const StoryState& state, const EndingRules& rules) {
    bool matureEnough = state.anonMaturity >= rules.trueMaturity;
    bool stableEnough = state.fangStability >= rules.trueStability;

    if (matureEnough && stableEnough && !HasFlag(state, "blocked_true_ending")) return 4;   // true ending
    if (state.anonMaturity >= rules.midMaturity)   return 3;   // Anon grew, Fang didn't get there
    if (state.fangStability >= rules.midStability) return 2;   // Fang stabilised, Anon didn't grow
    return 1;                                                  // both low: the worst outcome
}
