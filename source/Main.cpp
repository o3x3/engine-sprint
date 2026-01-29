#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>
#include <IXNetSystem.h>
#include <IXWebSocket.h>
#include <IXUserAgent.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include <functional>
#include <filesystem> 

// --- 1. OBJ PARSER ---
namespace Primitives { struct Vertex { glm::vec3 position; glm::vec2 texCoord; glm::vec3 normal; }; }
struct IndexTriplet { int v, vt, vn; bool operator==(IndexTriplet const& o) const { return v == o.v && vt == o.vt && vn == o.vn; } };
struct IndexTripletHash { std::size_t operator()(IndexTriplet const& k) const noexcept { return ((std::size_t)k.v * 73856093u) ^ ((std::size_t)k.vt * 19349663u) ^ ((std::size_t)k.vn * 83492791u); } };
void parseObj(const std::string& filePath, std::vector<Primitives::Vertex>& out_vertices, std::vector<unsigned int>& out_indices) {
    std::vector<glm::vec3> temp_positions; std::vector<glm::vec2> temp_tex_coords; std::vector<glm::vec3> temp_normals;
    out_vertices.clear(); out_indices.clear(); std::ifstream fileStream(filePath); if (!fileStream.is_open()) return;
    std::unordered_map<IndexTriplet, unsigned int, IndexTripletHash> indexMap; std::string line;
    while (std::getline(fileStream, line)) {
        if (line.empty()) continue; std::stringstream ss(line); std::string prefix; ss >> prefix;
        if (prefix == "v") { glm::vec3 p; ss >> p.x >> p.y >> p.z; temp_positions.push_back(p); }
        else if (prefix == "vt") { glm::vec2 t; ss >> t.x >> t.y; temp_tex_coords.push_back(t); }
        else if (prefix == "vn") { glm::vec3 n; ss >> n.x >> n.y >> n.z; temp_normals.push_back(n); }
        else if (prefix == "f") {
            std::string vertStr; std::vector<unsigned int> faceIndices;
            while (ss >> vertStr) {
                int vi = 0, vti = 0, vni = 0; std::stringstream vss(vertStr); vss >> vi;
                if (vss.peek() == '/') { vss.ignore(); if (vss.peek() != '/') vss >> vti; } if (vss.peek() == '/') { vss.ignore(); vss >> vni; }
                IndexTriplet key{ vi, vti, vni }; if (indexMap.count(key)) faceIndices.push_back(indexMap[key]);
                else {
                    Primitives::Vertex vx; vx.position = (vi > 0 && vi <= temp_positions.size()) ? temp_positions[vi - 1] : glm::vec3(0.0f);
                    vx.texCoord = (vti > 0 && vti <= temp_tex_coords.size()) ? temp_tex_coords[vti - 1] : glm::vec2(0.0f);
                    vx.normal = (vni > 0 && vni <= temp_normals.size()) ? temp_normals[vni - 1] : glm::vec3(0.0f);
                    unsigned int idx = static_cast<unsigned int>(out_vertices.size()); out_vertices.push_back(vx); indexMap[key] = idx; faceIndices.push_back(idx);
                }
            }
            for (size_t k = 1; k + 1 < faceIndices.size(); ++k) { out_indices.push_back(faceIndices[0]); out_indices.push_back(faceIndices[k]); out_indices.push_back(faceIndices[k + 1]); }
        }
    }
}

// --- 2. STRUCTS ---
using json = nlohmann::json;
namespace glm { void from_json(const json& j, vec3& v) { if (j.is_array() && j.size() >= 3) { v.x = j[0]; v.y = j[1]; v.z = j[2]; } else v = vec3(0.0f); } }
struct Transform { glm::vec3 position, rotation, scale; };
struct Material { std::string shader, texture; glm::vec3 color; };
struct Components { std::string mesh, collider, script; };
struct Entity { std::string id, type; Transform transform; Material material; Components components; };
struct SceneData { std::string id; glm::vec3 ambientLight; std::string skybox; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Transform, position, rotation, scale)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Material, shader, texture, color)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Components, mesh, collider, script)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Entity, id, type, transform, material, components)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SceneData, id, ambientLight, skybox)

enum JobType { JOB_SPAWN_ENTITY, JOB_DESTROY_ENTITY, JOB_UPDATE_TRANSFORM, JOB_BUILD_SCENE, JOB_LOAD_ASSETS };
struct Job { JobType type; json payload; };

// --- 3. NETWORK CLIENT ---
class NetworkClient {
public:
    ix::WebSocket webSocket;
    std::string url = "ws://localhost:8080/engine";
    std::function<void(std::string)> onMessageReceived;

    void init(std::function<void(std::string)> callback) {
        ix::initNetSystem();
        onMessageReceived = callback;
        webSocket.setUrl(url);
        webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Message && onMessageReceived) onMessageReceived(msg->str);
            else if (msg->type == ix::WebSocketMessageType::Open) send("{\"type\": \"status\", \"msg\": \"Engine Connected\"}");
            });
        webSocket.start();
    }
    void send(const std::string& msg) { webSocket.send(msg); }
    void cleanup() { webSocket.stop(); ix::uninitNetSystem(); }
};

// --- 4. ENGINE CLASS ---
glm::vec3 cameraPos = glm::vec3(0.0f, 2.0f, 10.0f);
glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
bool firstMouse = true; float yaw = -90.0f, pitch = 0.0f, lastX = 512.0f, lastY = 384.0f, fov = 45.0f;
struct MeshResource { unsigned int VAO, VBO, EBO; unsigned int indexCount; };
const char* vShader = R"(#version 330 core
layout (location=0) in vec3 aPos; layout (location=1) in vec2 aTex; layout (location=2) in vec3 aNor;
uniform mat4 model; uniform mat4 view; uniform mat4 projection;
out vec3 Normal; out vec3 FragPos;
void main() { gl_Position = projection * view * model * vec4(aPos, 1.0); FragPos = vec3(model * vec4(aPos, 1.0)); Normal = mat3(transpose(inverse(model))) * aNor; })";
const char* fShader = R"(#version 330 core
out vec4 FragColor; in vec3 Normal; in vec3 FragPos;
uniform vec3 uColor; uniform vec3 uLightColor;
void main() { vec3 ambient = 0.4 * uLightColor; vec3 norm = normalize(Normal); vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3)); float diff = max(dot(norm, lightDir), 0.0); vec3 diffuse = diff * uLightColor; vec3 result = (ambient + diffuse) * uColor; FragColor = vec4(result, 1.0); })";

void mouse_callback(GLFWwindow* window, double xpos, double ypos);

class Engine {
    GLFWwindow* window;
    unsigned int shaderProgram;
    SceneData currentScene;
    std::vector<Entity> entities;
    std::queue<Job> jobQueue;
    std::unordered_map<std::string, MeshResource> meshCache;
    std::function<void(std::string)> telemetrySender;
    int tickCount = 0;
    std::vector<json> jobHistory;

    // --- DEMO / REPLAY FLAGS ---
    bool isReplayMode = false;
    bool isDemoMode = false;
    float replayTimer = 0.0f;

public:
    int init(bool replay, bool demo) {
        isReplayMode = replay;
        isDemoMode = demo;

        if (!glfwInit()) return -1;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        // --- DAY 9: DEMO MODE (FULLSCREEN) ---
        if (isDemoMode) {
            GLFWmonitor* primary = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(primary);
            glfwWindowHint(GLFW_RED_BITS, mode->redBits);
            glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
            glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
            glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
            window = glfwCreateWindow(mode->width, mode->height, "TTG RUNTIME [LOCKED]", primary, NULL);
        }
        else {
            window = glfwCreateWindow(1024, 768, isReplayMode ? "Engine (REPLAY)" : "Engine (DEV)", NULL, NULL);
        }

        if (!window) return -1;
        glfwMakeContextCurrent(window);
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;
        glEnable(GL_DEPTH_TEST);

        currentScene.ambientLight = glm::vec3(0.1f, 0.1f, 0.1f); // Visible start
        return 0;
    }

    void loadShader() {
        unsigned int v = glCreateShader(GL_VERTEX_SHADER); glShaderSource(v, 1, &vShader, NULL); glCompileShader(v);
        unsigned int f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &fShader, NULL); glCompileShader(f);
        shaderProgram = glCreateProgram(); glAttachShader(shaderProgram, v); glAttachShader(shaderProgram, f); glLinkProgram(shaderProgram); glDeleteShader(v); glDeleteShader(f);
    }

    void setTelemetryCallback(std::function<void(std::string)> callback) { telemetrySender = callback; }
    void emit(const std::string& event, const json& data) {
        if (telemetrySender && !isReplayMode) {
            json packet; packet["type"] = "TELEMETRY"; packet["event"] = event; packet["data"] = data; packet["tick"] = tickCount;
            telemetrySender(packet.dump());
        }
    }

    std::string resolveMeshFromCollider(const std::string& colliderType) {
        if (colliderType == "box") return "assets/cube.txt";
        if (colliderType == "sphere") return "assets/sphere.txt";
        return "assets/cube.txt";
    }

    std::string findAssetPath(const std::string& filename) {
        if (std::filesystem::exists(filename)) return filename;
        std::string checkPath = "";
        for (int i = 0; i < 4; ++i) {
            checkPath = "../" + checkPath;
            std::string fullPath = checkPath + filename;
            if (std::filesystem::exists(fullPath)) return fullPath;
        }
        return filename;
    }

    void internalSpawnEntity(const Entity& ent) {
        std::string resourceKey = resolveMeshFromCollider(ent.components.collider);
        std::string actualPath = findAssetPath(resourceKey);

        if (meshCache.find(resourceKey) == meshCache.end()) {
            std::vector<Primitives::Vertex> vertices; std::vector<unsigned int> indices;
            parseObj(actualPath, vertices, indices);
            if (!vertices.empty()) {
                MeshResource res; glGenVertexArrays(1, &res.VAO); glGenBuffers(1, &res.VBO); glGenBuffers(1, &res.EBO);
                glBindVertexArray(res.VAO); glBindBuffer(GL_ARRAY_BUFFER, res.VBO); glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Primitives::Vertex), vertices.data(), GL_STATIC_DRAW);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, res.EBO); glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Primitives::Vertex), (void*)offsetof(Primitives::Vertex, position)); glEnableVertexAttribArray(0);
                glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Primitives::Vertex), (void*)offsetof(Primitives::Vertex, texCoord)); glEnableVertexAttribArray(1);
                glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Primitives::Vertex), (void*)offsetof(Primitives::Vertex, normal)); glEnableVertexAttribArray(2);
                res.indexCount = static_cast<unsigned int>(indices.size()); meshCache[resourceKey] = res;
            }
        }
        entities.push_back(ent);
        emit("entity_spawned", { {"id", ent.id} });
    }

    void pushJob(Job j) { jobQueue.push(j); }

    void parseNetworkJob(const std::string& jsonStr) {
        try {
            json j = json::parse(jsonStr);
            Job newJob; std::string typeStr = j["jobType"];
            if (typeStr == "SPAWN_ENTITIES") newJob.type = JOB_SPAWN_ENTITY;
            else if (typeStr == "BUILD_SCENE") newJob.type = JOB_BUILD_SCENE;
            else if (typeStr == "LOAD_ASSETS") newJob.type = JOB_LOAD_ASSETS;
            else if (typeStr == "UPDATE") newJob.type = JOB_UPDATE_TRANSFORM;
            else if (typeStr == "DESTROY") newJob.type = JOB_DESTROY_ENTITY;
            else return;

            newJob.payload = j["payload"];
            pushJob(newJob);
            if (!isReplayMode) jobHistory.push_back(j);
        }
        catch (...) {}
    }

    void loadReplayFile(const std::string& path) {
        std::string fullPath = findAssetPath(path);
        std::ifstream f(fullPath);
        if (!f.is_open()) return;
        try {
            json dump = json::parse(f);
            for (const auto& j : dump) {
                Job newJob; std::string typeStr = j["jobType"];
                if (typeStr == "SPAWN_ENTITIES") newJob.type = JOB_SPAWN_ENTITY;
                else if (typeStr == "BUILD_SCENE") newJob.type = JOB_BUILD_SCENE;
                else if (typeStr == "UPDATE") newJob.type = JOB_UPDATE_TRANSFORM;
                newJob.payload = j["payload"];
                pushJob(newJob);
            }
        }
        catch (...) {}
    }

    void dumpReplayToDisk() {
        if (isReplayMode || jobHistory.empty()) return;
        std::ofstream o("replay.json");
        o << json(jobHistory).dump(4);
    }

    void processJobs() {
        if (isReplayMode) {
            replayTimer += 1.0f / 60.0f;
            if (replayTimer >= 1.0f && !jobQueue.empty()) {
                replayTimer = 0.0f;
                Job j = jobQueue.front(); jobQueue.pop();
                executeJob(j);
            }
        }
        else {
            while (!jobQueue.empty()) {
                Job j = jobQueue.front(); jobQueue.pop();
                executeJob(j);
            }
        }
    }

    void executeJob(const Job& j) {
        switch (j.type) {
        case JOB_BUILD_SCENE: if (j.payload.contains("ambientLight")) currentScene.ambientLight = j.payload["ambientLight"]; break;
        case JOB_SPAWN_ENTITY: try { Entity newEnt = j.payload.get<Entity>(); internalSpawnEntity(newEnt); }
                             catch (...) {} break;
        case JOB_UPDATE_TRANSFORM: { std::string id = j.payload["id"]; for (auto& ent : entities) if (ent.id == id) { if (j.payload.contains("position")) ent.transform.position = j.payload["position"]; break; } break; }
        }
    }

    void recordRawString(const std::string& str) {
        if (!isReplayMode) { try { jobHistory.push_back(json::parse(str)); } catch (...) {} }
    }

    void run() {
        float lastFrame = 0.0f;
        while (!glfwWindowShouldClose(window)) {
            float currentFrame = static_cast<float>(glfwGetTime()); float deltaTime = currentFrame - lastFrame; lastFrame = currentFrame;
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, true);

            float cameraSpeed = 5.0f * deltaTime;
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) cameraPos += cameraSpeed * cameraFront;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) cameraPos -= cameraSpeed * cameraFront;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) cameraPos -= normalize(cross(cameraFront, cameraUp)) * cameraSpeed;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) cameraPos += normalize(cross(cameraFront, cameraUp)) * cameraSpeed;

            processJobs();
            tickCount++; if (tickCount % 60 == 0) emit("tick_update", { {"fps", 1.0f / deltaTime} });

            glm::vec3 bg = currentScene.ambientLight; glClearColor(bg.r * 0.2f, bg.g * 0.2f, bg.b * 0.2f, 1.0f); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glUseProgram(shaderProgram);

            // DYNAMIC ASPECT RATIO FOR DEMO MODE
            int width, height;
            glfwGetWindowSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glm::mat4 projection = glm::perspective(glm::radians(fov), (float)width / height, 0.1f, 100.0f);
            glm::mat4 view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);

            glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "view"), 1, GL_FALSE, glm::value_ptr(view)); glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "projection"), 1, GL_FALSE, glm::value_ptr(projection)); glUniform3fv(glGetUniformLocation(shaderProgram, "uLightColor"), 1, glm::value_ptr(currentScene.ambientLight));
            for (const auto& ent : entities) {
                std::string resourceKey = resolveMeshFromCollider(ent.components.collider); if (meshCache.find(resourceKey) == meshCache.end()) continue;
                MeshResource& mesh = meshCache[resourceKey]; glm::mat4 model = glm::mat4(1.0f);
                model = glm::translate(model, ent.transform.position); model = glm::rotate(model, glm::radians(ent.transform.rotation.y), glm::vec3(0, 1, 0)); model = glm::rotate(model, glm::radians(ent.transform.rotation.x), glm::vec3(1, 0, 0)); model = glm::rotate(model, glm::radians(ent.transform.rotation.z), glm::vec3(0, 0, 1)); model = glm::scale(model, ent.transform.scale);
                glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "model"), 1, GL_FALSE, glm::value_ptr(model)); glUniform3fv(glGetUniformLocation(shaderProgram, "uColor"), 1, glm::value_ptr(ent.material.color));
                glBindVertexArray(mesh.VAO); glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, 0);
            }
            glfwSwapBuffers(window); glfwPollEvents();
        }
        dumpReplayToDisk();
        glfwTerminate();
    }
};

void mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
    float xpos = static_cast<float>(xposIn); float ypos = static_cast<float>(yposIn);
    if (firstMouse) { lastX = xpos; lastY = ypos; firstMouse = false; }
    float xoffset = xpos - lastX; float yoffset = lastY - ypos; lastX = xpos; lastY = ypos; xoffset *= 0.1f; yoffset *= 0.1f;
    yaw += xoffset; pitch += yoffset; if (pitch > 89.0f) pitch = 89.0f; if (pitch < -89.0f) pitch = -89.0f;
    glm::vec3 front; front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch)); front.y = sin(glm::radians(pitch)); front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch)); cameraFront = glm::normalize(front);
}

std::string findAssetPath(const std::string& filename) {
    if (std::filesystem::exists(filename)) return filename;
    std::string checkPath = "";
    for (int i = 0; i < 4; ++i) {
        checkPath = "../" + checkPath;
        std::string fullPath = checkPath + filename;
        if (std::filesystem::exists(fullPath)) return fullPath;
    }
    return filename;
}

int main(int argc, char* argv[]) {
    Engine engine;

    // --- FLAG PARSING ---
    bool replayMode = (argc > 1 && std::string(argv[1]) == "--replay");
    bool demoMode = (argc > 1 && std::string(argv[1]) == "--demo");

    if (engine.init(replayMode, demoMode) != 0) return -1;
    NetworkClient netClient;

    if (replayMode) {
        engine.loadShader();
        std::string path = findAssetPath("replay.json");
        engine.loadReplayFile(path);
        engine.run();
    }
    else {
        engine.setTelemetryCallback([&netClient](std::string msg) { netClient.send(msg); });
        netClient.init([&engine](std::string msg) {
            engine.recordRawString(msg);
            engine.parseNetworkJob(msg);
            });
        engine.loadShader();
        engine.run();
        netClient.cleanup();
    }
    return 0;
}