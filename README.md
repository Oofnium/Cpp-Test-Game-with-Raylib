# C++ Visual Novel & Game Engine

A lightweight 2D game engine and narrative engine built from scratch in **C++** using the **Raylib** library[cite: 16, 20]. Designed as a modular system supporting custom script parsing, map exploration, dialogue trees with typewriter effects, and robust save/load management.

## 🚀 Features
* **Custom Script Parser:** Interprets narrative scripts (`story.txt`) containing scenes, choices, conditional requirements, and character expressions[cite: 21].
* **Tile-Based World System:** Grid-based movement, collision detection, and map-to-map transitions (`world.txt`)[cite: 16].
* **State & Narrative Management:** Tracks player stats, choices, and story progression flags independently from rendering logic (`SnootEngine.h`, `Narrative.cpp`)[cite: 17, 20].
* **Save/Load System:** Supports automated checkpoints, manual save slots, and persistent settings.

## 📂 Project Structure
* `main.cpp` - Main game loop, rendering, input handling, and state machine[cite: 16].
* `SnootEngine.h` - Core data models for narrative states, choices, and requirements[cite: 20].
* `ScriptParser.cpp` / `.h` - File loaders for script reading and syntax parsing[cite: 18, 19].
* `Narrative.cpp` - Stat tracking, condition evaluation, and ending logic[cite: 17].
* `story.txt` - Dialogue scripts and scene triggers[cite: 21].

## 🛠️ Tech Stack
* **Language:** C++
* **Graphics Library:** Raylib[cite: 16]
* **OS Tested:** Linux (Fedora)

## ⚙️ Getting Started
1. Clone the repository.
2. Ensure you have Raylib installed on your system.
3. Compile using g++ or your preferred IDE:
   
   g++ main_3.cpp Narrative.cpp ScriptParser.cpp -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
