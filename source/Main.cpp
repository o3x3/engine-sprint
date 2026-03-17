#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

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
#include <filesystem>
#include <mutex>

const int MAX_JOBS_PER_FRAME = 50;

// --- OBJ PARSER ---
namespace Primitives { struct Vertex { glm::vec3 position; glm::vec2 texCoord; glm::vec3 normal; }; }
struct IndexTriplet { int v, vt, vn; bool operator==(IndexTriplet const& o) const { return v == o.v && vt == o.vt && vn == o.vn; } };
struct IndexTripletHash { std::size_t operator()(IndexTriplet const& k) const noexcept { return ((std::size_t)k.v * 73856093u) ^ ((std::size_t)k.vt * 19349663u) ^ ((std::size_t)k.vn * 83492791u); } };

void parseObj(const std::string& filePath, std::vector<Primitives::Vertex>& out_vertices, std::vector<unsigned int>& out_indices) {
    std::vector<glm::vec3> temp_positions; std::vector<glm::vec2> temp_tex_coords; std::vector<glm::vec3> temp_normals;
    out_vertices.clear(); out_indices.clear(); std::ifstream fileStream(filePath);
    if (!fileStream.is_open()) return;
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

// --- STRUCTS ---
using json = nlohmann::json;
namespace glm { void from_json(const json& j, vec3& v) { if (j.is_array() && j.size() >= 3) { v.x = j[0]; v.y = j[1]; v.z = j[2]; } else v = vec3(0.0f); } }
namespace glm { void to_json(json& j, const vec3& v) { j = json::array({ v.x, v.y, v.z }); } }

struct Transform { glm::vec3 position, rotation, scale; };
struct Material { std::string shader, texture; glm::vec3 color; };
struct Components { std::string mesh, collider, script; };
struct Entity { std::string id, type; Transform transform; Material material; Components components; };

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Transform, position, rotation, scale)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Material, shader, texture, color)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Components, mesh, collider, script)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Entity, id, type, transform, material, components)

enum JobType { JOB_SPAWN_ENTITY, JOB_DESTROY_ENTITY, JOB_UPDATE_TRANSFORM, JOB_BUILD_SCENE, JOB_UPDATE_CAMERA, JOB_UPDATE_UI };
struct Job { JobType type; json payload; std::string id = ""; };

// --- SHADERS ---
const char* vShader = R"(#version 330 core
layout (location=0) in vec3 aPos; layout (location=1) in vec2 aTex; layout (location=2) in vec3 aNor;
uniform mat4 model; uniform mat4 view; uniform mat4 projection;
out vec3 Normal; out vec3 FragPos;
void main() { gl_Position = projection * view * model * vec4(aPos, 1.0); FragPos = vec3(model * vec4(aPos, 1.0)); Normal = mat3(transpose(inverse(model))) * aNor; })";

const char* fShader = R"(#version 330 core
out vec4 FragColor; in vec3 Normal; in vec3 FragPos; uniform vec3 uColor;
void main() { float ambientStrength = 0.6; vec3 ambient = ambientStrength * vec3(1.0); vec3 norm = normalize(Normal); vec3 lightDir = normalize(vec3(0.5, 1.0, 0.5)); float diff = max(dot(norm, lightDir), 0.0); vec3 diffuse = diff * vec3(0.8); vec3 result = (ambient + diffuse) * uColor; FragColor = vec4(result, 1.0); })";

// --- ENGINE ---
class Engine {
    GLFWwindow* window;
    unsigned int shaderProgram;
    std::vector<Entity> entities;
    std::queue<Job> jobQueue;
    std::mutex queueMutex;

    struct MeshResource { unsigned int VAO, VBO, EBO, indexCount; };
    std::unordered_map<std::string, MeshResource> meshCache;
    std::function<void(std::string)> telemetrySender;

    glm::vec3 cameraPos = glm::vec3(0.0f, 5.0f, 15.0f);
    glm::vec3 cameraFront = glm::vec3(0.0f, -0.2f, -1.0f);
    glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);

public:
    int init() {
        if (!glfwInit()) return -1;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3); glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(1024, 768, "TG ENGINE - GAMES MODULE", NULL, NULL);
        if (!window) return -1;
        glfwMakeContextCurrent(window);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;
        glEnable(GL_DEPTH_TEST);
        return 0;
    }

    void loadShader() {
        unsigned int v = glCreateShader(GL_VERTEX_SHADER); glShaderSource(v, 1, &vShader, NULL); glCompileShader(v);
        unsigned int f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &fShader, NULL); glCompileShader(f);
        shaderProgram = glCreateProgram(); glAttachShader(shaderProgram, v); glAttachShader(shaderProgram, f); glLinkProgram(shaderProgram); glDeleteShader(v); glDeleteShader(f);
    }

    void setTelemetryCallback(std::function<void(std::string)> cb) { telemetrySender = cb; }
    void emit(const std::string& event, const json& data) {
        if (telemetrySender) {
            json packet; packet["type"] = "TELEMETRY"; packet["event"] = event; packet["data"] = data;
            telemetrySender(packet.dump());
        }
    }

    void parseJob(const std::string& str) {
        try {
            json j = json::parse(str); Job newJob;
            std::string t = j["jobType"];
            std::transform(t.begin(), t.end(), t.begin(), ::toupper);

            if (t == "SPAWN_ENTITY") newJob.type = JOB_SPAWN_ENTITY;
            else if (t == "BUILD_SCENE") newJob.type = JOB_BUILD_SCENE;
            else if (t == "UPDATE") newJob.type = JOB_UPDATE_TRANSFORM;
            else if (t == "UPDATE_CAMERA") newJob.type = JOB_UPDATE_CAMERA;
            else if (t == "UPDATE_UI") newJob.type = JOB_UPDATE_UI;
            else if (t == "DESTROY_ENTITY") newJob.type = JOB_DESTROY_ENTITY;

            newJob.payload = j["payload"];
            std::lock_guard<std::mutex> lock(queueMutex); jobQueue.push(newJob);
        }
        catch (...) {}
    }

    void run() {
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            // SEND INPUTS TO PYTHON (No local camera flying)
            static bool keyStates[1024] = { false };
            auto checkKey = [&](int key, const std::string& name) {
                if (glfwGetKey(window, key) == GLFW_PRESS && !keyStates[key]) {
                    keyStates[key] = true; emit("input", { {"key", name}, {"action", "press"} });
                }
                else if (glfwGetKey(window, key) == GLFW_RELEASE && keyStates[key]) {
                    keyStates[key] = false; emit("input", { {"key", name}, {"action", "release"} });
                }
                };
            checkKey(GLFW_KEY_W, "w"); checkKey(GLFW_KEY_S, "s");
            checkKey(GLFW_KEY_A, "a"); checkKey(GLFW_KEY_D, "d");
            checkKey(GLFW_KEY_SPACE, "space"); checkKey(GLFW_KEY_Q, "q");
            checkKey(GLFW_KEY_LEFT, "left"); checkKey(GLFW_KEY_RIGHT, "right");

            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, true);

            // Process Jobs
            int processed = 0;
            while (processed < MAX_JOBS_PER_FRAME) {
                Job j; { std::lock_guard<std::mutex> lock(queueMutex); if (jobQueue.empty()) break; j = jobQueue.front(); jobQueue.pop(); }

                if (j.type == JOB_SPAWN_ENTITY) {
                    Entity ent = j.payload.get<Entity>();
                    std::string path = "assets/" + ent.components.collider + ".txt";
                    if (std::filesystem::exists(path) && meshCache.find(path) == meshCache.end()) {
                        std::vector<Primitives::Vertex> v; std::vector<unsigned int> i; parseObj(path, v, i);
                        if (!v.empty()) {
                            auto& res = meshCache[path]; glGenVertexArrays(1, &res.VAO); glGenBuffers(1, &res.VBO); glGenBuffers(1, &res.EBO);
                            glBindVertexArray(res.VAO); glBindBuffer(GL_ARRAY_BUFFER, res.VBO); glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(Primitives::Vertex), v.data(), GL_STATIC_DRAW);
                            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, res.EBO); glBufferData(GL_ELEMENT_ARRAY_BUFFER, i.size() * sizeof(unsigned int), i.data(), GL_STATIC_DRAW);
                            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Primitives::Vertex), (void*)0); glEnableVertexAttribArray(0);
                            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Primitives::Vertex), (void*)offsetof(Primitives::Vertex, texCoord)); glEnableVertexAttribArray(1);
                            glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Primitives::Vertex), (void*)offsetof(Primitives::Vertex, normal)); glEnableVertexAttribArray(2);
                            res.indexCount = i.size();
                        }
                    }
                    entities.push_back(ent);
                }
                else if (j.type == JOB_BUILD_SCENE) entities.clear();
                else if (j.type == JOB_UPDATE_TRANSFORM) {
                    for (auto& e : entities) if (e.id == j.payload["id"]) { if (j.payload.contains("position")) e.transform.position = j.payload["position"]; break; }
                }
                else if (j.type == JOB_DESTROY_ENTITY) {
                    entities.erase(std::remove_if(entities.begin(), entities.end(), [&](const Entity& e) { return e.id == j.payload["id"]; }), entities.end());
                }
                else if (j.type == JOB_UPDATE_CAMERA) {
                    if (j.payload.contains("position")) cameraPos = j.payload["position"];
                    if (j.payload.contains("lookAt")) cameraFront = glm::normalize(glm::vec3(j.payload["lookAt"]) - cameraPos);
                }
                else if (j.type == JOB_UPDATE_UI) {
                    if (j.payload.contains("title")) glfwSetWindowTitle(window, std::string(j.payload["title"]).c_str());
                }
                processed++;
            }

            // Render
            glClearColor(0.2f, 0.2f, 0.25f, 1.0f); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glUseProgram(shaderProgram);
            int w, h; glfwGetWindowSize(window, &w, &h); glViewport(0, 0, w, h);
            glm::mat4 proj = glm::perspective(glm::radians(60.0f), (float)w / h, 0.1f, 100.0f);
            glm::mat4 view = glm::lookAt(cameraPos, cameraPos + cameraFront, glm::vec3(0, 1, 0));
            glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "view"), 1, GL_FALSE, glm::value_ptr(view));
            glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "projection"), 1, GL_FALSE, glm::value_ptr(proj));

            for (const auto& ent : entities) {
                std::string path = "assets/" + ent.components.collider + ".txt";
                if (meshCache.find(path) == meshCache.end()) continue;
                auto& m = meshCache[path];
                glm::mat4 model = glm::translate(glm::mat4(1.0f), ent.transform.position);
                model = glm::scale(model, ent.transform.scale);
                glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "model"), 1, GL_FALSE, glm::value_ptr(model));
                glUniform3fv(glGetUniformLocation(shaderProgram, "uColor"), 1, glm::value_ptr(ent.material.color));
                glBindVertexArray(m.VAO); glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, 0);
            }
            glfwSwapBuffers(window);
        }
        glfwTerminate();
    }
};

int main() {
    ix::initNetSystem();
    Engine e; if (e.init() != 0) return -1;

    ix::WebSocket webSocket;
    webSocket.setUrl("ws://localhost:8080/engine");
    webSocket.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) e.parseJob(msg->str);
        });
    webSocket.start();

    e.setTelemetryCallback([&](std::string m) { webSocket.send(m); });
    e.loadShader(); e.run();
    webSocket.stop(); ix::uninitNetSystem();
    return 0;
}