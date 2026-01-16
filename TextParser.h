#pragma once
#include "Types.h"
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

class TextParser {
public:
    static Action ParseCommand(const std::string& input) {
        std::string lower = ToLower(input);
        std::vector<std::string> tokens = Tokenize(lower);

        if (tokens.empty()) return Action();
        std::string command = tokens[0];

        // Day 6: Mapping "move forward" -> Action Object
        if (command == "move" || command == "go") {
            if (tokens.size() > 1) {
                std::string direction = tokens[1];
                float magnitude = 15.0f; // Default speed

                // Optional: Check for custom magnitude
                if (tokens.size() > 2) {
                    try { magnitude = std::stof(tokens[2]); }
                    catch (...) {}
                }

                // NOTE: These are LOCAL vectors. Forward is -Z.
                if (direction == "forward") return Action(ActionType::MOVE, glm::vec3(0, 0, -1), magnitude);
                if (direction == "back")    return Action(ActionType::MOVE, glm::vec3(0, 0, 1), magnitude);
                if (direction == "left")    return Action(ActionType::MOVE, glm::vec3(-1, 0, 0), magnitude);
                if (direction == "right")   return Action(ActionType::MOVE, glm::vec3(1, 0, 0), magnitude);
                if (direction == "up")      return Action(ActionType::MOVE, glm::vec3(0, 1, 0), magnitude);
            }
        }

        if (command == "turn" || command == "rotate") {
            if (tokens.size() > 1) {
                std::string direction = tokens[1];
                float magnitude = 45.0f; // Degrees
                if (direction == "left")  return Action(ActionType::ROTATE, glm::vec3(0), magnitude);
                if (direction == "right") return Action(ActionType::ROTATE, glm::vec3(0), -magnitude);
            }
        }

        if (command == "stop")  return Action(ActionType::STOP);
        if (command == "spawn") return Action(ActionType::SPAWN);

        return Action(); // No-Op
    }

private:
    static std::string ToLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    static std::vector<std::string> Tokenize(const std::string& s) {
        std::vector<std::string> tokens;
        std::istringstream iss(s);
        std::string token;
        while (iss >> token) tokens.push_back(token);
        return tokens;
    }
};