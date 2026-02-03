# TTG Engine

**Job-Based Runtime for AI-Driven Gameplay**

Version 1.0 | C++17 | OpenGL 3.3 | WebSockets

---

## Overview

The TTG Engine is a specialized 3D runtime designed for the BHIV Gaming Stack. Unlike traditional engines (Unity/Unreal), it does not contain game logic. Instead, it acts as a "dumb terminal" that consumes a stream of **Jobs** from an external AI Control Plane (Rudra) via WebSockets.

### Key Features

- **AI-Driven Gameplay**: Game logic lives entirely in Python/AI models
- **Deterministic Replay**: Record job streams and replay them frame-perfectly
- **Cloud-Ready**: Rendering client decouples from logic server
- **Resilient**: Handles malformed data without crashing

---

## Architecture

The engine follows a **Consumer-Producer** pattern:

```
[AI Control Plane (Python)] → WebSocket (JSON) → [Engine Job Queue (C++)] → [Renderer]
```

### Core Systems

- **Network Client**: Maintains persistent connection to `ws://localhost:8080/engine` using ixwebsocket
- **Job Execution Layer**: Parses incoming JSON packets (`SPAWN`, `UPDATE`, `BUILD_SCENE`) and executes them
- **Telemetry Hooks**: Emits status updates (`entity_spawned`, `tick_update`) back to server
- **Replay System**: Records network traffic to disk and supports offline, deterministic playback

---

## Quick Start

### 1. Demo Mode (Recommended)

Launches the full stack in presentation mode (fullscreen, no debug logs):

```bash
run_demo.bat
```

**Expected**: Red scene loads, green cube spawns and moves automatically.

### 2. Manual Development Mode

Run components individually for debugging:

```bash
# Terminal 1: Start Server
python mock_server.py

# Terminal 2: Start Engine
ttg.exe
```

### 3. Replay Mode (Offline)

Prove determinism by running without the server using a recorded session:

```bash
ttg.exe --replay
```

Runs at 1 step/second (slow-mo) to visualize job queue execution.

### 4. Failure Test

Prove resilience by bombarding the engine with garbage data:

```bash
# Terminal 1: Start Chaos Server
python failure_test.py

# Terminal 2: Start Engine
ttg.exe
```

**Expected**: Console prints error logs, but window remains open.

---

## Protocol Documentation

### Incoming Jobs (Server → Engine)

#### 1. BUILD_SCENE
Resets the level and sets lighting.

```json
{
  "jobType": "BUILD_SCENE",
  "payload": {
    "sceneId": "level_1",
    "ambientLight": [0.8, 0.2, 0.2]
  }
}
```

#### 2. SPAWN_ENTITIES
Creates a new object in the world.

```json
{
  "jobType": "SPAWN_ENTITIES",
  "payload": {
    "id": "enemy_01",
    "type": "npc",
    "transform": {
      "position": [0, 0, 0],
      "rotation": [0, 0, 0],
      "scale": [1, 1, 1]
    },
    "material": { "color": [0, 1, 0] },
    "components": { "collider": "box" }
  }
}
```

#### 3. UPDATE
Teleports or moves an existing entity.

```json
{
  "jobType": "UPDATE",
  "payload": {
    "id": "enemy_01",
    "position": [2.5, 0, 0]
  }
}
```

### Outgoing Telemetry (Engine → Server)

- **entity_spawned**: Confirmation of entity creation
- **tick_update**: Sent every 60 frames (includes FPS)
- **job_completed**: Confirmation of async tasks

---

## Dependencies & Build

### Requirements

- **Compiler**: Visual Studio 2022 (MSVC v143)
- **Standard**: C++17 (required for `<filesystem>`)
- **Python**: 3.x (for mock server)

### Libraries

- **GLFW 3**: Windowing
- **GLAD**: OpenGL loader
- **GLM**: Math
- **nlohmann/json**: JSON parsing
- **ixwebsocket**: Networking

### Build Instructions

1. Open `ttg.sln` in Visual Studio
2. Set configuration to **Release**
3. Ensure `run_demo.bat` paths match your build output location
4. Build solution

---

## Technical Specifications

| Component | Technology |
|-----------|-----------|
| Language | C++17 |
| Graphics API | OpenGL 3.3 |
| Network Protocol | WebSocket (ixwebsocket) |
| Data Format | JSON |
| Architecture | External-Driven / Data-Oriented |
