import asyncio
import websockets
import json
import math

async def demo_director(websocket):
    print(f" Connected. Starting Day 2 Integration Demo...")
    
    # 1. RESET & START (Deterministic State)
    await websocket.send(json.dumps({ "command": "RESET" }))
    await asyncio.sleep(0.2)
    await websocket.send(json.dumps({ "command": "START" }))
    print(" Engine Reset & Started.")

    # 2. BUILD SCENE (Red Atmospheric Light)
    await websocket.send(json.dumps({
        "jobType": "BUILD_SCENE",
        "payload": { "id": "demo_scene", "ambientLight": [0.6, 0.2, 0.2], "skybox": "none" }
    }))
    print(" Scene Built.")

    # 3. SPAWN MULTIPLE ENTITIES (Asset Loading Test)
    # Entity A: The "Player" (Green Cube)
    await websocket.send(json.dumps({
        "jobType": "SPAWN_ENTITY",
        "payload": {
            "id": "player", "type": "player",
            "transform": { "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1] },
            "material": { "shader": "std", "texture": "none", "color": [0, 1, 0] },
            "components": { "mesh": "box", "collider": "box", "script": "" }
        }
    }))
    
    # Entity B: The "Target" (Blue Sphere - visualized as box if sphere missing)
    await websocket.send(json.dumps({
        "jobType": "SPAWN_ENTITY",
        "payload": {
            "id": "target", "type": "npc",
            "transform": { "position": [3, 1, 0], "rotation": [45, 45, 0], "scale": [0.5, 0.5, 0.5] },
            "material": { "shader": "std", "texture": "none", "color": [0, 0.5, 1] },
            "components": { "mesh": "sphere", "collider": "sphere", "script": "" }
        }
    }))
    print(" Entities Spawned.")

    # 4. ACTION LOOP (Action Injection Test)
    print(" Injecting Actions (Circular Motion)...")
    for i in range(60):
        # Calculate circular position
        angle = i * 0.2
        x = math.cos(angle) * 3
        z = math.sin(angle) * 3
        
        # Action: UPDATE absolute position for Target
        await websocket.send(json.dumps({
            "jobType": "UPDATE",
            "payload": { "id": "target", "position": [x, 1, z] }
        }))
        
        # Action: MOVE relative for Player (Jitter)
        await websocket.send(json.dumps({
            "command": "ACTION",
            "action": "MOVE",
            "payload": { "id": "player", "vec": [0, math.sin(i)*0.05, 0] }
        }))

        await asyncio.sleep(0.05)

    # 5. TELEMETRY CHECK
    print(" Demo Complete. Listening for final heartbeat...")
    async for message in websocket:
        data = json.loads(message)
        if data.get("event") == "tick_update":
            print(f" Final Telemetry Received: FPS {data['data']['fps']:.2f}")
            break

async def main():
    async with websockets.serve(demo_director, "localhost", 8080):
        print("--- DIRECTOR MODE READY ---")
        await asyncio.get_running_loop().create_future()

if __name__ == "__main__":
    asyncio.run(main())