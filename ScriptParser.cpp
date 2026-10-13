// ScriptParser.cpp - Implements reading story.txt lines into Scene data structures
#include "ScriptParser.h"
#include <fstream>
#include <sstream>
#include <iostream>

static std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string Upper(std::string s) {
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] >= 'a' && s[i] <= 'z') s[i] = (char)(s[i] - 32);
    }
    return s;
}

static bool ParseChoiceLine(const std::string& line, Choice& outChoice, std::string& error) {
    outChoice = Choice();
    std::string work = Trim(line);

    if (work.empty() || work[0] != '?') {
        error = "Choice line must start with '?'";
        return false;
    }
    work = Trim(work.substr(1));

    if (work.rfind("[REQ:", 0) == 0) {
        size_t closePos = work.find(']');
        if (closePos == std::string::npos) {
            error = "Malformed [REQ: ...] tag in choice";
            return false;
        }
        std::string reqBody = work.substr(5, closePos - 5);
        if (!ParseRequirement(reqBody, outChoice.requirement, error)) {
            return false;
        }
        work = Trim(work.substr(closePos + 1));
    }

    std::stringstream ss(work);
    std::string segment;
    int index = 0;

    while (std::getline(ss, segment, '|')) {
        segment = Trim(segment);
        if (segment.empty()) continue;

        if (index == 0) {
            outChoice.text = segment;
        } else {
            std::string upperSeg = Upper(segment);
            if (upperSeg.rfind("FLAG:", 0) == 0) {
                outChoice.flagsToSet.push_back(Trim(segment.substr(5)));
            } else if (upperSeg.rfind("JUMP:", 0) == 0) {
                outChoice.jumpTo = Trim(segment.substr(5));
            } else {
                StatEffect eff;
                std::string effErr;
                if (ParseEffect(segment, eff, effErr)) {
                    outChoice.effects.push_back(eff);
                } else {
                    error = "Unrecognized choice segment '" + segment + "' (" + effErr + ")";
                    return false;
                }
            }
        }
        index++;
    }

    if (outChoice.text.empty()) {
        error = "Choice has no prompt text";
        return false;
    }
    return true;
}

bool LoadStoryFromFile(const std::string& filename,
                       std::map<std::string, Scene>& outScenes,
                       std::string& errorMessage) {
    outScenes.clear();
    std::ifstream file(filename);
    if (!file.is_open()) {
        errorMessage = "Failed to open script file: " + filename;
        return false;
    }

    std::string line;
    int lineNum = 0;
    Scene* currentScene = nullptr;

    while (std::getline(file, line)) {
        lineNum++;
        std::string trimmed = Trim(line);

        if (trimmed.empty() || trimmed.rfind("#", 0) == 0 || trimmed.rfind("//", 0) == 0) {
            continue;
        }

        if (trimmed.rfind("[SCENE:", 0) == 0 && trimmed.back() == ']') {
            std::string id = Trim(trimmed.substr(7, trimmed.size() - 8));
            if (id.empty()) {
                errorMessage = "Line " + std::to_string(lineNum) + ": Scene ID cannot be empty";
                return false;
            }
            outScenes[id] = Scene{ id, {} };
            currentScene = &outScenes[id];
            continue;
        }

        if (!currentScene) {
            errorMessage = "Line " + std::to_string(lineNum) + ": Content found outside [SCENE: ...] block";
            return false;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            std::string tagBody = trimmed.substr(1, trimmed.size() - 2);

            if (tagBody.rfind("BG:", 0) == 0) {
                Step s;
                s.type = StepType::Background;
                s.key = Trim(tagBody.substr(3));
                currentScene->steps.push_back(s);
            }
            else if (tagBody.rfind("SHOW:", 0) == 0) {
                std::string rest = Trim(tagBody.substr(5));
                std::istringstream iss(rest);
                std::string key, posStr;
                iss >> key >> posStr;

                Step s;
                s.type = StepType::Show;
                s.key = key;
                s.pos = SpritePos::Center;

                std::string upperPos = Upper(posStr);
                if (upperPos == "LEFT")        s.pos = SpritePos::Left;
                else if (upperPos == "RIGHT")  s.pos = SpritePos::Right;
                else if (upperPos == "CENTER") s.pos = SpritePos::Center;

                currentScene->steps.push_back(s);
            }
            else if (tagBody.rfind("MUSIC:", 0) == 0) {
                Step s;
                s.type = StepType::Music;
                s.key = Trim(tagBody.substr(6));
                currentScene->steps.push_back(s);
            }
            else if (Upper(tagBody) == "ENDING") {
                Step s;
                s.type = StepType::Ending;
                currentScene->steps.push_back(s);
            }
            else {
                errorMessage = "Line " + std::to_string(lineNum) + ": Unknown tag [" + tagBody + "]";
                return false;
            }
            continue;
        }

        if (trimmed.front() == '?') {
            Choice choice;
            std::string parseErr;
            if (!ParseChoiceLine(trimmed, choice, parseErr)) {
                errorMessage = "Line " + std::to_string(lineNum) + ": " + parseErr;
                return false;
            }

            if (!currentScene->steps.empty() && currentScene->steps.back().type == StepType::Choices) {
                currentScene->steps.back().choices.push_back(choice);
            } else {
                Step s;
                s.type = StepType::Choices;
                s.choices.push_back(choice);
                currentScene->steps.push_back(s);
            }
            continue;
        }

        size_t colonPos = trimmed.find(':');
        if (colonPos != std::string::npos) {
            Step s;
            s.type = StepType::Dialogue;
            s.speaker = Trim(trimmed.substr(0, colonPos));
            s.text = Trim(trimmed.substr(colonPos + 1));
            currentScene->steps.push_back(s);
        } else {
            errorMessage = "Line " + std::to_string(lineNum) + ": Unrecognized format '" + trimmed + "'";
            return false;
        }
    }

    return true;
}