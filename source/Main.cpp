#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <iostream>
#include "GameEngine.h"

// ============================================================================
// BOILERPLATE: CAMERA & SHADER CLASSES
// (Included inline for single-file compilation convenience)
// ============================================================================

class SimpleCamera {
public:
    glm::vec3 Position, Front, Up, Right;
    float Yaw = -90.0f, Pitch = -20.0f;
    float MovementSpeed = 20.0f;

    SimpleCamera(glm::vec3 pos) : Position(pos), Front(0, 0, -1), Up(0, 1, 0) { UpdateVectors(); }

    glm::mat4 GetViewMatrix() { return glm::lookAt(Position, Position + Front, Up); }

    void ProcessKeyboard(int dir, float dt) {
        float vel = MovementSpeed * dt;
        if (dir == 0) Position += Front * vel;
        if (dir == 1) Position -= Front * vel;
        if (dir == 2) Position -= Right * vel;
        if (dir == 3) Position += Right * vel;
    }

    void ProcessMouse(float xoffset, float yoffset) {
        Yaw += xoffset * 0.1f;
        Pitch += yoffset * 0.1f;
        if (Pitch > 89.0f) Pitch = 89.0f;
        if (Pitch < -89.0f) Pitch = -89.0f;
        UpdateVectors();
    }

private:
    void UpdateVectors() {
        glm::vec3 f;
        f.x = cos(glm::radians(Yaw)) * cos(glm::radians(Pitch));
        f.y = sin(glm::radians(Pitch));
        f.z = sin(glm::radians(Yaw)) * cos(glm::radians(Pitch));
        Front = glm::normalize(f);
        Right = glm::normalize(glm::cross(Front, glm::vec3(0, 1, 0)));
        Up = glm::normalize(glm::cross(Right, Front));
    }
};

class Shader {
public:
    unsigned int ID;
    Shader(const char* vPath, const char* fPath) {
        // Simple inline shader source for portability
        const char* vSrc = R"(
            #version 330 core
            layout (location = 0) in vec3 aPos;
            layout (location = 1) in vec3 aNormal;
            out vec3 FragPos;
            out vec3 Normal;
            uniform mat4 model;
            uniform mat4 view;
            uniform mat4 projection;
            void main() {
                FragPos = vec3(model * vec4(aPos, 1.0));
                Normal = mat3(transpose(inverse(model))) * aNormal;
                gl_Position = projection * view * vec4(FragPos, 1.0);
            }
        )";
        const char* fSrc = R"(
            #version 330 core
            out vec4 FragColor;
            in vec3 Normal;
            in vec3 FragPos;
            uniform vec3 lightPos;
            uniform vec3 color;
            void main() {
                // Ambient
                float ambientStrength = 0.3;
                vec3 ambient = ambientStrength * color;
                // Diffuse
                vec3 norm = normalize(Normal);
                vec3 lightDir = normalize(lightPos - FragPos);
                float diff = max(dot(norm, lightDir), 0.0);
                vec3 diffuse = diff * color;
                FragColor = vec4(ambient + diffuse, 1.0);
            }
        )";

        unsigned int v, f;
        v = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(v, 1, &vSrc, NULL);
        glCompileShader(v);

        f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(f, 1, &fSrc, NULL);
        glCompileShader(f);

        ID = glCreateProgram();
        glAttachShader(ID, v);
        glAttachShader(ID, f);
        glLinkProgram(ID);
        glDeleteShader(v); glDeleteShader(f);
    }
    void use() { glUseProgram(ID); }
    void setMat4(const std::string& n, const glm::mat4& m) {
        glUniformMatrix4fv(glGetUniformLocation(ID, n.c_str()), 1, GL_FALSE, &m[0][0]);
    }
    void setVec3(const std::string& n, const glm::vec3& v) {
        glUniform3fv(glGetUniformLocation(ID, n.c_str()), 1, &v[0]);
    }
};

// ============================================================================
// GLOBALS & CALLBACKS
// ============================================================================
const unsigned int SCR_WIDTH = 1280;
const unsigned int SCR_HEIGHT = 720;

GameEngine engine;
SimpleCamera camera(glm::vec3(0.0f, 5.0f, 15.0f));

bool inputMode = false;
std::string currentInput = "";
float lastX = SCR_WIDTH / 2, lastY = SCR_HEIGHT / 2;
bool firstMouse = true;

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ENTER) {
            if (inputMode) {
                // Submit Command
                if (!currentInput.empty()) engine.AddCommand(currentInput);
                inputMode = false;
                currentInput.clear();
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                std::cout << "\n";
            }
            else {
                // Start Typing
                inputMode = true;
                currentInput.clear();
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                std::cout << "> ";
            }
            return;
        }

        if (inputMode) {
            if (key == GLFW_KEY_BACKSPACE && !currentInput.empty()) {
                currentInput.pop_back();
                std::cout << "\b \b";
            }
            else if (key >= GLFW_KEY_SPACE && key <= GLFW_KEY_Z) {
                char c = (char)key;
                if (!(mods & GLFW_MOD_SHIFT)) c = tolower(c);
                currentInput.push_back(c);
                std::cout << c;
            }
        }
    }
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    if (inputMode) return;
    if (firstMouse) { lastX = xpos; lastY = ypos; firstMouse = false; }
    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos;
    lastX = xpos; lastY = ypos;
    camera.ProcessMouse(xoffset, yoffset);
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "TTG Engine (Day 10)", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;
    glEnable(GL_DEPTH_TEST);

    // --- SETUP DATA ---
    Shader shader("v", "f"); // Sources handled inside class constructor
    engine.Init();

    // Cube Mesh
    float vertices[] = {
        -0.5f,-0.5f,-0.5f,  0.0f, 0.0f,-1.0f,
         0.5f,-0.5f,-0.5f,  0.0f, 0.0f,-1.0f,
         0.5f, 0.5f,-0.5f,  0.0f, 0.0f,-1.0f,
        -0.5f, 0.5f,-0.5f,  0.0f, 0.0f,-1.0f,
        -0.5f,-0.5f, 0.5f,  0.0f, 0.0f, 1.0f,
         0.5f,-0.5f, 0.5f,  0.0f, 0.0f, 1.0f,
         0.5f, 0.5f, 0.5f,  0.0f, 0.0f, 1.0f,
        -0.5f, 0.5f, 0.5f,  0.0f, 0.0f, 1.0f,
        -0.5f, 0.5f, 0.5f, -1.0f, 0.0f, 0.0f,
        -0.5f, 0.5f,-0.5f, -1.0f, 0.0f, 0.0f,
        -0.5f,-0.5f,-0.5f, -1.0f, 0.0f, 0.0f,
        -0.5f,-0.5f, 0.5f, -1.0f, 0.0f, 0.0f,
         0.5f, 0.5f, 0.5f,  1.0f, 0.0f, 0.0f,
         0.5f, 0.5f,-0.5f,  1.0f, 0.0f, 0.0f,
         0.5f,-0.5f,-0.5f,  1.0f, 0.0f, 0.0f,
         0.5f,-0.5f, 0.5f,  1.0f, 0.0f, 0.0f,
        -0.5f,-0.5f,-0.5f,  0.0f,-1.0f, 0.0f,
         0.5f,-0.5f,-0.5f,  0.0f,-1.0f, 0.0f,
         0.5f,-0.5f, 0.5f,  0.0f,-1.0f, 0.0f,
        -0.5f,-0.5f, 0.5f,  0.0f,-1.0f, 0.0f,
        -0.5f, 0.5f,-0.5f,  0.0f, 1.0f, 0.0f,
         0.5f, 0.5f,-0.5f,  0.0f, 1.0f, 0.0f,
         0.5f, 0.5f, 0.5f,  0.0f, 1.0f, 0.0f,
        -0.5f, 0.5f, 0.5f,  0.0f, 1.0f, 0.0f
    };
    unsigned int indices[] = {
        0,1,2, 2,3,0, 4,5,6, 6,7,4, 8,9,10, 10,11,8,
        12,13,14, 14,15,12, 16,17,18, 18,19,16, 20,21,22, 22,23,20
    };

    unsigned int VBO, VAO, EBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // --- GAME LOOP ---
    float lastFrame = 0.0f;
    std::cout << "\n=== TTG ENGINE STARTED ===\nPress ENTER to toggle input mode.\n";

    while (!glfwWindowShouldClose(window)) {
        float currentFrame = (float)glfwGetTime();
        float deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        // 1. INPUT (Polled via callbacks)
        if (!inputMode) {
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camera.ProcessKeyboard(0, deltaTime);
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camera.ProcessKeyboard(1, deltaTime);
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camera.ProcessKeyboard(2, deltaTime);
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camera.ProcessKeyboard(3, deltaTime);
        }

        // 2. UPDATE (Engine Logic)
        engine.Update(deltaTime);

        // 3. RENDER (Visuals)
        glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        shader.use();
        shader.setMat4("projection", glm::perspective(glm::radians(45.0f), (float)SCR_WIDTH / SCR_HEIGHT, 0.1f, 100.0f));
        shader.setMat4("view", camera.GetViewMatrix());
        shader.setVec3("lightPos", glm::vec3(5.0f, 10.0f, 5.0f));

        glBindVertexArray(VAO);

        // Render Loop: Decoupled from logic
        for (const auto& entity : engine.entities) {
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::translate(model, entity.position);
            model = glm::rotate(model, glm::radians(entity.rotation), glm::vec3(0, 1, 0));
            model = glm::scale(model, entity.scale);

            shader.setMat4("model", model);
            shader.setVec3("color", entity.color);
            glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwTerminate();
    return 0;
}