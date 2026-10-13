#include "raylib.h"
#include "raymath.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

const int TILE = 32;  // each tile is 32x32 pixels

enum GameState { MENU, ROUTES, CREDITS, EXPLORING, DIALOGUE, ENDING, PAUSED, LOADSCREEN };

// ================= settings you will edit =================
const int ENDING_COUNT = 4;        // the 4 original endings (all about Anon and Fang)
const int ROUTE_COUNT = 2;         // extra routes, unlocked after all 4 endings
const bool SHOW_POINTS = true;     // set to false for the real release
const float TYPE_SPEED = 45.0f;    // how many characters per second the dialogue types out
const float TRIGGER_RANGE = 40.0f; // how close you must get for an AUTO scene to start by itself
const char* SAVE_FILE = "save.txt";            // endings you have seen (survives between runs)
const char* SETTINGS_FILE = "settings.txt";    // options that survive between runs (for now: debug scores on/off)
const char* RUN_SAVE_FILE = "savegame.txt";    // the run in progress: where you are, flags, scores, finished scenes
const char* STORY_FILE = "story.txt";   // the script: who says what, and where
const char* WORLD_FILE = "world.txt";   // the maps and the doors between them
const char* CROWD_FILE = "crowd.txt";   // background characters who only say boring things (optional)
const int SLOT_COUNT = 3;               // manual save slots: save_slot1.txt ... save_slot3.txt
std::string SlotFile(int slot) { return "save_slot" + std::to_string(slot) + ".txt"; }

const char* endingTitles[ENDING_COUNT] = { "Ending 1 - Shooter", "Ending 2 - Breakup", "Ending 3 - Good (bittersweet)", "Ending 4 - Gold" };

const char* routeNames[ROUTE_COUNT]    = { "Naomi's Route", "Stella's Route" };
const char* routePartners[ROUTE_COUNT] = { "Naomi", "Stella" };
const char* routeBlurbs[ROUTE_COUNT]   = {
    "What if Naser and Naomi broke up a week before Anon arrived?",
    "Not written yet."
};

const int MENU_COUNT = 6;
const char* menuItems[MENU_COUNT] = { "Continue", "Load Game", "New Game", "Extra Routes", "Credits", "Quit" };

// ================= save file =================
int CountTrue(const bool flags[], int count) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        if (flags[i]) total++;
    }
    return total;
}

// writes something like "1 0 1 0 0 0": 4 endings, then 2 routes (1 = done)
void SaveProgress(const bool endings[], const bool routes[]) {
    std::ofstream out(SAVE_FILE);
    for (int i = 0; i < ENDING_COUNT; i++) out << (endings[i] ? 1 : 0) << " ";
    for (int i = 0; i < ROUTE_COUNT; i++) out << (routes[i] ? 1 : 0) << " ";
}

// reads it back (does nothing if there is no file yet)
void LoadProgress(bool endings[], bool routes[]) {
    std::ifstream in(SAVE_FILE);
    if (!in) return;
    for (int i = 0; i < ENDING_COUNT; i++) {
        int value = 0;
        in >> value;
        endings[i] = (value == 1);
    }
    for (int i = 0; i < ROUTE_COUNT; i++) {
        int value = 0;
        in >> value;
        routes[i] = (value == 1);
    }
}

// ================= the ending rule =================
// This is Snoot Game's own get_ending() from storyline.rpy: two scores (Anon and Fang, 0-5 each) plus one flag
// (the wing story, set in chapter 5). The original's letters: A = Shooter, B = Breakup, C = Good, D = Gold.
//
// One fix: in the original, anon 4+ with fang 5 but WITHOUT the wing story fell through every rule and landed
// on Shooter. Here that case becomes the Good ending instead (you were close to Gold, you didn't hit the best one).
int DecideEnding(int anon, int fang, bool wingStory) {
    if (anon >= 4 && fang >= 4 && wingStory)  return 3;   // Ending 4: Gold
    if (anon >= 4 || (anon >= 3 && fang <= 4)) return 2;   // Ending 3: Good (the original calls it "tradwife")
    if (anon <= 3 && fang >= 3)               return 1;   // Ending 2: Breakup (the original calls it "doomer")
    return 0;                                              // Ending 1: Shooter
}

// ================= data types =================

// A condition for an answer or a scene:  flag:name   !flag:name   anon>=2   partner<1
struct Condition {
    enum Kind { NONE, FLAG_SET, FLAG_NOT_SET, STAT_COMPARE };
    Kind kind = NONE;            // NONE = no condition, always true
    std::string flag;            // FLAG_SET / FLAG_NOT_SET
    bool useAnon = true;         // STAT_COMPARE: Anon's score (true) or the partner's (false)
    std::string op;              // STAT_COMPARE: ">=", ">", "<=", "<" or "=="
    int value = 0;
    std::vector<Condition> also; // more conditions joined with &: ALL of them must be true too
};

// One answer the player can pick.
struct Choice {
    std::string text;                      // what the player sees
    int anonPoints = 0;                    // grows Anon
    int partnerPoints = 0;                 // grows the partner (Fang, or the route's girl)
    std::string reply;                     // what the NPC says back
    std::vector<std::string> flagsToSet;   // flags this answer switches on
    Condition requirement;                 // the answer is locked until this is true
};

// One line of dialogue. If 'choices' is empty it's a normal line.
struct Line {
    std::string speaker;
    std::string text;
    std::vector<Choice> choices;
    std::string expression;                // portrait expression, like "annoyed" (can be empty)
    Condition requirement;                 // the line is skipped unless this is true (IF ... ENDIF blocks)
};

// One story scene: a character waits at a spot on a map, during a certain chapter
struct Scene {
    int route = -1;          // -1 = original story, 0.. = extra route
    int chapter = 1;
    std::string npc;
    int map = 0;             // which map they stand in (index into world.maps)
    int tileX = 0;           // ...and on which tile
    int tileY = 0;
    Condition requirement;   // the scene only appears while this is true
    bool autoStart = false;  // true = starts by itself when you walk up to the spot (no E needed)
    std::vector<std::string> flagsOnFinish;   // flags switched on when the scene ends (FLAG: name inside the scene)
    std::vector<Line> lines;
    bool done = false;
};

// A character in the cast (their look). Where they stand comes from the scenes.
struct Character {
    std::string name;
    Color color;
};

// A door: stepping on tile (x, y) takes you to tile (toX, toY) of another map
struct Warp {
    int x = 0;
    int y = 0;
    int toMap = 0;
    int toX = 0;
    int toY = 0;
    int sourceLine = 0;      // line in world.txt, for error messages
};

struct Map {
    std::string name;
    std::vector<std::string> rows;   // one string per row of tiles
    std::vector<Warp> warps;
};

// a background character: stands somewhere, and only says one of a few boring lines when you press E
struct CrowdMember {
    int map = 0;
    int tileX = 0;
    int tileY = 0;
    std::string name;
    std::vector<std::string> lines;
    Color color = { 170, 170, 170, 255 };
    int lastLine = -1;               // so the same line doesn't come up twice in a row
    int sourceLine = 0;              // line in crowd.txt, for error messages
};

struct World {
    std::vector<Map> maps;
    std::vector<CrowdMember> crowd;
    int startMap = 0;
    int startX = 1;
    int startY = 1;
};

// Everything that resets when you start a new run
struct Run {
    int route = -1;
    int chapter = 1;
    int anonPoints = 0;
    int partnerPoints = 0;
    int map = 0;                         // which map you are in
    Vector2 position = { 36, 36 };       // where you are on that map (pixels)
    std::set<std::string> flags;         // story flags switched on so far
    std::vector<Scene> scenes;
};

std::vector<Character> CreateCast() {
    std::vector<Character> cast;
    cast.push_back({ "Anon",   BLUE });
    cast.push_back({ "Fang",   RED });
    cast.push_back({ "Naomi",  ORANGE });
    cast.push_back({ "Naser",  GRAY });
    cast.push_back({ "Stella", GREEN });
    cast.push_back({ "Trish",  PINK });
    cast.push_back({ "Reed",   SKYBLUE });
    cast.push_back({ "Spears", BROWN });
    cast.push_back({ "Tsuki",  DARKGREEN });
    return cast;
}

Color ColorOf(const std::vector<Character>& cast, const std::string& name) {
    for (size_t i = 0; i < cast.size(); i++) {
        if (cast[i].name == name) return cast[i].color;
    }
    return GRAY;
}

bool InCast(const std::vector<Character>& cast, const std::string& name) {
    for (size_t i = 0; i < cast.size(); i++) {
        if (cast[i].name == name) return true;
    }
    return false;
}

// is this condition true right now?
bool ConditionMetSingle(const Run& run, const Condition& c) {
    switch (c.kind) {
        case Condition::NONE:         return true;
        case Condition::FLAG_SET:     return run.flags.count(c.flag) > 0;
        case Condition::FLAG_NOT_SET: return run.flags.count(c.flag) == 0;
        case Condition::STAT_COMPARE: {
            int have = c.useAnon ? run.anonPoints : run.partnerPoints;
            if (c.op == ">=") return have >= c.value;
            if (c.op == ">")  return have >  c.value;
            if (c.op == "<=") return have <= c.value;
            if (c.op == "<")  return have <  c.value;
            return have == c.value;
        }
    }
    return false;
}

// is this condition (and every condition joined to it with &) true right now?
bool ConditionMet(const Run& run, const Condition& c) {
    if (!ConditionMetSingle(run, c)) return false;
    for (size_t i = 0; i < c.also.size(); i++) {
        if (!ConditionMet(run, c.also[i])) return false;
    }
    return true;
}

// ================= tiles =================
bool IsKnownTile(char c) {
    return c == '#' || c == '.' || c == 'g' || c == 'r' || c == 'T' || c == 'x';
}

// walls, trees and furniture block you
bool IsSolidTile(char c) {
    return c == '#' || c == 'T' || c == 'x';
}

Color TileColor(char c) {
    switch (c) {
        case '#': return DARKBROWN;
        case '.': return BEIGE;
        case 'g': return Color{ 130, 175, 95, 255 };    // grass
        case 'r': return Color{ 105, 105, 115, 255 };   // road
        case 'T': return Color{ 40, 100, 50, 255 };     // tree
        case 'x': return Color{ 140, 100, 60, 255 };    // furniture
    }
    return MAGENTA;   // unknown tile (should never happen)
}

bool InsideMap(const Map& map, int tx, int ty) {
    return ty >= 0 && ty < (int)map.rows.size() && tx >= 0 && tx < (int)map.rows[ty].size();
}

// outside the map counts as solid
bool IsSolidAt(const Map& map, int tx, int ty) {
    if (!InsideMap(map, tx, ty)) return true;
    return IsSolidTile(map.rows[ty][tx]);
}

bool IsWarpTile(const Map& map, int tx, int ty) {
    for (size_t i = 0; i < map.warps.size(); i++) {
        if (map.warps[i].x == tx && map.warps[i].y == ty) return true;
    }
    return false;
}

// checks the 4 corners of the player's square
bool Blocked(const Map& map, Vector2 pos, float size) {
    return IsSolidAt(map, (int)(pos.x / TILE),              (int)(pos.y / TILE)) ||
           IsSolidAt(map, (int)((pos.x + size - 1) / TILE), (int)(pos.y / TILE)) ||
           IsSolidAt(map, (int)(pos.x / TILE),              (int)((pos.y + size - 1) / TILE)) ||
           IsSolidAt(map, (int)((pos.x + size - 1) / TILE), (int)((pos.y + size - 1) / TILE));
}

int FindMap(const World& world, const std::string& name) {
    for (size_t i = 0; i < world.maps.size(); i++) {
        if (world.maps[i].name == name) return (int)i;
    }
    return -1;
}

// "school_entrance" -> "School Entrance"
std::string PrettyName(const std::string& id) {
    std::string out = id;
    bool startOfWord = true;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == '_') {
            out[i] = ' ';
            startOfWord = true;
        } else if (startOfWord) {
            if (out[i] >= 'a' && out[i] <= 'z') out[i] = (char)(out[i] - 32);
            startOfWord = false;
        }
    }
    return out;
}

// ================= typewriter helpers =================

// how many characters (not bytes) a string has
int Utf8Length(const std::string& s) {
    int count = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) count++;   // skip the "continuation" bytes
    }
    return count;
}

// how many bytes the first N characters take up
int Utf8Prefix(const std::string& s, int characters) {
    int seen = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (seen == characters) return (int)i;
            seen++;
        }
    }
    return (int)s.size();
}

// ================= reading text files =================

// removes spaces/tabs (and Windows line-ending characters) from both ends
std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r");
    return s.substr(start, end - start + 1);
}

// cuts a long line of text into rows that fit in maxWidth pixels. Rows only break at a space, and the space
// we break at is dropped, so the characters add up to the original text (the typewriter relies on that).
std::vector<std::string> WrapText(const std::string& text, int fontSize, int maxWidth) {
    std::vector<std::string> rows;
    std::string row;
    bool rowHasWord = false;
    size_t pos = 0;
    while (true) {
        size_t space = text.find(' ', pos);
        std::string word = (space == std::string::npos) ? text.substr(pos) : text.substr(pos, space - pos);
        std::string candidate = rowHasWord ? row + " " + word : word;
        if (rowHasWord && MeasureText(candidate.c_str(), fontSize) > maxWidth) {
            rows.push_back(row);      // this row is full: the space before 'word' becomes the line break
            row = word;
        } else {
            row = candidate;
        }
        rowHasWord = true;
        if (space == std::string::npos) break;
        pos = space + 1;
    }
    rows.push_back(row);
    return rows;
}

// ================= character pictures =================
// Pictures are PNG files in the images/ folder next to story.txt. For a line spoken by Naomi with the expression
// "very sad", the game tries these in order and uses the first one that exists:
//     images/naomi very sad.png   images/naomi_very_sad.png     (the full expression)
//     images/naomi sad.png        images/naomi_sad.png          (just its last word)
//     images/naomi neutral.png    images/naomi_neutral.png
//     images/naomi.png
// If there is no picture at all, the speaker gets the coloured square. The original game names its pictures
// like "naomi very sad", so they can be dropped in as they are.
struct PortraitCache {
    std::map<std::string, Texture2D> loaded;   // path -> texture (loaded once, kept)
    std::set<std::string> missing;             // paths we already looked for and didn't find
};

// "Dr. Fernsworth" -> "dr_fernsworth"
std::string PictureName(const std::string& speaker) {
    std::string out;
    for (size_t i = 0; i < speaker.size(); i++) {
        char c = speaker[i];
        if (c >= 'A' && c <= 'Z') out += (char)(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
        else if (c == ' ' || c == '_') out += '_';
        // anything else (dots, commas, question marks) is dropped
    }
    return out;
}

// the picture for this speaker and expression, or nullptr if there isn't one
const Texture2D* FindPortrait(PortraitCache& cache, const std::string& speaker, const std::string& expression) {
    if (speaker == "Narrator" || speaker == "???") return nullptr;
    std::string name = PictureName(speaker);
    if (name.empty()) return nullptr;

    std::vector<std::string> candidates;
    std::vector<std::string> expressions;
    if (!expression.empty()) {
        expressions.push_back(expression);                                // "very sad"
        size_t lastSpace = expression.rfind(' ');
        if (lastSpace != std::string::npos) expressions.push_back(expression.substr(lastSpace + 1));   // "sad"
    }
    expressions.push_back("neutral");
    for (size_t i = 0; i < expressions.size(); i++) {
        std::string underscored = expressions[i];
        for (size_t c = 0; c < underscored.size(); c++) if (underscored[c] == ' ') underscored[c] = '_';
        candidates.push_back("images/" + name + " " + expressions[i] + ".png");
        candidates.push_back("images/" + name + "_" + underscored + ".png");
    }
    candidates.push_back("images/" + name + ".png");

    for (size_t i = 0; i < candidates.size(); i++) {
        const std::string& path = candidates[i];
        std::map<std::string, Texture2D>::iterator found = cache.loaded.find(path);
        if (found != cache.loaded.end()) return &found->second;
        if (cache.missing.count(path) > 0) continue;
        if (FileExists(path.c_str())) {
            Texture2D texture = LoadTexture(path.c_str());
            if (texture.id > 0) {
                cache.loaded[path] = texture;
                return &cache.loaded[path];
            }
        }
        cache.missing.insert(path);
    }
    return nullptr;
}

// "a | b | c" -> { "a", "b", "c" }
std::vector<std::string> Split(const std::string& s, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == separator) {
            parts.push_back(Trim(current));
            current = "";
        } else {
            current += s[i];
        }
    }
    parts.push_back(Trim(current));
    return parts;
}

// true for things like "3" or "-1"
bool IsNumber(const std::string& s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

// stores the error message and says "it failed"
bool Fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

std::string Lower(std::string s) {
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);
    }
    return s;
}

// reads  flag:name   !flag:name   anon>=2   partner<1   (no spaces needed)
bool ParseSingleCondition(const std::string& text, Condition& cond, std::string& error) {
    cond = Condition();
    std::string t = Trim(text);
    if (t.empty()) return Fail(error, "the condition is empty");

    bool negate = false;
    if (t[0] == '!') {
        negate = true;
        t = Trim(t.substr(1));
    }

    if (Lower(t).rfind("flag:", 0) == 0) {
        std::string name = Trim(t.substr(5));
        if (name.empty() || name.find(' ') != std::string::npos) {
            return Fail(error, "expected flag:name (the name can't be empty or contain spaces)");
        }
        cond.kind = negate ? Condition::FLAG_NOT_SET : Condition::FLAG_SET;
        cond.flag = name;
        return true;
    }
    if (negate) return Fail(error, "'!' only works in front of flag:name");

    size_t pos = t.find_first_of("<>=");
    if (pos == std::string::npos) {
        return Fail(error, "expected flag:name, !flag:name or something like anon>=2");
    }
    std::string op = (pos + 1 < t.size() && t[pos + 1] == '=') ? t.substr(pos, 2) : t.substr(pos, 1);
    if (op != ">=" && op != ">" && op != "<=" && op != "<" && op != "==") {
        return Fail(error, "unknown comparison '" + op + "' (use >=, >, <=, <, ==)");
    }

    std::string score = Lower(Trim(t.substr(0, pos)));
    if (score == "anon") cond.useAnon = true;
    else if (score == "partner") cond.useAnon = false;
    else return Fail(error, "unknown score '" + score + "' (use anon or partner)");

    std::string number = Trim(t.substr(pos + op.size()));
    if (!IsNumber(number)) return Fail(error, "the number after the comparison is missing or not a whole number");

    cond.kind = Condition::STAT_COMPARE;
    cond.op = op;
    cond.value = std::stoi(number);
    return true;
}

// conditions can be joined with &:   flag:a & !flag:b & anon>=2   (all of them must be true)
bool ParseCondition(const std::string& text, Condition& cond, std::string& error) {
    std::vector<std::string> parts = Split(text, '&');
    if (!ParseSingleCondition(parts[0], cond, error)) return false;
    for (size_t i = 1; i < parts.size(); i++) {
        Condition more;
        if (!ParseSingleCondition(parts[i], more, error)) return false;
        cond.also.push_back(more);
    }
    return true;
}

// every flag name a condition looks at (including the ones joined with &)
void ConditionFlags(const Condition& c, std::vector<std::string>& out) {
    if (c.kind == Condition::FLAG_SET || c.kind == Condition::FLAG_NOT_SET) out.push_back(c.flag);
    for (size_t i = 0; i < c.also.size(); i++) ConditionFlags(c.also[i], out);
}

// ================= loading crowd.txt =================
// CROWD <map> <x> <y> <name> | line | line | ...
// A background character. Optional file: without it the world is just the story characters.
bool LoadCrowd(const std::string& path, World& world, const std::vector<Scene>& story, std::string& error) {
    std::ifstream in(path);
    if (!in) return true;                                  // no crowd file is fine

    static const Color palette[] = {
        { 196, 164, 132, 255 }, { 150, 170, 190, 255 }, { 190, 150, 170, 255 }, { 160, 185, 150, 255 },
        { 200, 190, 130, 255 }, { 140, 140, 170, 255 }, { 185, 140, 120, 255 }, { 150, 190, 185, 255 } };
    const int paletteSize = 8;

    std::string rawLine;
    int lineNumber = 0;
    while (std::getline(in, rawLine)) {
        lineNumber++;
        std::string line = Trim(rawLine);
        if (line.empty() || line[0] == '#') continue;
        std::string where = path + " line " + std::to_string(lineNumber) + ": ";

        if (line.rfind("CROWD ", 0) != 0) return Fail(error, where + "expected: CROWD <map> <x> <y> <name> | line | line ...");
        std::vector<std::string> parts = Split(line, '|');
        if (parts.size() < 2) return Fail(error, where + "a background character needs at least one line to say (after a | )");

        std::istringstream head(parts[0]);
        std::string tag, mapName;
        int x = 0, y = 0;
        if (!(head >> tag >> mapName >> x >> y)) return Fail(error, where + "expected: CROWD <map> <x> <y> <name> | line | line ...");
        std::string name;
        std::getline(head, name);
        name = Trim(name);
        if (name.empty()) return Fail(error, where + "the character needs a name (like Student)");

        int mapIndex = FindMap(world, mapName);
        if (mapIndex == -1) return Fail(error, where + "there is no map called " + mapName);
        const Map& map = world.maps[mapIndex];
        if (y < 0 || y >= (int)map.rows.size() || x < 0 || x >= (int)map.rows[y].size()) {
            return Fail(error, where + "(" + std::to_string(x) + "," + std::to_string(y) + ") is outside the map " + mapName);
        }
        if (IsSolidTile(map.rows[y][x])) return Fail(error, where + "(" + std::to_string(x) + "," + std::to_string(y) + ") in " + mapName + " is a wall or an obstacle");
        for (size_t w = 0; w < map.warps.size(); w++) {
            if (map.warps[w].x == x && map.warps[w].y == y) return Fail(error, where + "(" + std::to_string(x) + "," + std::to_string(y) + ") in " + mapName + " is a door");
        }
        if (mapIndex == world.startMap && x == world.startX && y == world.startY) return Fail(error, where + "that is where the player starts");
        for (size_t c = 0; c < world.crowd.size(); c++) {
            if (world.crowd[c].map == mapIndex && world.crowd[c].tileX == x && world.crowd[c].tileY == y) {
                return Fail(error, where + "another background character (line " + std::to_string(world.crowd[c].sourceLine) + ") already stands there");
            }
        }
        for (size_t s = 0; s < story.size(); s++) {
            if (story[s].map == mapIndex && story[s].tileX == x && story[s].tileY == y) {
                return Fail(error, where + "a story scene already uses that tile (" + story[s].npc + ", chapter " + std::to_string(story[s].chapter) + ")");
            }
        }

        CrowdMember member;
        member.map = mapIndex;
        member.tileX = x;
        member.tileY = y;
        member.name = name;
        member.sourceLine = lineNumber;
        member.color = palette[world.crowd.size() % paletteSize];
        for (size_t p = 1; p < parts.size(); p++) {
            if (parts[p].empty()) return Fail(error, where + "empty line after a | (remove the extra |)");
            member.lines.push_back(parts[p]);
        }
        world.crowd.push_back(member);
    }
    return true;
}

// the background character closest to the player (within range pixels), or -1
int NearestCrowd(const World& world, int map, Vector2 playerCenter, float range) {
    int best = -1;
    float bestDistance = range;
    for (size_t i = 0; i < world.crowd.size(); i++) {
        const CrowdMember& c = world.crowd[i];
        if (c.map != map) continue;
        Vector2 center = { c.tileX * (float)TILE + TILE / 2.0f, c.tileY * (float)TILE + TILE / 2.0f };
        float d = Vector2Distance(center, playerCenter);
        if (d <= bestDistance) { bestDistance = d; best = (int)i; }
    }
    return best;
}

// ================= loading world.txt =================

struct PendingWarp {
    std::string from;
    std::string to;
    int x, y, toX, toY;
    int line;
};

bool LoadWorld(const std::string& path, World& world, std::string& error) {
    world = World();

    std::ifstream in(path);
    if (!in) {
        return Fail(error, "Could not open " + path + " (run the game from the mygame folder)");
    }

    std::vector<PendingWarp> pending;      // doors, checked after every map is read
    bool hasStart = false;
    std::string startName;
    int startX = 0, startY = 0, startLine = 0;

    Map current;
    bool inMap = false;
    std::string rawLine;
    int lineNumber = 0;

    while (std::getline(in, rawLine)) {
        lineNumber++;
        std::string line = Trim(rawLine);
        std::string where = path + " line " + std::to_string(lineNumber) + ": ";

        if (inMap) {
            // inside a map: every line is a row of tiles, until END
            if (line == "END") {
                if (current.rows.empty()) return Fail(error, where + "this map has no rows");
                for (size_t r = 0; r < current.rows.size(); r++) {
                    if (current.rows[r].size() != current.rows[0].size()) {
                        return Fail(error, path + ": map " + current.name + ", row " + std::to_string(r + 1)
                                           + " is not the same length as row 1");
                    }
                    for (size_t c = 0; c < current.rows[r].size(); c++) {
                        if (!IsKnownTile(current.rows[r][c])) {
                            return Fail(error, path + ": map " + current.name + ", row " + std::to_string(r + 1)
                                               + " has an unknown tile '" + std::string(1, current.rows[r][c]) + "'");
                        }
                    }
                }
                world.maps.push_back(current);
                inMap = false;
            }
            else if (line.empty() || line.find(' ') != std::string::npos) {
                return Fail(error, where + "a row of tiles can't be empty or contain spaces (end the map with END)");
            }
            else {
                current.rows.push_back(line);
            }
            continue;
        }

        if (line.empty() || line[0] == '#') continue;       // blank lines and comments

        if (line.rfind("MAP ", 0) == 0) {
            std::string name = Trim(line.substr(4));
            if (name.empty() || name.find(' ') != std::string::npos) {
                return Fail(error, where + "expected: MAP <name> (no spaces, use _ instead)");
            }
            if (FindMap(world, name) != -1) return Fail(error, where + "there are two maps called " + name);
            current = Map();
            current.name = name;
            inMap = true;
        }
        else if (line.rfind("WARP ", 0) == 0) {
            std::istringstream words(line);
            std::string keyword, from, fx, fy, to, tx, ty;
            words >> keyword >> from >> fx >> fy >> to >> tx >> ty;
            if (ty.empty() || !IsNumber(fx) || !IsNumber(fy) || !IsNumber(tx) || !IsNumber(ty)) {
                return Fail(error, where + "expected: WARP <map> <x> <y> <to-map> <x> <y>");
            }
            PendingWarp p;
            p.from = from;
            p.to = to;
            p.x = std::stoi(fx);
            p.y = std::stoi(fy);
            p.toX = std::stoi(tx);
            p.toY = std::stoi(ty);
            p.line = lineNumber;
            pending.push_back(p);
        }
        else if (line.rfind("START ", 0) == 0) {
            std::istringstream words(line);
            std::string keyword, name, sx, sy;
            words >> keyword >> name >> sx >> sy;
            if (sy.empty() || !IsNumber(sx) || !IsNumber(sy)) {
                return Fail(error, where + "expected: START <map> <x> <y>");
            }
            if (hasStart) return Fail(error, where + "there can only be one START");
            hasStart = true;
            startName = name;
            startX = std::stoi(sx);
            startY = std::stoi(sy);
            startLine = lineNumber;
        }
        else {
            return Fail(error, where + "I don't understand this line (expected MAP, WARP or START)");
        }
    }

    if (inMap) return Fail(error, path + ": the last map has no END");
    if (world.maps.empty()) return Fail(error, path + ": there are no maps");
    if (!hasStart) return Fail(error, path + ": there is no START line");

    // now that every map exists, check the doors
    for (size_t i = 0; i < pending.size(); i++) {
        const PendingWarp& p = pending[i];
        std::string where = path + " line " + std::to_string(p.line) + ": ";

        int from = FindMap(world, p.from);
        int to = FindMap(world, p.to);
        if (from == -1) return Fail(error, where + "there is no map named " + p.from);
        if (to == -1) return Fail(error, where + "there is no map named " + p.to);
        if (IsSolidAt(world.maps[from], p.x, p.y)) {
            return Fail(error, where + "the door tile is outside the map or a wall/obstacle");
        }
        if (IsSolidAt(world.maps[to], p.toX, p.toY)) {
            return Fail(error, where + "the arrival tile is outside the map or a wall/obstacle");
        }

        Warp w;
        w.x = p.x;
        w.y = p.y;
        w.toMap = to;
        w.toX = p.toX;
        w.toY = p.toY;
        w.sourceLine = p.line;
        world.maps[from].warps.push_back(w);
    }

    // arriving on a door would send you straight back, so that's not allowed
    for (size_t m = 0; m < world.maps.size(); m++) {
        for (size_t i = 0; i < world.maps[m].warps.size(); i++) {
            const Warp& w = world.maps[m].warps[i];
            if (IsWarpTile(world.maps[w.toMap], w.toX, w.toY)) {
                return Fail(error, path + " line " + std::to_string(w.sourceLine)
                                   + ": the arrival tile is itself a door (move it one step in front of the door)");
            }
        }
    }

    // the start
    std::string where = path + " line " + std::to_string(startLine) + ": ";
    int startIndex = FindMap(world, startName);
    if (startIndex == -1) return Fail(error, where + "there is no map named " + startName);
    if (IsSolidAt(world.maps[startIndex], startX, startY)) {
        return Fail(error, where + "the start tile is outside the map or a wall/obstacle");
    }
    if (IsWarpTile(world.maps[startIndex], startX, startY)) {
        return Fail(error, where + "the start tile is a door");
    }
    world.startMap = startIndex;
    world.startX = startX;
    world.startY = startY;
    return true;
}

// which maps can you reach from the start by walking through doors?
std::vector<bool> ReachableMaps(const World& world) {
    std::vector<bool> seen(world.maps.size(), false);
    std::vector<int> todo;
    seen[world.startMap] = true;
    todo.push_back(world.startMap);

    while (!todo.empty()) {
        int m = todo.back();
        todo.pop_back();
        for (size_t i = 0; i < world.maps[m].warps.size(); i++) {
            int next = world.maps[m].warps[i].toMap;
            if (!seen[next]) {
                seen[next] = true;
                todo.push_back(next);
            }
        }
    }
    return seen;
}

// A scene's name for the save file: route:chapter:character:map:x:y
// (stays the same even if you reorder story.txt, so saves keep working)
std::string RunKey(const Scene& s, const World& world) {
    return std::to_string(s.route) + ":" + std::to_string(s.chapter) + ":" + s.npc + ":" +
           world.maps[s.map].name + ":" + std::to_string(s.tileX) + ":" + std::to_string(s.tileY);
}

// ================= loading story.txt =================

bool LoadScenes(const std::string& path, const World& world, const std::vector<Character>& cast,
                std::vector<Scene>& scenes, std::string& error) {
    scenes.clear();

    std::ifstream in(path);
    if (!in) {
        return Fail(error, "Could not open " + path + " (run the game from the mygame folder)");
    }

    std::vector<bool> reachable = ReachableMaps(world);

    Scene current;
    bool inScene = false;
    bool inBlock = false;                // are we between IF and ENDIF?
    Condition blockRequirement;          // the IF condition, given to every line inside the block
    std::string rawLine;
    int lineNumber = 0;

    while (std::getline(in, rawLine)) {
        lineNumber++;
        std::string line = Trim(rawLine);
        if (line.empty() || line[0] == '#') continue;       // blank lines and comments

        std::string where = path + " line " + std::to_string(lineNumber) + ": ";

        if (line.rfind("SCENE ", 0) == 0) {
            // SCENE <route> <chapter> <npc> <map> <x> <y>
            if (inScene) return Fail(error, where + "the previous scene has no END");
            inBlock = false;
            blockRequirement = Condition();

            std::istringstream words(line);
            std::string keyword, routeText, chapterText, npcName, mapName, xText, yText, extra1, extra2, tooMany;
            words >> keyword >> routeText >> chapterText >> npcName >> mapName >> xText >> yText >> extra1 >> extra2 >> tooMany;

            if (yText.empty() || !tooMany.empty() || !IsNumber(routeText) || !IsNumber(chapterText) || !IsNumber(xText) || !IsNumber(yText)) {
                return Fail(error, where + "expected: SCENE <route> <chapter> <npc> <map> <x> <y>  (then optionally AUTO and/or REQ:condition)");
            }

            // the optional extras at the end: AUTO and REQ:condition, in any order
            bool autoStart = false;
            Condition sceneRequirement;
            std::string extras[2] = { extra1, extra2 };
            for (int e = 0; e < 2; e++) {
                if (extras[e].empty()) continue;
                std::string lowerExtra = Lower(extras[e]);

                if (lowerExtra == "auto") {
                    if (autoStart) return Fail(error, where + "AUTO is written twice");
                    autoStart = true;
                }
                else if (lowerExtra.rfind("req:", 0) == 0) {
                    std::string conditionError;
                    if (sceneRequirement.kind != Condition::NONE) return Fail(error, where + "a scene can only have one REQ");
                    if (!ParseCondition(extras[e].substr(4), sceneRequirement, conditionError)) {
                        return Fail(error, where + "bad REQ: " + conditionError);
                    }
                }
                else {
                    return Fail(error, where + "after the tile numbers only AUTO and REQ:condition are allowed (no spaces inside the condition)");
                }
            }
            int route = std::stoi(routeText);
            int chapter = std::stoi(chapterText);
            int tx = std::stoi(xText);
            int ty = std::stoi(yText);

            if (route < -1 || route >= ROUTE_COUNT) {
                return Fail(error, where + "route must be -1 (original story) or 0 to " + std::to_string(ROUTE_COUNT - 1));
            }
            if (chapter < 1) return Fail(error, where + "chapter must be 1 or higher");

            // "Narrator" is not a character: nobody stands there, so it only works for AUTO scenes
            bool knownNpc = (npcName == "Narrator" && autoStart);
            for (size_t i = 0; i < cast.size(); i++) {
                if (cast[i].name == npcName) knownNpc = true;
            }
            if (npcName == "Narrator" && !autoStart) {
                return Fail(error, where + "a Narrator scene needs AUTO (there is nobody to press E on)");
            }
            if (!knownNpc) return Fail(error, where + "there is no character named " + npcName);

            int mapIndex = FindMap(world, mapName);
            if (mapIndex == -1) return Fail(error, where + "there is no map named " + mapName);
            if (!reachable[mapIndex]) {
                return Fail(error, where + "you can't get to " + mapName + " from the start (a door is missing)");
            }
            if (IsSolidAt(world.maps[mapIndex], tx, ty)) {
                return Fail(error, where + "that tile is outside the map or a wall/obstacle");
            }
            if (IsWarpTile(world.maps[mapIndex], tx, ty)) {
                return Fail(error, where + "that tile is a door");
            }

            current = Scene();
            current.route = route;
            current.chapter = chapter;
            current.npc = npcName;
            current.map = mapIndex;
            current.tileX = tx;
            current.tileY = ty;
            current.requirement = sceneRequirement;
            current.autoStart = autoStart;
            inScene = true;
        }
        else if (line == "END") {
            if (!inScene) return Fail(error, where + "END without a SCENE");
            if (current.lines.empty()) return Fail(error, where + "this scene has no lines");
            if (inBlock) return Fail(error, where + "this scene has an IF without an ENDIF");
            bool someLineAlwaysPlays = false;
            for (size_t i = 0; i < current.lines.size(); i++) {
                if (current.lines[i].requirement.kind == Condition::NONE) someLineAlwaysPlays = true;
            }
            if (!someLineAlwaysPlays) return Fail(error, where + "every line in this scene is inside an IF, so it could play nothing");
            for (size_t i = 0; i < current.lines.size(); i++) {
                bool hasQuestion = !current.lines[i].choices.empty();
                bool alwaysThere = false;
                for (size_t c = 0; c < current.lines[i].choices.size(); c++) {
                    if (current.lines[i].choices[c].requirement.kind == Condition::NONE) alwaysThere = true;
                }
                if (hasQuestion && !alwaysThere) {
                    return Fail(error, where + "the question \"" + current.lines[i].text + "\" needs at least one answer without a REQ");
                }
            }
            scenes.push_back(current);
            inScene = false;
        }
        else if (!inScene) {
            return Fail(error, where + "this line is outside a SCENE");
        }
        else if (line.rfind("IF ", 0) == 0) {
            // IF condition   (the lines up to ENDIF only play while the condition is true)
            if (inBlock) return Fail(error, where + "IF blocks can't be inside each other (close the other one with ENDIF first)");
            std::string conditionError;
            if (!ParseCondition(line.substr(3), blockRequirement, conditionError)) {
                return Fail(error, where + "bad IF: " + conditionError);
            }
            inBlock = true;
        }
        else if (line == "ENDIF") {
            if (!inBlock) return Fail(error, where + "ENDIF without an IF");
            inBlock = false;
            blockRequirement = Condition();
        }
        else if (line[0] == '?' && !(line.size() > 1 && line[1] == '?')) {
            // ? answer | anonPoints | partnerPoints | reply   ("???: text" is dialogue, not an answer)
            if (current.lines.empty()) return Fail(error, where + "an answer needs a line of dialogue above it");

            std::vector<std::string> parts = Split(line.substr(1), '|');
            if (parts.size() < 4 || parts[0].empty() || !IsNumber(parts[1]) || !IsNumber(parts[2])) {
                return Fail(error, where + "expected: ? answer | anonPoints | partnerPoints | reply  (then optionally FLAG: name and REQ: condition)");
            }

            Choice c;
            c.text = parts[0];
            c.anonPoints = std::stoi(parts[1]);
            c.partnerPoints = std::stoi(parts[2]);
            c.reply = parts[3];

            // optional extras: FLAG: name  and  REQ: condition
            for (size_t i = 4; i < parts.size(); i++) {
                std::string lowerPart = Lower(parts[i]);
                if (lowerPart.rfind("flag:", 0) == 0) {
                    std::string name = Trim(parts[i].substr(5));
                    if (name.empty() || name.find(' ') != std::string::npos) {
                        return Fail(error, where + "expected FLAG: name (the name can't be empty or contain spaces)");
                    }
                    c.flagsToSet.push_back(name);
                }
                else if (lowerPart.rfind("req:", 0) == 0) {
                    std::string conditionError;
                    if (c.requirement.kind != Condition::NONE) return Fail(error, where + "an answer can only have one REQ");
                    if (!ParseCondition(parts[i].substr(4), c.requirement, conditionError)) {
                        return Fail(error, where + "bad REQ: " + conditionError);
                    }
                }
                else {
                    return Fail(error, where + "I don't understand '" + parts[i] + "' (expected FLAG: name or REQ: condition)");
                }
            }
            current.lines.back().choices.push_back(c);   // attaches to the line above
        }
        else if (Lower(line).rfind("flag:", 0) == 0) {
            // FLAG: name   (on its own line: switched on when the scene ends)
            std::string name = Trim(line.substr(5));
            if (name.empty() || name.find(' ') != std::string::npos) {
                return Fail(error, where + "expected FLAG: name (the name can't be empty or contain spaces)");
            }
            current.flagsOnFinish.push_back(name);
        }
        else {
            // Name: text
            size_t colon = line.find(':');
            if (colon == std::string::npos) return Fail(error, where + "expected: Name: text  (or Name (expression): text)");

            std::string who = Trim(line.substr(0, colon));
            std::string expression;
            size_t open = who.find('(');
            if (open != std::string::npos) {
                if (who.back() != ')') return Fail(error, where + "expected: Name (expression): text");
                expression = Trim(who.substr(open + 1, who.size() - open - 2));
                who = Trim(who.substr(0, open));
                if (expression.empty()) return Fail(error, where + "the expression in ( ) can't be empty");
            }
            if (who.empty()) return Fail(error, where + "expected: Name: text");

            Line l;
            l.speaker = who;
            l.expression = expression;
            l.text = Trim(line.substr(colon + 1));
            l.requirement = blockRequirement;      // NONE outside an IF block
            current.lines.push_back(l);
        }
    }

    if (inScene) return Fail(error, path + ": the last scene has no END");

    // two scenes with the same route, chapter, character and tile couldn't be told apart in a save file
    std::set<std::string> keys;
    for (size_t s = 0; s < scenes.size(); s++) {
        std::string key = RunKey(scenes[s], world);
        if (keys.count(key) > 0) {
            return Fail(error, path + ": two scenes share the same route, chapter, character and tile (" + key + "). Move one of them to another tile.");
        }
        keys.insert(key);
    }

    // catch typos: a flag used in a REQ must be switched on by some answer
    std::set<std::string> knownFlags;
    for (size_t s = 0; s < scenes.size(); s++) {
        for (size_t f = 0; f < scenes[s].flagsOnFinish.size(); f++) knownFlags.insert(scenes[s].flagsOnFinish[f]);
        for (size_t i = 0; i < scenes[s].lines.size(); i++) {
            for (size_t c = 0; c < scenes[s].lines[i].choices.size(); c++) {
                const std::vector<std::string>& set = scenes[s].lines[i].choices[c].flagsToSet;
                for (size_t f = 0; f < set.size(); f++) knownFlags.insert(set[f]);
            }
        }
    }
    for (size_t s = 0; s < scenes.size(); s++) {
        std::vector<std::string> used;
        ConditionFlags(scenes[s].requirement, used);
        for (size_t u = 0; u < used.size(); u++) {
            if (knownFlags.count(used[u]) == 0) {
                return Fail(error, path + ": the scene with " + scenes[s].npc + " in chapter " + std::to_string(scenes[s].chapter)
                                   + " needs the flag " + used[u] + ", but no answer or scene ever sets it (typo?)");
            }
        }
        for (size_t i = 0; i < scenes[s].lines.size(); i++) {
            used.clear();
            ConditionFlags(scenes[s].lines[i].requirement, used);
            for (size_t u = 0; u < used.size(); u++) {
                if (knownFlags.count(used[u]) == 0) {
                    return Fail(error, path + ": a line in the scene with " + scenes[s].npc + " in chapter " + std::to_string(scenes[s].chapter)
                                       + " (\"" + scenes[s].lines[i].text.substr(0, 30) + "...\") needs the flag " + used[u]
                                       + " in an IF, but no answer or scene ever sets it (typo?)");
                }
            }
            for (size_t c = 0; c < scenes[s].lines[i].choices.size(); c++) {
                const Choice& choice = scenes[s].lines[i].choices[c];
                used.clear();
                ConditionFlags(choice.requirement, used);
                for (size_t u = 0; u < used.size(); u++) {
                    if (knownFlags.count(used[u]) == 0) {
                        return Fail(error, path + ": the answer \"" + choice.text + "\" needs the flag " + used[u]
                                           + ", but no answer or scene ever sets it (typo?)");
                    }
                }
            }
        }
    }
    return true;
}

// Every chapter from 1 up to the last one needs at least one scene, or the story gets stuck.
std::string CheckChapters(const std::vector<Scene>& scenes) {
    for (int route = -1; route < ROUTE_COUNT; route++) {
        int last = 0;
        for (size_t i = 0; i < scenes.size(); i++) {
            if (scenes[i].route == route && scenes[i].chapter > last) last = scenes[i].chapter;
        }

        if (route == -1 && last == 0) return "the original story (route -1) has no scenes";

        for (int chapter = 1; chapter <= last; chapter++) {
            bool found = false;
            bool alwaysThere = false;
            for (size_t i = 0; i < scenes.size(); i++) {
                if (scenes[i].route == route && scenes[i].chapter == chapter) {
                    found = true;
                    if (scenes[i].requirement.kind == Condition::NONE) alwaysThere = true;
                }
            }
            if (!found) {
                return "route " + std::to_string(route) + " has no scene in chapter " + std::to_string(chapter);
            }
            if (!alwaysThere) {
                return "route " + std::to_string(route) + ", chapter " + std::to_string(chapter)
                       + ": every scene has a REQ, so the chapter could get stuck (add one that always appears)";
            }
        }
    }
    return "";
}

// loads and checks world.txt and story.txt; returns "" when everything is fine
std::string LoadAll(World& world, std::vector<Scene>& story, const std::vector<Character>& cast) {
    std::string error;
    if (LoadWorld(WORLD_FILE, world, error)) {
        if (LoadScenes(STORY_FILE, world, cast, story, error)) {
            error = CheckChapters(story);
            if (error.empty()) LoadCrowd(CROWD_FILE, world, story, error);
        }
    }
    if (!error.empty()) {
        world = World();
        story.clear();
    }
    return error;
}

// does this route have any scenes written for it?
bool RouteHasStory(const std::vector<Scene>& story, int route) {
    for (size_t i = 0; i < story.size(); i++) {
        if (story[i].route == route) return true;
    }
    return false;
}

Run NewRun(int route, const std::vector<Scene>& story, const World& world) {
    Run r;
    r.route = route;
    r.scenes = story;                  // a fresh copy, so every scene is unfinished again
    r.map = world.startMap;
    r.position = { world.startX * (float)TILE + 4, world.startY * (float)TILE + 4 };
    return r;
}

// ================= story helpers =================

// is this scene waiting for the player right now?
bool IsPending(const Run& run, const Scene& s) {
    return !s.done && s.route == run.route && s.chapter == run.chapter && ConditionMet(run, s.requirement);
}

// how many scenes are still unfinished in the current chapter
int PendingScenes(const Run& run) {
    int count = 0;
    for (size_t i = 0; i < run.scenes.size(); i++) {
        if (IsPending(run, run.scenes[i])) count++;
    }
    return count;
}

// the highest chapter number this route has
int LastChapter(const Run& run) {
    int last = 0;
    for (size_t i = 0; i < run.scenes.size(); i++) {
        const Scene& s = run.scenes[i];
        if (s.route == run.route && s.chapter > last) last = s.chapter;
    }
    return last;
}

// the closest waiting scene on the map you're standing in (or -1). wantAuto picks which kind: E-scenes or AUTO scenes.
int NearestScene(const Run& run, Vector2 playerCenter, float maxDistance, bool wantAuto) {
    int best = -1;
    float bestDistance = maxDistance;
    for (int i = 0; i < (int)run.scenes.size(); i++) {
        const Scene& s = run.scenes[i];
        if (!IsPending(run, s) || s.map != run.map || s.autoStart != wantAuto) continue;

        Vector2 center = { s.tileX * (float)TILE + 16, s.tileY * (float)TILE + 16 };
        float d = Vector2Distance(playerCenter, center);
        if (d < bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

// the "debug scores" option: the Anon / partner scores are hidden unless this is on (F4 or the pause menu)
bool LoadDebugScores() {
    std::ifstream in(SETTINGS_FILE);
    std::string name;
    int value = 0;
    while (in >> name >> value) {
        if (name == "debug_scores") return value != 0;
    }
    return false;                                  // no file, or no such line: off
}

void SaveDebugScores(bool on) {
    std::ofstream out(SETTINGS_FILE);
    out << "debug_scores " << (on ? 1 : 0) << "\n";
}

const char* PartnerName(int route) {
    return (route == -1) ? "Fang" : routePartners[route];
}

// ================= saving and loading the run in progress =================

// Writes savegame.txt: route, chapter, scores, map, position, flags and finished scenes.
bool SaveRun(const Run& run, const World& world, const std::string& path = RUN_SAVE_FILE) {
    std::ofstream out(path);
    if (!out) return false;

    out << "SNOOT_SAVE 1\n";
    out << "route " << run.route << "\n";
    out << "chapter " << run.chapter << "\n";
    out << "anon " << run.anonPoints << "\n";
    out << "partner " << run.partnerPoints << "\n";
    out << "map " << world.maps[run.map].name << "\n";
    out << "pos " << run.position.x << " " << run.position.y << "\n";
    for (const std::string& flag : run.flags) out << "flag " << flag << "\n";
    for (size_t i = 0; i < run.scenes.size(); i++) {
        if (run.scenes[i].done) out << "done " << RunKey(run.scenes[i], world) << "\n";
    }
    return out.good();
}

bool RunSaveExists(const std::string& path = RUN_SAVE_FILE) {
    std::ifstream in(path);
    return (bool)in;
}

// Reads savegame.txt into a fresh run. If the save doesn't fit the current story/world, 'error' says why.
bool LoadRun(Run& result, const World& world, const std::vector<Scene>& story, std::string& error, const std::string& path = RUN_SAVE_FILE) {
    std::ifstream in(path);
    if (!in) return Fail(error, "there is no saved game");

    std::string header;
    std::getline(in, header);
    if (Trim(header) != "SNOOT_SAVE 1") return Fail(error, "this isn't a save file from this version of the game");

    int route = -1, chapter = 1, anon = 0, partner = 0;
    std::string mapName;
    float px = 0, py = 0;
    std::set<std::string> flags, doneKeys;

    std::string rawLine;
    while (std::getline(in, rawLine)) {
        std::string line = Trim(rawLine);
        if (line.empty()) continue;

        std::istringstream words(line);
        std::string key;
        words >> key;

        if      (key == "route")   words >> route;
        else if (key == "chapter") words >> chapter;
        else if (key == "anon")    words >> anon;
        else if (key == "partner") words >> partner;
        else if (key == "map")     words >> mapName;
        else if (key == "pos")     words >> px >> py;
        else if (key == "flag")    { std::string f; words >> f; flags.insert(f); }
        else if (key == "done")    { std::string d; words >> d; doneKeys.insert(d); }
        else return Fail(error, "the save has a line I don't understand: " + line);

        if (words.fail()) return Fail(error, "the save has a broken line: " + line);
    }

    if (route < -1 || route >= ROUTE_COUNT || (route != -1 && !RouteHasStory(story, route))) {
        return Fail(error, "the saved route doesn't exist in the current story");
    }

    Run r = NewRun(route, story, world);
    if (chapter < 1 || chapter > LastChapter(r)) {
        return Fail(error, "the saved chapter doesn't exist in the current story (the story changed)");
    }
    int mapIndex = FindMap(world, mapName);
    if (mapIndex == -1) return Fail(error, "the saved map '" + mapName + "' is not in world.txt any more");

    r.chapter = chapter;
    r.anonPoints = anon;
    r.partnerPoints = partner;
    r.flags = flags;
    r.map = mapIndex;
    r.position = { px, py };
    if (Blocked(world.maps[mapIndex], r.position, 24)) {
        // the map was edited and you'd be standing in a wall: start the run's map from the beginning instead
        r.map = world.startMap;
        r.position = { world.startX * (float)TILE + 4, world.startY * (float)TILE + 4 };
    }
    for (size_t i = 0; i < r.scenes.size(); i++) {
        if (doneKeys.count(RunKey(r.scenes[i], world)) > 0) r.scenes[i].done = true;
    }

    result = r;
    return true;
}

// Is there a save we can continue from? Also writes a one-line description for the menu.
bool CheckSave(const World& world, const std::vector<Scene>& story, std::string& note, const std::string& path = RUN_SAVE_FILE) {
    note = "";
    if (!RunSaveExists(path)) return false;

    Run loaded;
    std::string error;
    if (!LoadRun(loaded, world, story, error, path)) {
        note = "Save can't be used: " + error;
        return false;
    }
    std::string who = (loaded.route == -1) ? "Original story" : routeNames[loaded.route];
    note = who + ", chapter " + std::to_string(loaded.chapter) + "/" + std::to_string(LastChapter(loaded)) +
           ", " + PrettyName(world.maps[loaded.map].name);
    return true;
}

int main() {
    InitWindow(800, 448, "My Game");   // 25 x 14 tiles fit on screen
    SetExitKey(KEY_NULL);               // ESC opens the pause menu instead of closing the game
    SetTargetFPS(60);
    srand((unsigned)time(nullptr));     // for the background characters' random lines
    PortraitCache portraits;                 // character pictures from images/ (loaded when first needed)

    float playerSize = 24;
    float speed = 200;

    GameState state = MENU;

    std::vector<Character> cast = CreateCast();
    World world;
    std::vector<Scene> story;
    std::string storyError = LoadAll(world, story, cast);
    if (!storyError.empty()) TraceLog(LOG_ERROR, "%s", storyError.c_str());

    Run run = NewRun(-1, story, world);

    // the conversation happening right now
    std::vector<Line> activeLines;
    int activeScene = -1;              // which scene it belongs to
    int currentLine = 0;
    int selected = 0;                  // which choice is highlighted
    float typed = 0;                   // how many characters of the current line are showing
    bool debugMode = false;
    bool showScores = LoadDebugScores();   // the debug scores option (off unless the player turns it on)            // F3 turns the debug overlay on and off

    // progress that survives between runs (saved in save.txt)
    bool endingsSeen[ENDING_COUNT] = { false };
    bool routesDone[ROUTE_COUNT] = { false };
    LoadProgress(endingsSeen, routesDone);

    std::string endingTitle;           // shown on the ending screen
    bool justUnlocked = false;         // true when this run unlocked the extra routes

    int menuIndex = 2;                 // New Game
    int routeIndex = 0;
    bool quitRequested = false;

    // pause menu and load screen
    int pauseScreen = 0;               // 0 = the pause list, 1 = stats, 2 = save slots
    int pauseIndex = 0;
    int slotIndex = 0;                 // highlighted entry on the save / load screens
    GameState loadReturn = MENU;       // where BACKSPACE on the load screen goes back to
    bool slotOk[SLOT_COUNT + 1] = { false };      // [0] = the autosave, [1..3] = the manual slots
    std::string slotNote[SLOT_COUNT + 1];

    // the saved run (if any)
    std::string saveNote;              // shown next to "Continue" in the menu
    bool continueAvailable = !storyError.empty() ? false : CheckSave(world, story, saveNote);
    if (continueAvailable) menuIndex = 0;     // start the menu on Continue

    std::string toastText;             // a short message ("Game saved")
    float toastTimer = 0;

    // re-reads the autosave and the three slots, for the save and load screens
    auto RefreshSlots = [&]() {
        for (int i = 0; i <= SLOT_COUNT; i++) {
            std::string path = (i == 0) ? std::string(RUN_SAVE_FILE) : SlotFile(i);
            bool exists = RunSaveExists(path);
            std::string note;
            slotOk[i] = exists && CheckSave(world, story, note, path);
            slotNote[i] = exists ? note : "(empty)";
        }
    };

    while (!WindowShouldClose() && !quitRequested) {
        bool routesUnlocked = (CountTrue(endingsSeen, ENDING_COUNT) == ENDING_COUNT);

        if (IsKeyPressed(KEY_F3)) debugMode = !debugMode;
        if (IsKeyPressed(KEY_F4)) {
            showScores = !showScores;
            SaveDebugScores(showScores);
            toastText = showScores ? "Debug scores on" : "Debug scores off";
            toastTimer = 2.0f;
        }
        if (toastTimer > 0) toastTimer -= GetFrameTime();

        // ---------------- update ----------------
        if (state == MENU) {
            if (IsKeyPressed(KEY_W)) menuIndex--;
            if (IsKeyPressed(KEY_S)) menuIndex++;
            if (menuIndex < 0) menuIndex = MENU_COUNT - 1;
            if (menuIndex >= MENU_COUNT) menuIndex = 0;

            if (IsKeyPressed(KEY_R)) {
                // reload world.txt and story.txt without restarting the game
                storyError = LoadAll(world, story, cast);
                if (!storyError.empty()) TraceLog(LOG_ERROR, "%s", storyError.c_str());
                continueAvailable = storyError.empty() && CheckSave(world, story, saveNote);
            }

            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) {
                if (menuIndex == 0) {
                    // Continue: load the autosave (savegame.txt)
                    Run loaded;
                    std::string loadError;
                    if (continueAvailable && LoadRun(loaded, world, story, loadError)) {
                        run = loaded;
                        activeScene = -1;
                        state = EXPLORING;
                    }
                }
                else if (menuIndex == 1 && storyError.empty()) {
                    RefreshSlots();                         // Load Game: pick the autosave or a slot
                    slotIndex = 0;
                    loadReturn = MENU;
                    state = LOADSCREEN;
                }
                else if (menuIndex == 2 && storyError.empty()) {
                    run = NewRun(-1, story, world);        // a fresh run of the original story
                    state = EXPLORING;
                }
                else if (menuIndex == 3) {
                    routeIndex = 0;
                    state = ROUTES;
                }
                else if (menuIndex == 4) {
                    state = CREDITS;
                }
                else if (menuIndex == 5) {
                    quitRequested = true;
                }
            }
        }
        else if (state == CREDITS) {
            if (IsKeyPressed(KEY_BACKSPACE)) state = MENU;
        }
        else if (state == ROUTES) {
            if (IsKeyPressed(KEY_BACKSPACE)) {
                state = MENU;
            }
            else if (routesUnlocked) {
                if (IsKeyPressed(KEY_W)) routeIndex--;
                if (IsKeyPressed(KEY_S)) routeIndex++;
                if (routeIndex < 0) routeIndex = ROUTE_COUNT - 1;
                if (routeIndex >= ROUTE_COUNT) routeIndex = 0;

                if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) && RouteHasStory(story, routeIndex)) {
                    run = NewRun(routeIndex, story, world);    // a fresh run on that route
                    state = EXPLORING;
                }
            }
        }
        else if (state == EXPLORING && IsKeyPressed(KEY_ESCAPE)) {
            state = PAUSED;                 // the pause menu
            pauseScreen = 0;
            pauseIndex = 0;
        }
        else if (state == EXPLORING) {
            const Map& map = world.maps[run.map];

            Vector2 direction = { 0, 0 };
            if (IsKeyDown(KEY_W)) direction.y -= 1;
            if (IsKeyDown(KEY_S)) direction.y += 1;
            if (IsKeyDown(KEY_A)) direction.x -= 1;
            if (IsKeyDown(KEY_D)) direction.x += 1;
            direction = Vector2Normalize(direction);

            float dt = GetFrameTime();

            Vector2 next = { run.position.x + direction.x * speed * dt, run.position.y };
            if (!Blocked(map, next, playerSize)) run.position.x = next.x;

            next = { run.position.x, run.position.y + direction.y * speed * dt };
            if (!Blocked(map, next, playerSize)) run.position.y = next.y;

            // did we step on a door?
            bool warped = false;
            int tileX = (int)((run.position.x + playerSize / 2) / TILE);
            int tileY = (int)((run.position.y + playerSize / 2) / TILE);
            for (size_t i = 0; i < map.warps.size(); i++) {
                const Warp& w = map.warps[i];
                if (w.x == tileX && w.y == tileY) {
                    run.map = w.toMap;
                    run.position = { w.toX * (float)TILE + 4, w.toY * (float)TILE + 4 };
                    warped = true;
                    break;
                }
            }

            // save when you go through a door, or any time with F5
            if (warped || IsKeyPressed(KEY_F5)) {
                if (SaveRun(run, world)) {
                    toastText = warped ? "Autosaved" : "Game saved";
                    toastTimer = 2.0f;
                    continueAvailable = true;
                } else {
                    toastText = "Could not write savegame.txt";
                    toastTimer = 3.0f;
                }
            }

            Vector2 playerCenter = { run.position.x + playerSize / 2, run.position.y + playerSize / 2 };

            // an AUTO scene starts by itself when you walk up to its spot
            int autoScene = NearestScene(run, playerCenter, TRIGGER_RANGE, true);
            if (autoScene != -1) {
                activeScene = autoScene;
                activeLines = run.scenes[autoScene].lines;
                currentLine = 0;
                while (currentLine < (int)activeLines.size() - 1 && !ConditionMet(run, activeLines[currentLine].requirement)) currentLine++;   // skip lines hidden by an IF
                selected = 0;
                typed = 0;
                state = DIALOGUE;
            }
            else {
                // otherwise: talk to whoever is waiting nearby when you press E
                int nearScene = NearestScene(run, playerCenter, 48, false);
                if (!warped && nearScene != -1 && IsKeyPressed(KEY_E)) {
                    activeScene = nearScene;
                    activeLines = run.scenes[nearScene].lines;
                    currentLine = 0;
                    while (currentLine < (int)activeLines.size() - 1 && !ConditionMet(run, activeLines[currentLine].requirement)) currentLine++;   // skip lines hidden by an IF
                    selected = 0;
                    typed = 0;
                    state = DIALOGUE;
                }
                else if (!warped && nearScene == -1 && IsKeyPressed(KEY_E)) {
                    // no story character here: maybe a background one
                    int who = NearestCrowd(world, run.map, playerCenter, 44);
                    if (who != -1) {
                        CrowdMember& member = world.crowd[who];
                        int pick = rand() % (int)member.lines.size();
                        if (member.lines.size() > 1 && pick == member.lastLine) pick = (pick + 1) % (int)member.lines.size();
                        member.lastLine = pick;
                        Line chat;
                        chat.speaker = member.name;
                        chat.text = member.lines[pick];
                        activeLines.clear();
                        activeLines.push_back(chat);
                        activeScene = -1;                  // not a story scene: nothing to record afterwards
                        currentLine = 0;
                        selected = 0;
                        typed = 0;
                        state = DIALOGUE;
                    }
                }
            }
        }
        else if (state == DIALOGUE) {
            Line& line = activeLines[currentLine];
            float length = (float)Utf8Length(line.text);
            bool advance = false;               // true = go on to the next line that plays

            if (typed < length) {
                // --- the line is still typing out: SPACE shows the rest at once ---
                typed += TYPE_SPEED * GetFrameTime();
                if (IsKeyPressed(KEY_SPACE)) typed = length;
            }
            else if (!line.choices.empty()) {
                // --- a question: pick an answer (locked answers are skipped) ---
                int count = (int)line.choices.size();

                // make sure the highlight is on an answer you can actually pick
                while (!ConditionMet(run, line.choices[selected].requirement)) selected = (selected + 1) % count;

                if (IsKeyPressed(KEY_S)) {
                    do { selected = (selected + 1) % count; } while (!ConditionMet(run, line.choices[selected].requirement));
                }
                if (IsKeyPressed(KEY_W)) {
                    do { selected = (selected + count - 1) % count; } while (!ConditionMet(run, line.choices[selected].requirement));
                }

                if (IsKeyPressed(KEY_SPACE)) {
                    Choice picked = line.choices[selected];       // copy it first
                    run.anonPoints += picked.anonPoints;
                    run.partnerPoints += picked.partnerPoints;
                    for (size_t i = 0; i < picked.flagsToSet.size(); i++) {
                        run.flags.insert(picked.flagsToSet[i]);   // switch the flags on
                    }

                    if (picked.reply.empty()) {
                        advance = true;           // no reply written: just carry on with the story
                    } else {
                        // insert the NPC's reply as a new line right after this one
                        Line reply = { line.speaker, picked.reply, {}, line.expression, Condition() };
                        activeLines.insert(activeLines.begin() + currentLine + 1, reply);
                        currentLine++;
                        selected = 0;
                        typed = 0;
                    }
                }
            }
            else if (IsKeyPressed(KEY_SPACE)) {
                advance = true;                 // a normal line: on to the next one
            }

            if (advance) {
                currentLine++;
                selected = 0;
                typed = 0;
                // lines inside an IF whose condition is false don't play
                while (currentLine < (int)activeLines.size() && !ConditionMet(run, activeLines[currentLine].requirement)) currentLine++;

                if (currentLine >= (int)activeLines.size() && activeScene < 0) {
                    state = EXPLORING;          // a chat with a background character: nothing to record
                }
                else if (currentLine >= (int)activeLines.size()) {
                    // the conversation is over
                    state = EXPLORING;

                    for (size_t f = 0; f < run.scenes[activeScene].flagsOnFinish.size(); f++) {
                        run.flags.insert(run.scenes[activeScene].flagsOnFinish[f]);   // the scene's own flags
                    }
                    run.scenes[activeScene].done = true;
                    activeScene = -1;

                    // chapter finished? move on. Story finished? work out the ending.
                    if (PendingScenes(run) == 0) {
                        run.chapter++;

                        if (run.chapter > LastChapter(run)) {
                            if (run.route == -1) {
                                // original story: the two scores decide which of the 4 endings
                                int ending = DecideEnding(run.anonPoints, run.partnerPoints, run.flags.count("wing_story") > 0);
                                bool wasUnlocked = routesUnlocked;
                                endingsSeen[ending] = true;
                                endingTitle = endingTitles[ending];
                                justUnlocked = !wasUnlocked && (CountTrue(endingsSeen, ENDING_COUNT) == ENDING_COUNT);
                            } else {
                                // extra route: good if both scores are decent
                                bool good = (run.anonPoints >= 2 && run.partnerPoints >= 2);
                                routesDone[run.route] = true;
                                endingTitle = std::string(routeNames[run.route]) + (good ? ": Together" : ": Not This Time");
                                justUnlocked = false;
                            }
                            SaveProgress(endingsSeen, routesDone);    // write save.txt
                            std::remove(RUN_SAVE_FILE);               // the run is over: nothing to continue
                            continueAvailable = false;
                            state = ENDING;
                        }
                    }

                    if (state == EXPLORING) {
                        // the run goes on: autosave after every finished scene
                        if (SaveRun(run, world)) {
                            toastText = "Autosaved";
                            toastTimer = 2.0f;
                            continueAvailable = true;
                        }
                    }
                }
            }
        }
        else if (state == PAUSED) {
            if (pauseScreen == 0) {
                const int PAUSE_ITEMS = 6;               // Resume, Stats, Save, Load, Debug scores, Back to menu
                if (IsKeyPressed(KEY_W)) pauseIndex = (pauseIndex + PAUSE_ITEMS - 1) % PAUSE_ITEMS;
                if (IsKeyPressed(KEY_S)) pauseIndex = (pauseIndex + 1) % PAUSE_ITEMS;
                if (IsKeyPressed(KEY_ESCAPE)) {
                    state = EXPLORING;
                }
                else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) {
                    if (pauseIndex == 0) {
                        state = EXPLORING;
                    }
                    else if (pauseIndex == 1) {
                        pauseScreen = 1;
                    }
                    else if (pauseIndex == 2) {
                        RefreshSlots();
                        slotIndex = 0;
                        pauseScreen = 2;
                    }
                    else if (pauseIndex == 3) {
                        RefreshSlots();
                        slotIndex = 0;
                        loadReturn = PAUSED;
                        state = LOADSCREEN;
                    }
                    else if (pauseIndex == 4) {
                        showScores = !showScores;          // the debug scores option
                        SaveDebugScores(showScores);
                    }
                    else {
                        // back to the main menu: the run is autosaved first so Continue picks up right here
                        if (SaveRun(run, world)) continueAvailable = CheckSave(world, story, saveNote);
                        menuIndex = continueAvailable ? 0 : 2;
                        state = MENU;
                    }
                }
            }
            else if (pauseScreen == 1) {
                if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER)) pauseScreen = 0;
            }
            else {
                if (IsKeyPressed(KEY_W)) slotIndex = (slotIndex + SLOT_COUNT - 1) % SLOT_COUNT;
                if (IsKeyPressed(KEY_S)) slotIndex = (slotIndex + 1) % SLOT_COUNT;
                if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_ESCAPE)) {
                    pauseScreen = 0;
                }
                else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) {
                    if (SaveRun(run, world, SlotFile(slotIndex + 1))) {
                        toastText = "Saved to slot " + std::to_string(slotIndex + 1);
                        RefreshSlots();
                    } else {
                        toastText = "Could not write the save file";
                    }
                    toastTimer = 2.0f;
                }
            }
        }
        else if (state == LOADSCREEN) {
            const int ENTRIES = SLOT_COUNT + 1;          // the autosave + the three slots
            if (IsKeyPressed(KEY_W)) slotIndex = (slotIndex + ENTRIES - 1) % ENTRIES;
            if (IsKeyPressed(KEY_S)) slotIndex = (slotIndex + 1) % ENTRIES;
            if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_ESCAPE)) {
                state = loadReturn;
            }
            else if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) && slotOk[slotIndex]) {
                Run loaded;
                std::string loadError;
                std::string path = (slotIndex == 0) ? std::string(RUN_SAVE_FILE) : SlotFile(slotIndex);
                if (LoadRun(loaded, world, story, loadError, path)) {
                    run = loaded;
                    activeScene = -1;
                    state = EXPLORING;
                    toastText = "Loaded";
                    toastTimer = 2.0f;
                }
            }
        }
        else if (state == ENDING) {
            if (IsKeyPressed(KEY_ENTER)) {
                menuIndex = continueAvailable ? 0 : 2;
                state = MENU;
            }
        }

        // ---------------- drawing ----------------
        BeginDrawing();
        ClearBackground(BLACK);

        if (state == EXPLORING || state == DIALOGUE || state == PAUSED || (state == LOADSCREEN && loadReturn == PAUSED)) {
            const Map& map = world.maps[run.map];
            Vector2 playerCenter = { run.position.x + playerSize / 2, run.position.y + playerSize / 2 };
            int nearScene = NearestScene(run, playerCenter, 48, false);

            // --- the camera follows the player, but never shows past the edge of the map ---
            float mapWidth = (float)(map.rows[0].size() * TILE);
            float mapHeight = (float)(map.rows.size() * TILE);

            Camera2D camera = {};
            camera.offset = { 400, 224 };            // the middle of the screen
            camera.target = playerCenter;
            camera.rotation = 0;
            camera.zoom = 1;

            if (mapWidth <= 800) camera.target.x = mapWidth / 2;                       // small map: centre it
            else camera.target.x = Clamp(camera.target.x, 400, mapWidth - 400);
            if (mapHeight <= 448) camera.target.y = mapHeight / 2;
            else camera.target.y = Clamp(camera.target.y, 224, mapHeight - 224);

            BeginMode2D(camera);

            // tiles
            for (int y = 0; y < (int)map.rows.size(); y++) {
                for (int x = 0; x < (int)map.rows[y].size(); x++) {
                    DrawRectangle(x * TILE, y * TILE, TILE, TILE, TileColor(map.rows[y][x]));
                }
            }

            // doors
            for (size_t i = 0; i < map.warps.size(); i++) {
                DrawRectangle(map.warps[i].x * TILE + 4, map.warps[i].y * TILE + 4, 24, 24, GOLD);
            }

            // characters waiting for you on this map
            for (int i = 0; i < (int)run.scenes.size(); i++) {
                const Scene& s = run.scenes[i];
                if (!IsPending(run, s) || s.map != run.map) continue;

                int px = s.tileX * TILE + 4;
                int py = s.tileY * TILE + 4;
                if (InCast(cast, s.npc)) {
                    DrawRectangle(px, py, 24, 24, ColorOf(cast, s.npc));
                    DrawText(s.npc.c_str(), px - 4, py - 16, 14, BLACK);
                } else {
                    // an event spot with nobody standing on it
                    DrawRectangleLines(s.tileX * TILE + 2, s.tileY * TILE + 2, 28, 28, YELLOW);
                }
                DrawText("!", px + 8, py - 38, 22, MAROON);
                if (i == nearScene && state == EXPLORING) {
                    DrawText("Press E", px - 12, py + 30, 16, BLACK);
                }
            }

            // background characters (they only chat; E when you are close)
            int nearCrowd = (state == EXPLORING && nearScene == -1) ? NearestCrowd(world, run.map, playerCenter, 44) : -1;
            for (size_t i = 0; i < world.crowd.size(); i++) {
                const CrowdMember& c = world.crowd[i];
                if (c.map != run.map) continue;
                int px = c.tileX * TILE + 4;
                int py = c.tileY * TILE + 4;
                DrawRectangle(px, py, 24, 24, c.color);
                DrawRectangleLines(px, py, 24, 24, Fade(BLACK, 0.5f));
                if ((int)i == nearCrowd) DrawText("E: talk", px - 8, py + 28, 14, BLACK);
            }

            DrawRectangleV(run.position, { playerSize, playerSize }, BLUE);

            // --- debug overlay, part 1: things drawn in the world (F3) ---
            if (debugMode) {
                int cols = (int)map.rows[0].size();
                int rowCount = (int)map.rows.size();

                // grid lines
                for (int x = 0; x <= cols; x++) DrawLine(x * TILE, 0, x * TILE, rowCount * TILE, Fade(WHITE, 0.25f));
                for (int y = 0; y <= rowCount; y++) DrawLine(0, y * TILE, cols * TILE, y * TILE, Fade(WHITE, 0.25f));

                // collision boxes: every tile that blocks you
                for (int y = 0; y < rowCount; y++) {
                    for (int x = 0; x < cols; x++) {
                        if (IsSolidTile(map.rows[y][x])) DrawRectangleLines(x * TILE, y * TILE, TILE, TILE, RED);
                    }
                }

                // doors
                for (size_t i = 0; i < map.warps.size(); i++) {
                    DrawRectangleLines(map.warps[i].x * TILE, map.warps[i].y * TILE, TILE, TILE, GOLD);
                }

                // the talk range around every waiting character
                for (size_t i = 0; i < run.scenes.size(); i++) {
                    const Scene& s = run.scenes[i];
                    if (!IsPending(run, s) || s.map != run.map) continue;
                    DrawCircleLines(s.tileX * TILE + 16, s.tileY * TILE + 16, s.autoStart ? TRIGGER_RANGE : 48, s.autoStart ? ORANGE : SKYBLUE);
                }

                // the player's collision box and talk range
                DrawRectangleLines((int)run.position.x, (int)run.position.y, (int)playerSize, (int)playerSize, LIME);
                DrawCircleLines((int)playerCenter.x, (int)playerCenter.y, 48, LIME);
            }

            EndMode2D();

            // --- the HUD (stays on screen, not part of the world) ---
            DrawRectangle(0, 0, 800, 32, Fade(BLACK, 0.6f));
            DrawRectangle(0, 416, 800, 32, Fade(BLACK, 0.6f));

            if (SHOW_POINTS) {
                if (showScores) DrawText(TextFormat("Anon: %d   %s: %d", run.anonPoints, PartnerName(run.route), run.partnerPoints), 10, 8, 20, WHITE);
            }

            DrawText(TextFormat("Endings: %d/%d", CountTrue(endingsSeen, ENDING_COUNT), ENDING_COUNT), 10, 424, 20, WHITE);
            int shownChapter = (run.chapter > LastChapter(run)) ? LastChapter(run) : run.chapter;
            DrawText(TextFormat("Chapter %d/%d", shownChapter, LastChapter(run)), 170, 424, 20, WHITE);
            DrawText(PrettyName(map.name).c_str(), 320, 424, 20, SKYBLUE);
            if (run.route != -1) {
                DrawText(routeNames[run.route], 580, 424, 20, YELLOW);
            }
            DrawText("F5 save", 730, 428, 14, GRAY);

            if (toastTimer > 0) {
                DrawText(toastText.c_str(), 330, 392, 18, GREEN);
            }

            // objectives: who is waiting for you, and where
            int waiting = PendingScenes(run);
            if (waiting > 0) {
                DrawRectangle(0, 32, 340, 12 + waiting * 20, Fade(BLACK, 0.5f));
                int row = 0;
                for (size_t i = 0; i < run.scenes.size(); i++) {
                    const Scene& s = run.scenes[i];
                    if (!IsPending(run, s)) continue;

                    std::string who = (s.npc == "Narrator") ? "Something is up" : s.npc;
                    std::string text = "! " + who + " - " + PrettyName(world.maps[s.map].name);
                    if (s.map == run.map) text += "  (here)";
                    DrawText(text.c_str(), 10, 40 + row * 20, 16, WHITE);
                    row++;
                }
            }

            // --- debug overlay, part 2: the info panel ---
            if (debugMode) {
                int doneScenes = 0;
                int routeScenes = 0;
                for (size_t i = 0; i < run.scenes.size(); i++) {
                    if (run.scenes[i].route != run.route) continue;
                    routeScenes++;
                    if (run.scenes[i].done) doneScenes++;
                }
                int tileX = (int)(playerCenter.x / TILE);
                int tileY = (int)(playerCenter.y / TILE);

                std::string flagList;
                for (const std::string& f : run.flags) {
                    if (!flagList.empty()) flagList += ", ";
                    flagList += f;
                }
                if (flagList.empty()) flagList = "(none)";
                if (flagList.size() > 34) flagList = flagList.substr(0, 31) + "...";

                DrawRectangle(520, 36, 280, 148, Fade(BLACK, 0.75f));
                DrawText("DEBUG  (F3 to hide)", 528, 42, 14, YELLOW);
                DrawText(TextFormat("FPS: %d", GetFPS()), 528, 60, 14, WHITE);
                DrawText(TextFormat("map: %s (#%d)", map.name.c_str(), run.map), 528, 76, 14, WHITE);
                DrawText(TextFormat("tile: (%d, %d)   pixels: (%.0f, %.0f)", tileX, tileY, run.position.x, run.position.y), 528, 92, 14, WHITE);
                DrawText(TextFormat("route: %d   chapter: %d/%d", run.route, run.chapter, LastChapter(run)), 528, 108, 14, WHITE);
                DrawText(TextFormat("anon: %d   partner: %d", run.anonPoints, run.partnerPoints), 528, 124, 14, WHITE);
                DrawText(TextFormat("scenes done: %d/%d   waiting: %d", doneScenes, routeScenes, PendingScenes(run)), 528, 140, 14, WHITE);
                DrawText(TextFormat("flags: %s", flagList.c_str()), 528, 156, 14, WHITE);
            }
        }

        if (state == DIALOGUE) {
            Line& line = activeLines[currentLine];
            bool finishedTyping = (typed >= (float)Utf8Length(line.text));
            bool isQuestion = !line.choices.empty();

            bool hasName = (line.speaker != "Narrator");   // narration has no name label

            // the whole line cut into rows that fit the box; the box grows with the number of rows
            std::vector<std::string> rows = WrapText(line.text, 22, 720);
            int rowHeight = 26;
            int textTop = hasName ? 50 : 16;                // distance from the top of the box to the first row
            int answersTop = textTop + (int)rows.size() * rowHeight + 8;
            // a long answer needs several rows too: cut every answer to fit, and count the rows for the box height
            std::vector<std::vector<std::string> > answerRows;
            int answerRowCount = 0;
            if (isQuestion) {
                for (size_t i = 0; i < line.choices.size(); i++) {
                    bool available = ConditionMet(run, line.choices[i].requirement);
                    std::string label = "  " + line.choices[i].text + (available ? "" : " (locked)");   // 2 spaces = room for the "> " marker
                    answerRows.push_back(WrapText(label, 20, 690));
                    answerRowCount += (int)answerRows.back().size();
                }
            }
            int boxH = answersTop + answerRowCount * 22 + 26;   // 26 = room for the SPACE hint
            int boxY = 428 - boxH;

            DrawRectangle(20, boxY, 760, boxH, BLACK);
            DrawRectangleLines(20, boxY, 760, boxH, WHITE);
            if (hasName) DrawText(line.speaker.c_str(), 40, boxY + 10, 24, YELLOW);

            // only the part that has been typed so far (each row break used up one space)
            int budget = (int)typed;
            for (size_t r = 0; r < rows.size() && budget > 0; r++) {
                int rowLength = Utf8Length(rows[r]);
                int take = (budget < rowLength) ? budget : rowLength;
                std::string part = rows[r].substr(0, Utf8Prefix(rows[r], take));
                DrawText(part.c_str(), 40, boxY + textTop + (int)r * rowHeight, 22, WHITE);
                budget -= rowLength + 1;
            }

            // the speaker's picture: a PNG from images/ if there is one, otherwise a coloured square
            const Texture2D* picture = FindPortrait(portraits, line.speaker, line.expression);
            if (picture != nullptr) {
                float maxHeight = (float)boxY - 8.0f;            // never taller than the room above the text box
                if (maxHeight > 300.0f) maxHeight = 300.0f;
                float scale = maxHeight / (float)picture->height;
                if ((float)picture->width * scale > 260.0f) scale = 260.0f / (float)picture->width;
                float w = (float)picture->width * scale;
                float h = (float)picture->height * scale;
                Rectangle source = { 0.0f, 0.0f, (float)picture->width, (float)picture->height };
                Rectangle target = { 30.0f, (float)boxY - h - 4.0f, w, h };
                DrawTexturePro(*picture, source, target, { 0.0f, 0.0f }, 0.0f, WHITE);
            } else if (InCast(cast, line.speaker)) {
                int portraitSize = 96;
                int portraitX = 30;
                int portraitY = boxY - portraitSize - 8;
                DrawRectangle(portraitX, portraitY, portraitSize, portraitSize, ColorOf(cast, line.speaker));
                DrawRectangleLines(portraitX, portraitY, portraitSize, portraitSize, WHITE);
                DrawText(line.speaker.substr(0, 1).c_str(), portraitX + 34, portraitY + 20, 48, WHITE);
                if (!line.expression.empty()) {
                    DrawText(line.expression.c_str(), portraitX + 6, portraitY + portraitSize - 20, 16, WHITE);
                }
            }

            if (finishedTyping && isQuestion) {
                int answerY = boxY + answersTop;
                for (int i = 0; i < (int)line.choices.size(); i++) {
                    bool available = ConditionMet(run, line.choices[i].requirement);
                    Color c = available ? ((i == selected) ? YELLOW : WHITE) : GRAY;
                    std::vector<std::string> pieces = answerRows[i];
                    if (i == selected && available) pieces[0].replace(0, 2, "> ");      // the marker takes the place of the 2 spaces
                    for (size_t r = 0; r < pieces.size(); r++) {
                        DrawText(pieces[r].c_str(), (r == 0) ? 40 : 56, answerY, 20, c);   // later rows are indented
                        answerY += 22;
                    }
                }
                DrawText("W/S choose   SPACE confirm", 520, 406, 16, GRAY);
            } else if (finishedTyping) {
                DrawText("SPACE >", 690, 402, 16, GRAY);
            }
        }

        if (state == ENDING) {
            DrawText(endingTitle.c_str(), 60, 90, 40, YELLOW);
            if (showScores) DrawText(TextFormat("Anon: %d    %s: %d", run.anonPoints, PartnerName(run.route), run.partnerPoints), 60, 150, 24, WHITE);
            DrawText(TextFormat("Endings seen: %d / %d", CountTrue(endingsSeen, ENDING_COUNT), ENDING_COUNT), 60, 190, 24, WHITE);

            // the notification: this run just completed all 4 original endings
            if (justUnlocked) {
                DrawText("NEW! You can now partner with a different girl.", 60, 250, 24, GREEN);
                DrawText("Open Extra Routes from the main menu.", 60, 285, 22, GREEN);
            }

            DrawText("Press ENTER to go back to the menu", 60, 360, 20, GRAY);
        }

        if (state == PAUSED) {
            DrawRectangle(0, 0, 800, 448, Fade(BLACK, 0.78f));
            if (pauseScreen == 0) {
                std::string pauseItems[6] = { "Resume", "Stats", "Save", "Load", showScores ? "Debug scores: on" : "Debug scores: off", "Back to menu" };
                DrawText("Paused", 60, 50, 40, YELLOW);
                for (int i = 0; i < 6; i++) {
                    std::string label = (i == pauseIndex ? "> " : "  ") + pauseItems[i];
                    DrawText(label.c_str(), 60, 120 + i * 40, 28, (i == pauseIndex) ? YELLOW : WHITE);
                }
                DrawText("W/S choose   ENTER select   ESC resume", 60, 390, 18, GRAY);
            }
            else if (pauseScreen == 1) {
                int done = 0;
                for (size_t i = 0; i < run.scenes.size(); i++) if (run.scenes[i].done) done++;
                std::string story_name = (run.route == -1) ? "Original story" : std::string(routeNames[run.route]);
                DrawText("Stats", 60, 50, 40, YELLOW);
                DrawText(TextFormat("Story: %s", story_name.c_str()), 60, 120, 24, WHITE);
                DrawText(TextFormat("Chapter: %d / %d", run.chapter, LastChapter(run)), 60, 155, 24, WHITE);
                DrawText(TextFormat("Place: %s", PrettyName(world.maps[run.map].name).c_str()), 60, 190, 24, WHITE);
                if (showScores) {
                    DrawText(TextFormat("Anon: %d      %s: %d", run.anonPoints, PartnerName(run.route), run.partnerPoints), 60, 235, 28, SKYBLUE);
                } else {
                    DrawText("Scores are hidden (F4 or the pause menu turns them on)", 60, 241, 18, GRAY);
                }
                DrawText(TextFormat("Scenes done: %d / %d", done, (int)run.scenes.size()), 60, 285, 22, WHITE);
                DrawText(TextFormat("Endings seen: %d / %d", CountTrue(endingsSeen, ENDING_COUNT), ENDING_COUNT), 60, 318, 22, WHITE);
                DrawText("BACKSPACE: back", 60, 390, 18, GRAY);
            }
            else {
                DrawText("Save", 60, 50, 40, YELLOW);
                for (int i = 0; i < SLOT_COUNT; i++) {
                    std::string label = (i == slotIndex ? "> " : "  ") + std::string("Slot ") + std::to_string(i + 1);
                    DrawText(label.c_str(), 60, 120 + i * 62, 28, (i == slotIndex) ? YELLOW : WHITE);
                    DrawText(slotNote[i + 1].c_str(), 96, 152 + i * 62, 16, slotOk[i + 1] ? SKYBLUE : GRAY);
                }
                DrawText("W/S choose   ENTER save here (overwrites)   BACKSPACE back", 60, 390, 18, GRAY);
            }
        }

        if (state == LOADSCREEN) {
            DrawRectangle(0, 0, 800, 448, (loadReturn == PAUSED) ? Fade(BLACK, 0.78f) : BLACK);
            DrawText("Load Game", 60, 40, 40, YELLOW);
            for (int i = 0; i <= SLOT_COUNT; i++) {
                std::string title = (i == 0) ? "Autosave" : "Slot " + std::to_string(i);
                std::string label = (i == slotIndex ? "> " : "  ") + title;
                Color c = !slotOk[i] ? GRAY : ((i == slotIndex) ? YELLOW : WHITE);
                DrawText(label.c_str(), 60, 105 + i * 58, 28, c);
                DrawText(slotNote[i].c_str(), 96, 137 + i * 58, 16, slotOk[i] ? SKYBLUE : GRAY);
            }
            DrawText("W/S choose   ENTER load   BACKSPACE back", 60, 390, 18, GRAY);
        }

        if (state == MENU) {
            bool anyRouteDone = false;
            for (int r = 0; r < ROUTE_COUNT; r++) {
                if (routesDone[r]) anyRouteDone = true;
            }

            DrawText("My Game", 60, 60, 50, YELLOW);

            for (int i = 0; i < MENU_COUNT; i++) {
                Color c = WHITE;
                std::string label = (i == menuIndex ? "> " : "  ") + std::string(menuItems[i]);

                if (i == 0 && !continueAvailable) {
                    c = GRAY;
                    label += " (no save)";
                }
                else if (i == 3 && !routesUnlocked) {
                    c = GRAY;
                    label += " (locked)";
                }
                else if (i == 3 && !anyRouteDone) {
                    c = GREEN;
                    label += " (NEW!)";
                }
                if (i == menuIndex && !(i == 0 && !continueAvailable)) c = YELLOW;

                DrawText(label.c_str(), 60, 120 + i * 33, 28, c);
            }

            // what the saved run is (or why it can't be used)
            if (!saveNote.empty()) {
                DrawText(saveNote.c_str(), 60, 120 + 6 * 33 + 2, 16, continueAvailable ? SKYBLUE : ORANGE);
            }

            if (!storyError.empty()) {
                DrawText(storyError.c_str(), 60, 344, 16, RED);
            }

            DrawText(TextFormat("Endings: %d/%d", CountTrue(endingsSeen, ENDING_COUNT), ENDING_COUNT), 60, 368, 20, WHITE);
            DrawText("W/S choose   ENTER select   R: reload story, world and crowd files", 60, 398, 18, GRAY);
            DrawText(TextFormat("F4: debug scores (%s)", showScores ? "on" : "off"), 60, 421, 16, GRAY);
        }

        if (state == ROUTES) {
            int seenCount = CountTrue(endingsSeen, ENDING_COUNT);

            DrawText("Extra Routes", 60, 50, 40, YELLOW);

            if (!routesUnlocked) {
                DrawText("Locked.", 60, 140, 28, WHITE);
                DrawText(TextFormat("See all %d endings to unlock (%d/%d).", ENDING_COUNT, seenCount, ENDING_COUNT), 60, 185, 22, GRAY);
            } else {
                for (int i = 0; i < ROUTE_COUNT; i++) {
                    Color c = (i == routeIndex) ? YELLOW : WHITE;
                    std::string label = (i == routeIndex ? "> " : "  ") + std::string(routeNames[i]);
                    if (routesDone[i]) label += " (done)";
                    if (!RouteHasStory(story, i)) {
                        label += " (coming soon)";
                        if (i != routeIndex) c = GRAY;
                    }
                    DrawText(label.c_str(), 60, 140 + i * 40, 28, c);
                }
                DrawText(routeBlurbs[routeIndex], 60, 260, 20, WHITE);
                DrawText("W/S choose   ENTER start", 60, 340, 18, GRAY);
            }

            DrawText("BACKSPACE: back", 60, 390, 18, GRAY);
        }

        if (state == CREDITS) {
            DrawText("Credits", 60, 50, 40, YELLOW);
            DrawText("Remake based on Snoot Game by Cavemanon", 60, 120, 22, WHITE);
            DrawText("Story text and characters come from the original game script", 60, 150, 20, WHITE);
            DrawText("(code: AGPL v3, assets: CC BY-SA 4.0)", 60, 176, 20, GRAY);
            DrawText("Characters from Goodbye Volcano High by KO_OP", 60, 210, 22, WHITE);
            DrawText("Music: credit the Snoot Game composers here", 60, 258, 22, WHITE);
            DrawText("Made with C++ and raylib. Add your own team below!", 60, 306, 22, WHITE);
            DrawText("BACKSPACE: back", 60, 390, 18, GRAY);
        }

        EndDrawing();
    }

    for (std::map<std::string, Texture2D>::iterator it = portraits.loaded.begin(); it != portraits.loaded.end(); ++it) {
        UnloadTexture(it->second);
    }
    CloseWindow();
    return 0;
}
