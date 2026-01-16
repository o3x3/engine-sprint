#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>

// ============================================================================
// ENUMS & CONSTANTS
// ============================================================================
enum class ActionType { NONE, MOVE, ROTATE, INTERACT, SPAWN, STOP };
enum class EntityState { IDLE, MOVING, ROTATING };

// ============================================================================
// DATA STRUCTURES
// ============================================================================

// Day 5: Action Abstraction
// Defines "What" happened, not "How" it happened.
struct Action {
    ActionType type;
    glm::vec3 direction; // Local direction (e.g., 0,0,-1 for forward)
    float magnitude;

    Action(ActionType t = ActionType::NONE, glm::vec3 dir = glm::vec3(0), float mag = 0)
        : type(t), direction(dir), magnitude(mag) {
    }
};

// Day 3 & 8: Extensible Entity Structure
struct Entity {
    int id;
    std::string name;

    // Transform
    glm::vec3 position;
    glm::vec3 velocity;
    glm::vec3 acceleration;
    float rotation; // Degrees around Y-axis
    glm::vec3 scale;

    // Physics Properties
    float radius = 0.5f;
    bool isStatic = false;
    bool useGravity = true;

    // Visual Properties
    glm::vec3 color;
    EntityState state;

    // Constraints
    glm::vec3 boundaryMin;
    glm::vec3 boundaryMax;

    Entity(int _id, std::string _name, glm::vec3 _pos)
        : id(_id), name(_name), position(_pos), velocity(0.0f), acceleration(0.0f),
        rotation(0.0f), scale(0.5f), color(1.0f), state(EntityState::IDLE),
        boundaryMin(-15.0f, -5.0f, -15.0f), boundaryMax(15.0f, 50.0f, 15.0f) {
    }
};