#pragma once
#include "Types.h"
#include "TextParser.h"
#include <iostream>
#include <queue>
#include <glm/gtc/matrix_transform.hpp>

class GameEngine {
private:
    // Constants for Day 7 Deterministic Loop
    const float FIXED_TIMESTEP = 1.0f / 60.0f;
    const glm::vec3 GRAVITY = glm::vec3(0.0f, -9.81f, 0.0f);

    float accumulator = 0.0f;

public:
    std::vector<Entity> entities;
    std::queue<std::string> commandQueue;
    int nextEntityId = 0;
    int playerEntityId = -1;

    void Init() {
        // Create the main controllable entity
        Entity player(nextEntityId++, "player", glm::vec3(0.0f, 2.0f, 0.0f));
        player.color = glm::vec3(1.0f, 0.3f, 0.3f);
        playerEntityId = player.id;
        entities.push_back(player);
    }

    void AddCommand(const std::string& cmd) {
        commandQueue.push(cmd);
    }

    // Day 7: Deterministic Update Loop
    // Decouples physics steps from frame rate.
    void Update(float deltaTime) {
        // 1. Process Input (Once per frame is fine for text)
        ProcessCommands();

        // 2. Accumulate Time
        accumulator += deltaTime;

        // 3. Fixed Step Physics
        while (accumulator >= FIXED_TIMESTEP) {
            PhysicsStep(FIXED_TIMESTEP);
            accumulator -= FIXED_TIMESTEP;
        }
    }

private:
    void ProcessCommands() {
        if (commandQueue.empty()) return;

        std::string cmd = commandQueue.front();
        commandQueue.pop();

        Action action = TextParser::ParseCommand(cmd);

        // Execute Action on Player (if valid)
        if (action.type != ActionType::NONE && playerEntityId != -1) {
            Entity* player = GetEntity(playerEntityId);
            if (player) ExecuteAction(*player, action);
        }
    }

    // Day 5: Action Execution Layer
    void ExecuteAction(Entity& entity, const Action& action) {
        switch (action.type) {
        case ActionType::MOVE: {
            // Fix: Calculate World Direction based on Rotation
            glm::mat4 rotMat = glm::rotate(glm::mat4(1.0f), glm::radians(entity.rotation), glm::vec3(0, 1, 0));
            glm::vec3 worldDir = glm::vec3(rotMat * glm::vec4(action.direction, 0.0f));

            entity.velocity += worldDir * action.magnitude;
            entity.state = EntityState::MOVING;
            std::cout << "[ENGINE] Moving " << entity.name << std::endl;
            break;
        }
        case ActionType::ROTATE:
            entity.rotation += action.magnitude;
            entity.state = EntityState::ROTATING;
            std::cout << "[ENGINE] Rotating " << entity.name << std::endl;
            break;
        case ActionType::STOP:
            entity.velocity = glm::vec3(0.0f);
            entity.state = EntityState::IDLE;
            std::cout << "[ENGINE] Stopping " << entity.name << std::endl;
            break;
        case ActionType::SPAWN:
            SpawnRandomEntity(entity.position);
            break;
        default: break;
        }
    }

    void PhysicsStep(float dt) {
        for (Entity& e : entities) {
            if (e.isStatic) continue;

            // Gravity
            if (e.useGravity) e.velocity += GRAVITY * dt;

            // Integration
            e.position += e.velocity * dt;

            // Simple Constraints (Ground & Walls)
            if (e.position.y < e.radius) {
                e.position.y = e.radius;
                e.velocity.y = -e.velocity.y * 0.3f; // Bounce
                e.velocity.x *= 0.95f; // Ground friction
                e.velocity.z *= 0.95f;
            }

            // Boundary Clamp
            e.position = glm::clamp(e.position, e.boundaryMin, e.boundaryMax);

            // Idle Check
            if (glm::length(e.velocity) < 0.1f && e.position.y <= e.radius + 0.1f) {
                e.velocity = glm::vec3(0.0f);
                if (e.state == EntityState::MOVING) e.state = EntityState::IDLE;
            }
        }

        // Basic Collision Resolution
        ResolveCollisions();
    }

    void ResolveCollisions() {
        for (size_t i = 0; i < entities.size(); ++i) {
            for (size_t j = i + 1; j < entities.size(); ++j) {
                Entity& a = entities[i];
                Entity& b = entities[j];

                float dist = glm::distance(a.position, b.position);
                float minDist = a.radius + b.radius;

                if (dist < minDist) {
                    glm::vec3 dir = glm::normalize(a.position - b.position);
                    float overlap = minDist - dist;
                    if (!a.isStatic) a.position += dir * overlap * 0.5f;
                    if (!b.isStatic) b.position -= dir * overlap * 0.5f;
                }
            }
        }
    }

    void SpawnRandomEntity(glm::vec3 origin) {
        Entity e(nextEntityId++, "cube", origin + glm::vec3(2, 5, 0));
        e.color = glm::vec3(
            (float)rand() / RAND_MAX,
            (float)rand() / RAND_MAX,
            (float)rand() / RAND_MAX
        );
        entities.push_back(e);
        std::cout << "[ENGINE] Spawned Entity " << e.id << std::endl;
    }

    Entity* GetEntity(int id) {
        for (auto& e : entities) if (e.id == id) return &e;
        return nullptr;
    }
};