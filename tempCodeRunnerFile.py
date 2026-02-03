import asyncio
import websockets
import json

async def rudra_server(websocket):
    print(f"[Rudra] Engine connected!")
    
    try:
        # 1. LIFECYCLE
        await websocket.send(json.dumps({ "command": "RESET" }))
        await asyncio.sleep(0.5)
        await websocket.send(json.dumps({ "command": "START" }))
        
        # 2. SCENE
        await asyncio.sleep(1)
        await websocket.send(json.dumps({
            "jobType": "BUILD_SCENE",
            "payload": { "id": "scene_01", "ambientLight": [0.8, 0.2, 0.2], "skybox": "daylight" }
        }))

        # 3. SPAWN
        await asyncio.sleep(1)
        await websocket.send(json.dumps({
            "jobType": "SPAWN_ENTITY",
            "payload": {
                "id": "cube_01", "type": "object",
                "transform": { "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1] },
                "material": { "shader": "standard", "texture": "none", "color": [0, 1, 0] },
                "components": { "mesh": "box", "collider": "box", "script": "none" }
            }
        }))
        
        # 4. ACTION INJECTION (Day 1c Feature)
        # Instead of absolute UPDATE, we send a relative MOVE action
        await asyncio.sleep(1)
        print("[Rudra] Sending ACTION: MOVE (Relative +2 Y)...")
        await websocket.send(json.dumps({
            "command": "ACTION",
            "action": "MOVE",
            "payload": {
                "id": "cube_01",
                "vec": [0, 2, 0] # Move UP by 2 units
            }
        }))

        # 5. ACTION INJECTION (Interact)
        await asyncio.sleep(1)
        print("[Rudra] Sending ACTION: INTERACT...")
        await websocket.send(json.dumps({
            "command": "ACTION",
            "action": "INTERACT",
            "payload": { "id": "cube_01" }
        }))

        await asyncio.get_running_loop().create_future()

    except websockets.exceptions.ConnectionClosed:
        print("[Rudra] Engine disconnected.")

async def main():
    async with websockets.serve(rudra_server, "localhost", 8080):
        print("--- RUDRA CONTROL PLANE (Day 1c Actions) ---")
        await asyncio.get_running_loop().create_future()

if __name__ == "__main__":
    asyncio.run(main())