import asyncio
import websockets
import json

async def failure_server(websocket):
    print("[FailureTest] Engine Connected. Sending Garbage...")
    await asyncio.sleep(2)

    # TEST 1: Malformed JSON (Syntax Error)
    # Expected: Engine catches error, prints log, DOES NOT CRASH.
    print("[FailureTest] Sending Broken JSON...")
    await websocket.send("{ 'type': 'SPAWN_ENTITIES', 'payload': ... oops forgot close brace") 

    await asyncio.sleep(1)

    # TEST 2: Missing Data (No ID)
    # Expected: Engine catches "key not found", ignores job.
    print("[FailureTest] Sending Job with Missing Fields...")
    await websocket.send(json.dumps({
        "jobType": "SPAWN_ENTITIES",
        "payload": {
            "type": "npc",
            # "id" is MISSING
            "transform": { "position": [0,0,0], "rotation":[0,0,0], "scale":[1,1,1] }
        }
    }))

    await asyncio.sleep(1)

    # TEST 3: Logic Bomb (Scale 0)
    # Expected: Engine handles it (invisible object) or clamps it, but NO crash (division by zero).
    print("[FailureTest] Sending Zero Scale...")
    await websocket.send(json.dumps({
        "jobType": "SPAWN_ENTITIES",
        "payload": {
            "id": "zero_scale_cube",
            "type": "object",
            "transform": { "position": [2,0,0], "rotation":[0,0,0], "scale":[0,0,0] }, 
            "material": { "color": [1,0,0] },
            "components": { "mesh": "box", "collider": "box" }
        }
    }))

    print("[FailureTest] Done. If Engine is still running, PASS.")

async def main():
    async with websockets.serve(failure_server, "localhost", 8080):
        await asyncio.get_running_loop().create_future()

if __name__ == "__main__":
    asyncio.run(main())