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
#include <mutex>
#include <chrono>

const int MAX_JOBS_PER_FRAME = 10;

// --- 1. OBJ PARSER ---
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

// --- 2. STRUCTS ---
using json = nlohmann::json;
namespace glm { void from_json(const json& j, vec3& v) { if (j.is_array() && j.size() >= 3) { v.x = j[0]; v.y = j[1]; v.z = j[2]; } else v = vec3(0.0f); } }
namespace glm { void to_json(json& j, const vec3& v) { j = json::array({ v.x, v.y, v.z }); } }

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

enum JobType {
    JOB_SPAWN_ENTITY, JOB_DESTROY_ENTITY, JOB_UPDATE_TRANSFORM, JOB_BUILD_SCENE,
    JOB_LIFECYCLE_RESET, JOB_LIFECYCLE_START, JOB_LIFECYCLE_STOP,
    JOB_ACTION_MOVE, JOB_ACTION_INTERACT, JOB_BUCKET_SNAPSHOT
};

// --- UPDATE: Job now tracks ID ---
struct Job {
    JobType type;
    json payload;
    std::string id = ""; // New field
};

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
    std::mutex queueMutex;
    std::unordered_map<std::string, MeshResource> meshCache;
    std::function<void(std::string)> telemetrySender;
    int tickCount = 0;
    bool isRunning = false;

public:
    int init() {
        if (!glfwInit()) return -1;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3); glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(1024, 768, "TG ENGINE (Day 5)", NULL, NULL);
        if (!window) return -1;
        glfwMakeContextCurrent(window);
        glfwSetCursorPosCallback(window, mouse_callback);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;
        glEnable(GL_DEPTH_TEST);
        currentScene.ambientLight = glm::vec3(0.1f, 0.1f, 0.1f);
        std::filesystem::create_directory("bucket");
        return 0;
    }

    void loadShader() {
        unsigned int v = glCreateShader(GL_VERTEX_SHADER); glShaderSource(v, 1, &vShader, NULL); glCompileShader(v);
        unsigned int f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &fShader, NULL); glCompileShader(f);
        shaderProgram = glCreateProgram(); glAttachShader(shaderProgram, v); glAttachShader(shaderProgram, f); glLinkProgram(shaderProgram); glDeleteShader(v); glDeleteShader(f);
    }

    void setTelemetryCallback(std::function<void(std::string)> callback) { telemetrySender = callback; }
    void emit(const std::string& event, const json& data) {
        if (telemetrySender) {
            json packet; packet["type"] = "TELEMETRY"; packet["event"] = event; packet["data"] = data; packet["tick"] = tickCount;
            // Add job_id if present in data
            if (data.contains("job_id")) packet["job_id"] = data["job_id"];
            telemetrySender(packet.dump());
        }
    }

    std::string resolveMesh(const std::string& collider) {
        if (collider == "box") return "assets/cube.txt";
        if (collider == "sphere") return "assets/sphere.txt";
        return "assets/cube.txt";
    }

    void writeBucketSnapshot() {
        try {
            json snapshot; snapshot["tick"] = tickCount; snapshot["entities"] = entities; snapshot["scene"] = currentScene;
            auto now = std::chrono::system_clock::now().time_since_epoch().count();
            std::string filename = "bucket/snapshot_" + std::to_string(now) + ".json";
            std::ofstream file(filename); file << snapshot.dump(4);
            emit("bucket_write", { {"file", filename}, {"status", "success"} });
        }
        catch (...) { emit("error", { {"msg", "Bucket write failed"} }); }
    }

    void internalSpawn(const Entity& ent) {
        std::string path = resolveMesh(ent.components.collider);
        if (!std::filesystem::exists(path)) { emit("error", { {"msg", "Asset missing"}, {"path", path} }); return; }
        if (meshCache.find(path) == meshCache.end()) {
            std::vector<Primitives::Vertex> v; std::vector<unsigned int> i; parseObj(path, v, i);
            if (v.empty()) return;
            MeshResource res; glGenVertexArrays(1, &res.VAO); glGenBuffers(1, &res.VBO); glGenBuffers(1, &res.EBO);
            glBindVertexArray(res.VAO); glBindBuffer(GL_ARRAY_BUFFER, res.VBO); glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(Primitives::Vertex), v.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, res.EBO); glBufferData(GL_ELEMENT_ARRAY_BUFFER, i.size() * sizeof(unsigned int), i.data(), GL_STATIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Primitives::Vertex), (void*)0); glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Primitives::Vertex), (void*)offsetof(Primitives::Vertex, texCoord)); glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Primitives::Vertex), (void*)offsetof(Primitives::Vertex, normal)); glEnableVertexAttribArray(2);
            res.indexCount = (unsigned int)i.size(); meshCache[path] = res;
        }
        entities.push_back(ent);
    }

    void pushJob(Job j) { std::lock_guard<std::mutex> lock(queueMutex); jobQueue.push(j); }

    void parseJob(const std::string& str) {
        try {
            json j = json::parse(str); Job newJob;

            // --- UPDATE: Capture Job ID ---
            if (j.contains("job_id")) newJob.id = j["job_id"];

            if (j.contains("command")) {
                std::string c = j["command"];
                if (c == "START") newJob.type = JOB_LIFECYCLE_START;
                else if (c == "STOP") newJob.type = JOB_LIFECYCLE_STOP;
                else if (c == "RESET") newJob.type = JOB_LIFECYCLE_RESET;
                else if (c == "SNAPSHOT") newJob.type = JOB_BUCKET_SNAPSHOT;
                else if (c == "ACTION") { newJob.type = JOB_ACTION_MOVE; newJob.payload = j["payload"]; }
            }
            else {
                std::string t = j["jobType"];
                // Normalization: Ensure uppercase (Fixes mismatch if Python sends lowercase)
                std::transform(t.begin(), t.end(), t.begin(), ::toupper);

                if (t == "SPAWN_ENTITY") newJob.type = JOB_SPAWN_ENTITY;
                else if (t == "BUILD_SCENE") newJob.type = JOB_BUILD_SCENE;
                else if (t == "UPDATE") newJob.type = JOB_UPDATE_TRANSFORM;
                newJob.payload = j["payload"];
            }
            pushJob(newJob);
        }
        catch (...) {}
    }

    void processJobs() {
        int jobsProcessed = 0;
        while (true) {
            if (jobsProcessed >= MAX_JOBS_PER_FRAME) break;
            Job j;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (jobQueue.empty()) break;
                j = jobQueue.front(); jobQueue.pop();
            }
            try {
                // --- UPDATE: Emit Started ---
                if (!j.id.empty()) emit("job_started", { {"job_id", j.id} });

                switch (j.type) {
                case JOB_LIFECYCLE_START: isRunning = true; emit("status", { {"state", "running"} }); break;
                case JOB_LIFECYCLE_STOP: isRunning = false; emit("status", { {"state", "paused"} }); break;
                case JOB_LIFECYCLE_RESET: entities.clear(); break;
                case JOB_BUCKET_SNAPSHOT: writeBucketSnapshot(); break;
                case JOB_SPAWN_ENTITY: internalSpawn(j.payload.get<Entity>()); break;
                case JOB_BUILD_SCENE:
                    entities.clear();
                    if (j.payload.contains("ambientLight")) currentScene.ambientLight = j.payload["ambientLight"];
                    break;
                case JOB_UPDATE_TRANSFORM: {
                    std::string id = j.payload["id"];
                    for (auto& e : entities) if (e.id == id) { if (j.payload.contains("position")) e.transform.position = j.payload["position"]; break; }
                    break;
                }
                case JOB_ACTION_MOVE: {
                    std::string id = j.payload["id"]; glm::vec3 v = j.payload["vec"];
                    for (auto& e : entities) if (e.id == id) e.transform.position += v;
                    break;
                }
                }

                // --- UPDATE: Emit Completed ---
                if (!j.id.empty()) emit("job_completed", { {"job_id", j.id}, {"result", "success"} });

            }
            catch (const std::exception& e) {
                if (!j.id.empty()) emit("job_failed", { {"job_id", j.id}, {"error", e.what()} });
                else emit("error", { {"msg", "Job Failed"}, {"details", e.what()} });
            }
            jobsProcessed++;
        }
    }

    void run() {
        float lastFrame = 0.0f;
        while (!glfwWindowShouldClose(window)) {
            float current = (float)glfwGetTime(); float dt = current - lastFrame; lastFrame = current;
            glfwPollEvents();

            float speed = 5.0f * dt;
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) cameraPos += speed * cameraFront;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) cameraPos -= speed * cameraFront;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) cameraPos -= glm::normalize(glm::cross(cameraFront, cameraUp)) * speed;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) cameraPos += glm::normalize(glm::cross(cameraFront, cameraUp)) * speed;

            processJobs();
            if (isRunning) { tickCount++; if (tickCount % 60 == 0) emit("tick_update", { {"fps", 1.0f / dt} }); }

            glm::vec3 bg = currentScene.ambientLight; glClearColor(bg.r * 0.2f, bg.g * 0.2f, bg.b * 0.2f, 1.0f); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glUseProgram(shaderProgram);
            int w, h; glfwGetWindowSize(window, &w, &h); glViewport(0, 0, w, h);
            glm::mat4 proj = glm::perspective(glm::radians(fov), (float)w / h, 0.1f, 100.0f);
            glm::mat4 view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
            glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "view"), 1, GL_FALSE, glm::value_ptr(view));
            glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "projection"), 1, GL_FALSE, glm::value_ptr(proj));
            glUniform3fv(glGetUniformLocation(shaderProgram, "uLightColor"), 1, glm::value_ptr(currentScene.ambientLight));

            for (const auto& ent : entities) {
                std::string path = resolveMesh(ent.components.collider);
                if (meshCache.find(path) == meshCache.end()) continue;
                MeshResource& m = meshCache[path];
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

void mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
    float xpos = static_cast<float>(xposIn); float ypos = static_cast<float>(yposIn);
    if (firstMouse) { lastX = xpos; lastY = ypos; firstMouse = false; }
    float xoffset = xpos - lastX; float yoffset = lastY - ypos; lastX = xpos; lastY = ypos; xoffset *= 0.1f; yoffset *= 0.1f;
    yaw += xoffset; pitch += yoffset; if (pitch > 89.0f) pitch = 89.0f; if (pitch < -89.0f) pitch = -89.0f;
    glm::vec3 front; front.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch)); front.y = sin(glm::radians(pitch)); front.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch)); cameraFront = glm::normalize(front);
}

int main() {
    Engine e;
    if (e.init() != 0) return -1;
    NetworkClient net;
    e.setTelemetryCallback([&](std::string m) { net.send(m); });
    net.init([&](std::string m) { e.parseJob(m); });
    e.loadShader();
    e.run();
    net.cleanup();
    return 0;
}