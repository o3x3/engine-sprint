import asyncio
import websockets
import json

async def rudra_server(websocket):
    print(f" Engine connected!")
    
    try:
        async for message in websocket:
            print(f" Received from Engine: {message}")
            
            # 1. Wait 2 seconds, then Build the Scene (Optional but good)
            await asyncio.sleep(2)
            print(" Sending BUILD_SCENE...")
            scene_job = {
                "jobType": "BUILD_SCENE",  # <--- UPDATED KEY
                "payload": {
                    "sceneId": "scene_net_test",
                    "ambientLight": [0.8, 0.2, 0.2], # Red ambient light to prove it works
                    "skybox": "none"
                }
            }
            await websocket.send(json.dumps(scene_job))

            # 2. Wait 1 second, then SPAWN an entity
            await asyncio.sleep(1)
            print(" Sending SPAWN_ENTITIES...")
            
            spawn_job = {
                "jobType": "SPAWN_ENTITIES", # <--- UPDATED KEY
                "payload": {
                    "id": "net_cube_01",
                    "type": "object",
                    "transform": {
                        "position": [0, 0, -5], 
                        "rotation": [0, 45, 0], 
                        "scale": [1, 1, 1]
                    },
                    "material": {
                        "shader": "standard", 
                        "texture": "none", 
                        "color": [0, 1, 0] # Green
                    },
                    "components": {
                        "mesh": "cube", 
                        "collider": "box", 
                        "script": "none"
                    }
                }
            }
            await websocket.send(json.dumps(spawn_job))
            
            # 3. Wait 2 seconds, then MOVE it
            await asyncio.sleep(2)
            print(" Sending UPDATE...")
            move_job = {
                "jobType": "UPDATE",
                "payload": {
                    "id": "net_cube_01",
                    "position": [2, 0, -5] # Move Right
                }
            }
            await websocket.send(json.dumps(move_job))

    except websockets.exceptions.ConnectionClosed:
        print(" Engine disconnected.")

async def main():
    async with websockets.serve(rudra_server, "localhost", 8080):
        print("--- MOCK SERVER RUNNING (ws://localhost:8080) ---")
        await asyncio.get_running_loop().create_future()

if __name__ == "__main__":
    asyncio.run(main())