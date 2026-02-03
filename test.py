import asyncio
import websockets
import json
import time

async def stress_test(websocket):
    print(f"[Rudra] Starting Day 3 Stress Test...")
    
    # 1. Setup
    await websocket.send(json.dumps({ "command": "START" }))
    await websocket.send(json.dumps({ "jobType": "BUILD_SCENE", "payload": { "id": "stress_scene", "ambientLight": [0.5, 0.5, 0.5], "skybox": "none" } }))
    
    # 2. STRESS TEST: Spam 50 entities instantly
    # Engine MAX_JOBS_PER_FRAME is 10. This should take 5 frames to process.
    print(f"[Rudra] Spamming 50 entities instantly...")
    for i in range(50):
        await websocket.send(json.dumps({
            "jobType": "SPAWN_ENTITY",
            "payload": {
                "id": f"cube_{i}",
                "type": "object",
                "transform": { "position": [i*0.5, 0, 0], "rotation": [0,0,0], "scale": [0.2, 0.2, 0.2] },
                "material": { "color": [1, 0, 0], "shader": "std", "texture": "none" },
                "components": { "mesh": "box", "collider": "box", "script": "" }
            }
        }))
    print("[Rudra] Spam complete. Engine should not freeze.")
    
    await asyncio.sleep(2) # Wait for processing

    # 3. BUCKET SNAPSHOT TEST
    print(f"[Rudra] Requesting Bucket Snapshot...")
    await websocket.send(json.dumps({ "command": "SNAPSHOT" }))

    # 4. Monitor
    async for message in websocket:
        data = json.loads(message)
        if data.get("event") == "bucket_write":
            print(f"[SUCCESS] Snapshot confirmed: {data['data']['file']}")
            break

async def main():
    async with websockets.serve(stress_test, "localhost", 8080):
        print("--- DAY 3 STRESS TEST READY ---")
        await asyncio.get_running_loop().create_future()

if __name__ == "__main__":
    asyncio.run(main())