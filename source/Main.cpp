#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

// IXWebSocket pulls in windows.h — must come BEFORE glad to avoid APIENTRY redefinition
#include <IXNetSystem.h>
#include <IXWebSocket.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

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
#include <functional>
#include <array>
#include <cmath>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int   MAX_JOBS_PER_FRAME = 50;
static constexpr float FOV_DEG = 60.0f;
static constexpr float NEAR_PLANE = 0.1f;
static constexpr float FAR_PLANE = 200.0f;
static constexpr int   WIN_W = 1024;
static constexpr int   WIN_H = 768;
static constexpr float MOUSE_SENSITIVITY = 0.12f; // degrees per pixel

// ---------------------------------------------------------------------------
// OBJ Parser
// ---------------------------------------------------------------------------
namespace Primitives {
    struct Vertex {
        glm::vec3 position;
        glm::vec2 texCoord;
        glm::vec3 normal;
    };
}

struct IndexTriplet {
    int v, vt, vn;
    bool operator==(const IndexTriplet& o) const noexcept {
        return v == o.v && vt == o.vt && vn == o.vn;
    }
};

struct IndexTripletHash {
    std::size_t operator()(const IndexTriplet& k) const noexcept {
        std::size_t h = 2166136261u;
        h ^= (std::size_t)k.v;  h *= 16777619u;
        h ^= (std::size_t)k.vt; h *= 16777619u;
        h ^= (std::size_t)k.vn; h *= 16777619u;
        return h;
    }
};

static bool parseObj(const std::string& filePath,
    std::vector<Primitives::Vertex>& out_vertices,
    std::vector<unsigned int>& out_indices)
{
    std::ifstream fileStream(filePath);
    if (!fileStream.is_open()) return false;

    std::vector<glm::vec3> temp_positions;
    std::vector<glm::vec2> temp_tex_coords;
    std::vector<glm::vec3> temp_normals;

    temp_positions.reserve(512);
    temp_tex_coords.reserve(512);
    temp_normals.reserve(512);
    out_vertices.reserve(1024);
    out_indices.reserve(2048);

    std::unordered_map<IndexTriplet, unsigned int, IndexTripletHash> indexMap;
    indexMap.reserve(1024);

    std::string line;
    while (std::getline(fileStream, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::stringstream ss(line);
        std::string prefix;
        ss >> prefix;

        if (prefix == "v") {
            glm::vec3 p{};
            ss >> p.x >> p.y >> p.z;
            temp_positions.push_back(p);
        }
        else if (prefix == "vt") {
            glm::vec2 t{};
            ss >> t.x >> t.y;
            temp_tex_coords.push_back(t);
        }
        else if (prefix == "vn") {
            glm::vec3 n{};
            ss >> n.x >> n.y >> n.z;
            temp_normals.push_back(n);
        }
        else if (prefix == "f") {
            std::string vertStr;
            std::vector<unsigned int> faceIndices;
            faceIndices.reserve(4);

            while (ss >> vertStr) {
                int vi = 0, vti = 0, vni = 0;
                std::stringstream vss(vertStr);
                vss >> vi;
                if (vss.peek() == '/') {
                    vss.ignore();
                    if (vss.peek() != '/') vss >> vti;
                }
                if (vss.peek() == '/') {
                    vss.ignore();
                    vss >> vni;
                }

                IndexTriplet key{ vi, vti, vni };
                auto it = indexMap.find(key);
                if (it != indexMap.end()) {
                    faceIndices.push_back(it->second);
                }
                else {
                    Primitives::Vertex vx{};
                    if (vi > 0 && vi <= (int)temp_positions.size())  vx.position = temp_positions[vi - 1];
                    if (vti > 0 && vti <= (int)temp_tex_coords.size()) vx.texCoord = temp_tex_coords[vti - 1];
                    if (vni > 0 && vni <= (int)temp_normals.size())    vx.normal = temp_normals[vni - 1];

                    unsigned int idx = static_cast<unsigned int>(out_vertices.size());
                    out_vertices.push_back(vx);
                    indexMap[key] = idx;
                    faceIndices.push_back(idx);
                }
            }

            for (size_t k = 1; k + 1 < faceIndices.size(); ++k) {
                out_indices.push_back(faceIndices[0]);
                out_indices.push_back(faceIndices[k]);
                out_indices.push_back(faceIndices[k + 1]);
            }
        }
    }
    return !out_vertices.empty();
}

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------
using json = nlohmann::json;

namespace glm {
    inline void from_json(const json& j, vec3& v) {
        if (j.is_array() && j.size() >= 3)
            v = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
        else
            v = vec3(0.0f);
    }
    inline void to_json(json& j, const vec3& v) {
        j = json::array({ v.x, v.y, v.z });
    }
}

struct Transform { glm::vec3 position, rotation, scale; };
struct Material { std::string shader, texture; glm::vec3 color; };
struct Components { std::string mesh, collider, script; };

struct Entity {
    std::string id, type;
    Transform   transform;
    Material    material;
    Components  components;
    glm::mat4   modelMatrix{ 1.0f };
    bool        dirty{ true };
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Transform, position, rotation, scale)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Material, shader, texture, color)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Components, mesh, collider, script)

inline void from_json(const json& j, Entity& e) {
    j.at("id").get_to(e.id);
    j.at("type").get_to(e.type);
    j.at("transform").get_to(e.transform);
    j.at("material").get_to(e.material);
    j.at("components").get_to(e.components);
    e.dirty = true;
}

enum JobType {
    JOB_SPAWN_ENTITY,
    JOB_DESTROY_ENTITY,
    JOB_UPDATE_TRANSFORM,
    JOB_BUILD_SCENE,
    JOB_UPDATE_CAMERA,
    JOB_UPDATE_UI,
    JOB_SET_MOUSE_MODE  // payload: { "mode": "capture" | "free" }
};

struct Job {
    JobType type;
    json    payload;
};

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
static const char* kVertShader = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTex;
layout(location = 2) in vec3 aNor;

uniform mat4 model;
uniform mat4 viewProj;
uniform mat4 normalMatrix;  // precomputed transpose(inverse(model)) - avoids doing it per-vertex in shader

out vec3  vNormal;
out vec3  vFragPos;
out float vDepth;

void main() {
    vec4 worldPos = model * vec4(aPos, 1.0);
    gl_Position   = viewProj * worldPos;
    vFragPos      = worldPos.xyz;
    vNormal       = normalize(vec3(normalMatrix * vec4(aNor, 0.0)));
    // Pass clip-space w as linear depth for fog (avoids expensive reconstruction from gl_FragCoord)
    vDepth        = gl_Position.w;
}
)glsl";

static const char* kFragShader = R"glsl(
#version 330 core
in  vec3  vNormal;
in  vec3  vFragPos;
in  float vDepth;
out vec4  FragColor;

uniform vec3 uColor;
uniform vec3 uCamPos;

const vec3  kLightDir = normalize(vec3(0.5, 1.0, 0.5));
const vec3  kFogColor = vec3(0.18, 0.18, 0.22);  // must match glClearColor
const float kFogNear  = 30.0;
const float kFogFar   = 120.0;
const float kAmbient  = 0.45;
const float kDiffStr  = 0.55;
const float kRimStr   = 0.12;

void main() {
    // Two-sided lighting: flip normal for back faces so assets with inconsistent
    // winding still shade correctly instead of going black or showing through.
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;

    // Key light (directional)
    float diff = max(dot(N, kLightDir), 0.0);

    // Rim light from camera - wraps around silhouettes, gives greybox shapes
    // readable edges even when the key light is nearly head-on
    vec3  viewDir = normalize(uCamPos - vFragPos);
    float rim     = pow(1.0 - max(dot(N, viewDir), 0.0), 3.0) * kRimStr;

    vec3 light = vec3(kAmbient) + kDiffStr * diff + rim;
    vec3 color = light * uColor;

    // Linear depth fog - objects fade to sky colour with distance
    float fog = clamp((vDepth - kFogNear) / (kFogFar - kFogNear), 0.0, 1.0);
    color = mix(color, kFogColor, fog);

    FragColor = vec4(color, 1.0);
}
)glsl";

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
class Engine {
public:
    int  init();
    void loadShaders();
    void run();
    void parseJob(const std::string& str);
    void setTelemetryCallback(std::function<void(std::string)> cb) {
        telemetrySender_ = std::move(cb);
    }

private:
    struct MeshResource {
        unsigned int VAO{}, VBO{}, EBO{};
        unsigned int indexCount{};
    };

    struct Uniforms {
        int model{ -1 }, viewProj{ -1 }, uColor{ -1 }, normalMatrix{ -1 }, uCamPos{ -1 };
    };

    void          emit(const std::string& event, const json& data);
    MeshResource* getOrLoadMesh(const std::string& colliderName);
    glm::mat4     buildModelMatrix(const Entity& e) const;
    void          processJobs();
    void          handleInput();
    void          handleMouse();
    void          render();
    void          clearScene();
    void          setMouseCapture(bool capture);

    // GL / window
    GLFWwindow* window_ = nullptr;
    unsigned int shaderProgram_ = 0;
    Uniforms     uniforms_;

    // Scene
    std::vector<Entity>                           entities_;
    std::unordered_map<std::string, MeshResource> meshCache_;

    // Job queue
    std::queue<Job> jobQueue_;
    std::mutex      queueMutex_;

    // Callbacks
    std::function<void(std::string)> telemetrySender_;

    // Camera
    glm::vec3 camPos_{ 0.0f, 5.0f, 15.0f };
    glm::vec3 camFront_{ 0.0f, -0.2f, -1.0f };

    // Keyboard state
    std::array<bool, GLFW_KEY_LAST + 1> keyStates_{};

    // Mouse state
    bool   mouseCaptured_{ false };
    double lastMouseX_{ WIN_W / 2.0 };
    double lastMouseY_{ WIN_H / 2.0 };
    bool   firstMouseFrame_{ true };

    // UI
    std::string pendingTitle_;
    bool        titleDirty_{ false };
};

// ---------------------------------------------------------------------------
int Engine::init() {
    if (!glfwInit()) { std::cerr << "glfwInit failed\n"; return -1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    window_ = glfwCreateWindow(WIN_W, WIN_H, "TG ENGINE", nullptr, nullptr);
    if (!window_) { glfwTerminate(); return -1; }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1); // vsync

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "gladLoad failed\n"; return -1;
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);  

    return 0;
}

// ---------------------------------------------------------------------------
static unsigned int compileShader(GLenum type, const char* src) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        std::cerr << "Shader error: " << log << '\n';
    }
    return s;
}

void Engine::loadShaders() {
    unsigned int v = compileShader(GL_VERTEX_SHADER, kVertShader);
    unsigned int f = compileShader(GL_FRAGMENT_SHADER, kFragShader);
    shaderProgram_ = glCreateProgram();
    glAttachShader(shaderProgram_, v);
    glAttachShader(shaderProgram_, f);
    glLinkProgram(shaderProgram_);
    glDeleteShader(v);
    glDeleteShader(f);

    // Cache once — never call glGetUniformLocation in the hot path
    uniforms_.model = glGetUniformLocation(shaderProgram_, "model");
    uniforms_.viewProj = glGetUniformLocation(shaderProgram_, "viewProj");
    uniforms_.uColor = glGetUniformLocation(shaderProgram_, "uColor");
    uniforms_.normalMatrix = glGetUniformLocation(shaderProgram_, "normalMatrix");
    uniforms_.uCamPos = glGetUniformLocation(shaderProgram_, "uCamPos");
}

// ---------------------------------------------------------------------------
void Engine::emit(const std::string& event, const json& data) {
    if (!telemetrySender_) return;
    json packet;
    packet["type"] = "TELEMETRY";
    packet["event"] = event;
    packet["data"] = data;
    telemetrySender_(packet.dump());
}

// ---------------------------------------------------------------------------
void Engine::setMouseCapture(bool capture) {
    mouseCaptured_ = capture;
    firstMouseFrame_ = true; // reset jump-guard whenever mode changes
    glfwSetInputMode(window_, GLFW_CURSOR,
        capture ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

// ---------------------------------------------------------------------------
Engine::MeshResource* Engine::getOrLoadMesh(const std::string& colliderName) {
    std::string path = "assets/" + colliderName + ".txt";

    auto it = meshCache_.find(path);
    if (it != meshCache_.end()) return &it->second;

    if (!std::filesystem::exists(path)) return nullptr;

    std::vector<Primitives::Vertex> verts;
    std::vector<unsigned int>       indices;
    if (!parseObj(path, verts, indices)) return nullptr;

    MeshResource res{};
    glGenVertexArrays(1, &res.VAO);
    glGenBuffers(1, &res.VBO);
    glGenBuffers(1, &res.EBO);

    glBindVertexArray(res.VAO);

    glBindBuffer(GL_ARRAY_BUFFER, res.VBO);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(verts.size() * sizeof(Primitives::Vertex)),
        verts.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, res.EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)(indices.size() * sizeof(unsigned int)),
        indices.data(), GL_STATIC_DRAW);

    constexpr auto stride = (GLsizei)sizeof(Primitives::Vertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
        (void*)offsetof(Primitives::Vertex, texCoord));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride,
        (void*)offsetof(Primitives::Vertex, normal));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    res.indexCount = (unsigned int)indices.size();
    meshCache_[path] = res;
    return &meshCache_[path];
}

// ---------------------------------------------------------------------------
glm::mat4 Engine::buildModelMatrix(const Entity& e) const {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), e.transform.position);
    if (e.transform.rotation.y != 0.0f)
        m = glm::rotate(m, glm::radians(e.transform.rotation.y), { 0,1,0 });
    if (e.transform.rotation.x != 0.0f)
        m = glm::rotate(m, glm::radians(e.transform.rotation.x), { 1,0,0 });
    if (e.transform.rotation.z != 0.0f)
        m = glm::rotate(m, glm::radians(e.transform.rotation.z), { 0,0,1 });
    m = glm::scale(m, e.transform.scale);
    return m;
}

// ---------------------------------------------------------------------------
void Engine::clearScene() {
    for (auto& [key, res] : meshCache_) {
        glDeleteVertexArrays(1, &res.VAO);
        glDeleteBuffers(1, &res.VBO);
        glDeleteBuffers(1, &res.EBO);
    }
    meshCache_.clear();
    entities_.clear();
}

// ---------------------------------------------------------------------------
void Engine::parseJob(const std::string& str) {
    try {
        json j = json::parse(str);

        std::string t = j.value("jobType", "");
        std::transform(t.begin(), t.end(), t.begin(), ::toupper);

        Job job{};
        if (t == "SPAWN_ENTITY")   job.type = JOB_SPAWN_ENTITY;
        else if (t == "BUILD_SCENE")    job.type = JOB_BUILD_SCENE;
        else if (t == "UPDATE")         job.type = JOB_UPDATE_TRANSFORM;
        else if (t == "UPDATE_CAMERA")  job.type = JOB_UPDATE_CAMERA;
        else if (t == "UPDATE_UI")      job.type = JOB_UPDATE_UI;
        else if (t == "DESTROY_ENTITY") job.type = JOB_DESTROY_ENTITY;
        else if (t == "SET_MOUSE_MODE") job.type = JOB_SET_MOUSE_MODE;
        else return;

        job.payload = std::move(j["payload"]);

        std::lock_guard<std::mutex> lock(queueMutex_);
        jobQueue_.push(std::move(job));
    }
    catch (const std::exception& ex) {
        std::cerr << "parseJob error: " << ex.what() << '\n';
    }
}

// ---------------------------------------------------------------------------
void Engine::handleInput() {
    static const struct { int key; const char* name; } kKeys[] = {
        { GLFW_KEY_W,     "w"     }, { GLFW_KEY_S,     "s"     },
        { GLFW_KEY_A,     "a"     }, { GLFW_KEY_D,     "d"     },
        { GLFW_KEY_SPACE, "space" }, { GLFW_KEY_Q,     "q"     },
        { GLFW_KEY_LEFT,  "left"  }, { GLFW_KEY_RIGHT, "right" },
        { GLFW_KEY_UP,    "up"    }, { GLFW_KEY_DOWN,  "down"  },
    };

    for (auto& [key, name] : kKeys) {
        bool pressed = (glfwGetKey(window_, key) == GLFW_PRESS);
        if (pressed && !keyStates_[key]) {
            keyStates_[key] = true;
            emit("input", { {"key", name}, {"action", "press"} });
        }
        else if (!pressed && keyStates_[key]) {
            keyStates_[key] = false;
            emit("input", { {"key", name}, {"action", "release"} });
        }
    }

    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        if (mouseCaptured_) setMouseCapture(false);
        else glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
}

// ---------------------------------------------------------------------------
void Engine::handleMouse() {
    if (!mouseCaptured_) return;

    double mx, my;
    glfwGetCursorPos(window_, &mx, &my);

    if (firstMouseFrame_) {
        lastMouseX_ = mx;
        lastMouseY_ = my;
        firstMouseFrame_ = false;
        return;
    }

    double dx = mx - lastMouseX_;
    double dy = my - lastMouseY_;
    lastMouseX_ = mx;
    lastMouseY_ = my;

    // Dead-zone: ignore sub-pixel jitter
    if (std::abs(dx) < 0.5 && std::abs(dy) < 0.5) return;

    emit("mouse", {
        {"dx", dx * MOUSE_SENSITIVITY},
        {"dy", dy * MOUSE_SENSITIVITY}
        });
}

// ---------------------------------------------------------------------------
void Engine::processJobs() {
    int processed = 0;
    while (processed < MAX_JOBS_PER_FRAME) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (jobQueue_.empty()) break;
            job = std::move(jobQueue_.front());
            jobQueue_.pop();
        }

        switch (job.type) {

        case JOB_SPAWN_ENTITY: {
            Entity ent = job.payload.get<Entity>();
            getOrLoadMesh(ent.components.collider);
            ent.modelMatrix = buildModelMatrix(ent);
            ent.dirty = false;
            entities_.push_back(std::move(ent));
            break;
        }

        case JOB_BUILD_SCENE:
            clearScene();
            break;

        case JOB_UPDATE_TRANSFORM: {
            const std::string id = job.payload.value("id", "");
            for (auto& e : entities_) {
                if (e.id != id) continue;
                if (job.payload.contains("position")) e.transform.position = job.payload["position"];
                if (job.payload.contains("rotation")) e.transform.rotation = job.payload["rotation"];
                if (job.payload.contains("scale"))    e.transform.scale = job.payload["scale"];
                e.dirty = true;
                break;
            }
            break;
        }

        case JOB_DESTROY_ENTITY: {
            const std::string id = job.payload.value("id", "");
            entities_.erase(
                std::remove_if(entities_.begin(), entities_.end(),
                    [&](const Entity& e) { return e.id == id; }),
                entities_.end());
            break;
        }

        case JOB_UPDATE_CAMERA: {
            if (job.payload.contains("position")) camPos_ = job.payload["position"];
            if (job.payload.contains("lookAt")) {
                glm::vec3 target = job.payload["lookAt"];
                camFront_ = glm::normalize(target - camPos_);
            }
            break;
        }

        case JOB_UPDATE_UI: {
            if (job.payload.contains("title")) {
                pendingTitle_ = job.payload["title"].get<std::string>();
                titleDirty_ = true;
            }
            break;
        }

        case JOB_SET_MOUSE_MODE: {
            std::string mode = job.payload.value("mode", "free");
            setMouseCapture(mode == "capture");
            break;
        }
        }
        ++processed;
    }
}

// ---------------------------------------------------------------------------
void Engine::render() {
    int w, h;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w == 0 || h == 0) return;

    glViewport(0, 0, w, h);
    glClearColor(0.18f, 0.18f, 0.22f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(shaderProgram_);

    glm::mat4 view = glm::lookAt(camPos_, camPos_ + camFront_, { 0,1,0 });
    glm::mat4 proj = glm::perspective(glm::radians(FOV_DEG), (float)w / (float)h, NEAR_PLANE, FAR_PLANE);
    glm::mat4 viewProj = proj * view;
    glUniformMatrix4fv(uniforms_.viewProj, 1, GL_FALSE, glm::value_ptr(viewProj));

    // Camera position for rim light - upload once per frame not per entity
    glUniform3fv(uniforms_.uCamPos, 1, glm::value_ptr(camPos_));

    for (auto& ent : entities_) {
        std::string path = "assets/" + ent.components.collider + ".txt";
        auto it = meshCache_.find(path);
        if (it == meshCache_.end()) continue;

        if (ent.dirty) {
            ent.modelMatrix = buildModelMatrix(ent);
            ent.dirty = false;
        }

        // Normal matrix: inverse-transpose of model. Computed on CPU to avoid
        // doing it per-vertex in the shader (expensive for large meshes).
        glm::mat4 normalMat = glm::transpose(glm::inverse(ent.modelMatrix));

        glUniformMatrix4fv(uniforms_.model, 1, GL_FALSE, glm::value_ptr(ent.modelMatrix));
        glUniformMatrix4fv(uniforms_.normalMatrix, 1, GL_FALSE, glm::value_ptr(normalMat));
        glUniform3fv(uniforms_.uColor, 1, glm::value_ptr(ent.material.color));
        glBindVertexArray(it->second.VAO);
        glDrawElements(GL_TRIANGLES, (GLsizei)it->second.indexCount, GL_UNSIGNED_INT, nullptr);
    }

    glBindVertexArray(0);

    if (titleDirty_) {
        glfwSetWindowTitle(window_, pendingTitle_.c_str());
        titleDirty_ = false;
    }

    glfwSwapBuffers(window_);
}

// ---------------------------------------------------------------------------
void Engine::run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        handleInput();
        handleMouse();
        processJobs();
        render();
    }

    clearScene();
    glDeleteProgram(shaderProgram_);
    glfwDestroyWindow(window_);
    glfwTerminate();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main() {
    ix::initNetSystem();

    Engine engine;
    if (engine.init() != 0) return -1;
    engine.loadShaders();

    ix::WebSocket ws;
    ws.setUrl("ws://localhost:8080/engine");

    ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message)
            engine.parseJob(msg->str);
        });

    ws.start();
    engine.setTelemetryCallback([&](std::string m) { ws.send(m); });

    engine.run();

    ws.stop();
    ix::uninitNetSystem();
    return 0;
}