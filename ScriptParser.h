// ScriptParser.h - Declares the file loader for story scripts
#pragma once
#include "SnootEngine.h"
#include <map>
#include <string>

// Parses a story file into a map of scene_id -> Scene.
bool LoadStoryFromFile(const std::string& filename,
                       std::map<std::string, Scene>& outScenes,
                       std::string& errorMessage);