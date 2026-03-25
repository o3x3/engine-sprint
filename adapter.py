import asyncio
import websockets
import json
import math
import random
import os
import sys

ENGINE_PORT = 8080
engine_websocket = None
current_game     = None

# ---------------------------------------------------------------------------
# Asset helper
# ---------------------------------------------------------------------------
def create_prop(id, x, y, z, color, scale, collider="cube"):
    return {
        "jobType": "SPAWN_ENTITY",
        "payload": {
            "id": id,
            "type": "static",
            "transform": {
                "position": [x, y, z],
                "rotation": [0, 0, 0],
                "scale":    scale
            },
            "material": {
                "shader":  "std",
                "texture": "none",
                "color":   color
            },
            "components": {
                "mesh":     collider if collider != "plane" else "plane",
                "collider": collider,
                "script":   ""
            }
        }
    }

def set_mouse(mode: str):
    """Send SET_MOUSE_MODE job. mode = 'capture' | 'free'"""
    return json.dumps({
        "jobType": "SET_MOUSE_MODE",
        "payload": {"mode": mode}
    })

# ---------------------------------------------------------------------------
# Collision module
# ---------------------------------------------------------------------------
# Opt-in AABB collision system. Games feed their entity lists in; the module
# returns resolved positions and hit events. No C++ changes needed - it runs
# entirely on the Python tick.
#
# Usage in a game class:
#   self.collision = CollisionWorld()
#   self.collision.add("ground", 0, -0.5, 0,  scale=[80,1,80], static=True)
#   self.collision.add("player", 0,  1.0, 0,  scale=[0.8,1.8,0.8], static=False)
#
#   In update():
#   hits = self.collision.move("player", new_x, new_y, new_z)
#   resolved_pos = self.collision.get_pos("player")
#   if "ground" in hits: on_ground = True

class AABB:
    """Axis-aligned bounding box. position is the CENTER."""
    __slots__ = ("x", "y", "z", "hw", "hh", "hd")  # half-extents

    def __init__(self, x, y, z, scale):
        self.x  = x;  self.y  = y;  self.z  = z
        self.hw = scale[0] * 0.5
        self.hh = scale[1] * 0.5
        self.hd = scale[2] * 0.5

    def overlaps(self, other) -> bool:
        return (abs(self.x - other.x) < self.hw + other.hw and
                abs(self.y - other.y) < self.hh + other.hh and
                abs(self.z - other.z) < self.hd + other.hd)

    def penetration(self, other):
        """Returns (axis, depth, sign) of minimum penetration axis."""
        dx = self.x - other.x;  px = (self.hw + other.hw) - abs(dx)
        dy = self.y - other.y;  py = (self.hh + other.hh) - abs(dy)
        dz = self.z - other.z;  pz = (self.hd + other.hd) - abs(dz)
        if px < py and px < pz:
            return "x", px, (1 if dx > 0 else -1)
        elif py < pz:
            return "y", py, (1 if dy > 0 else -1)
        else:
            return "z", pz, (1 if dz > 0 else -1)


class CollisionWorld:
    """
    Lightweight swept AABB collision for Python-side game logic.

    - Static bodies: immovable (terrain, obstacles, platforms)
    - Dynamic bodies: can be moved via move(); get resolved against statics
    - No dynamic-vs-dynamic resolution (sufficient for most arcade games)
    """

    def __init__(self):
        self._bodies  = {}   # id -> {"box": AABB, "static": bool, "vel": [vx,vy,vz]}
        self._enabled = True

    def enable(self, on: bool = True):
        self._enabled = on

    def add(self, id: str, x: float, y: float, z: float,
            scale: list, static: bool = False, vel: list = None):
        self._bodies[id] = {
            "box":    AABB(x, y, z, scale),
            "static": static,
            "vel":    vel or [0.0, 0.0, 0.0],
            "scale":  scale,
        }

    def remove(self, id: str):
        self._bodies.pop(id, None)

    def update_pos(self, id: str, x: float, y: float, z: float):
        """Teleport a body without collision (use for static scenery updates)."""
        if id in self._bodies:
            b = self._bodies[id]["box"]
            b.x = x; b.y = y; b.z = z

    def get_pos(self, id: str):
        if id not in self._bodies: return None
        b = self._bodies[id]["box"]
        return (b.x, b.y, b.z)

    def move(self, id: str, nx: float, ny: float, nz: float) -> set:
        """
        Attempt to move dynamic body `id` to (nx, ny, nz).
        Returns a set of body IDs that were hit during resolution.
        The body is repositioned to the resolved location.
        Call get_pos(id) afterwards for the actual position.
        """
        if not self._enabled or id not in self._bodies: return set()
        entry = self._bodies[id]
        if entry["static"]: return set()

        box = entry["box"]
        box.x = nx; box.y = ny; box.z = nz

        hits = set()
        # Resolve against all static bodies (two passes to handle corners)
        for _ in range(2):
            for other_id, other in self._bodies.items():
                if other_id == id or not other["static"]: continue
                other_box = other["box"]
                if not box.overlaps(other_box): continue
                hits.add(other_id)
                axis, depth, sign = box.penetration(other_box)
                if   axis == "x": box.x += depth * sign
                elif axis == "y": box.y += depth * sign
                elif axis == "z": box.z += depth * sign

        return hits

    def clear(self):
        self._bodies.clear()


# ---------------------------------------------------------------------------
# GAME 1: Endless Runner
# ---------------------------------------------------------------------------
class RunnerGame:
    def __init__(self):
        self.active       = True
        self.lane         = 0
        self.z            = 0.0
        self.y            = 0.0
        self.vel_y        = 0.0
        self.score        = 0
        self.spawn_cursor = -20
        self.game_over    = False
        self.obstacles    = []

    async def start(self):
        print("🏃 STARTING RUNNER...")
        await engine_websocket.send(json.dumps({"jobType": "BUILD_SCENE", "payload": {"id": "run"}}))
        await engine_websocket.send(json.dumps(create_prop("player", 0, 0.9, 0, [0, 0.5, 1], [0.8, 1.8, 0.8])))

    async def update(self):
        if not self.active: return
        if self.game_over:
            await engine_websocket.send(json.dumps({"jobType": "UPDATE_UI", "payload": {"title": "CRASHED! [Q] Menu"}}))
            return

        self.z     -= 0.4
        self.score  = int(abs(self.z))

        if self.y > 0 or self.vel_y > 0:
            self.y    += self.vel_y
            self.vel_y -= 0.025
            if self.y <= 0:
                self.y     = 0
                self.vel_y = 0

        player_x = self.lane * 3.0
        for obs in self.obstacles:
            if abs(self.z - obs['z']) < 1.0 and abs(player_x - obs['x']) < 1.0:
                if self.y < 1.0:
                    self.game_over = True

        await engine_websocket.send(json.dumps({"jobType": "UPDATE", "payload": {"id": "player", "position": [player_x, self.y + 0.9, self.z]}}))
        await engine_websocket.send(json.dumps({"jobType": "UPDATE_CAMERA", "payload": {"position": [0, 6, self.z + 10], "lookAt": [0, 0, self.z - 10]}}))
        await engine_websocket.send(json.dumps({"jobType": "UPDATE_UI",     "payload": {"title": f"RUNNER: {self.score} | [A/D] Lane  [SPACE] Jump  [Q] Menu"}}))

        if self.z < self.spawn_cursor + 40:
            self.spawn_cursor -= 15
            await engine_websocket.send(json.dumps(create_prop(f"f_{self.spawn_cursor}", 0, -0.1, self.spawn_cursor, [0.2, 0.2, 0.2], [15, 0.2, 15], "plane")))
            if random.random() > 0.4:
                lane = random.choice([-1, 0, 1]) * 3.0
                await engine_websocket.send(json.dumps(create_prop(f"o_{self.spawn_cursor}", lane, 1.0, self.spawn_cursor, [1, 0, 0], [2.5, 2, 1])))
                self.obstacles.append({'x': lane, 'z': self.spawn_cursor})

        # Prune far-behind obstacles
        self.obstacles = [o for o in self.obstacles if o['z'] > self.z - 5]

    async def handle_input(self, key, action):
        if self.game_over:
            if key == "q" and action == "press": self.active = False
            return
        if action == "press":
            if   key in ("a", "left")  and self.lane > -1: self.lane -= 1
            elif key in ("d", "right") and self.lane <  1: self.lane += 1
            elif key == "space" and self.y == 0:            self.vel_y = 0.45
            elif key == "q":                                self.active = False

# ---------------------------------------------------------------------------
# GAME 2: Side-scroller (Mario style)
# ---------------------------------------------------------------------------
class ScrollerGame:
    def __init__(self):
        self.active        = True
        self.x             = 0.0
        self.y             = 0.0
        self.vel_y         = 0.0
        self.score         = 0
        self.game_over     = False
        self.obstacles     = []
        self.moving_left   = False
        self.moving_right  = False
        self.spawn_cursor  = 10
        self.chunk_size    = 10
        self.on_ground     = True
        self._tasks        = []

    def _send(self, msg):
        t = asyncio.create_task(engine_websocket.send(msg))
        self._tasks.append(t)
        self._tasks = [t for t in self._tasks if not t.done()]

    async def start(self):
        print("👾 STARTING MARIO MODE...")
        await engine_websocket.send(json.dumps({"jobType": "BUILD_SCENE", "payload": {"id": "scroll"}}))
        for i in range(-2, 5):
            self._spawn_chunk(i * self.chunk_size)
        self.spawn_cursor = 40
        await engine_websocket.send(json.dumps(create_prop("player", 0, 0.5, 0, [1, 0.5, 0], [1, 1, 1])))

    def _spawn_chunk(self, x_pos):
        self._send(json.dumps(create_prop(f"f_{x_pos}", x_pos, -0.5, 0, [0.2, 0.3, 0.2], [self.chunk_size, 1, 5], "plane")))
        if random.random() > 0.3:
            h   = random.choice([0.5, 2.5])
            col = [0.8, 0.2, 0.2] if h < 1 else [0.2, 0.2, 0.8]
            self._send(json.dumps(create_prop(f"o_{x_pos}", x_pos, h, 0, col, [1, 1, 1])))
            self.obstacles.append({'x': x_pos, 'y': h, 'top': h + 0.5, 'bottom': h - 0.5})

    async def update(self):
        if not self.active: return
        if self.game_over:
            await engine_websocket.send(json.dumps({"jobType": "UPDATE_UI", "payload": {"title": "DEAD! [Q] Menu"}}))
            return

        if self.moving_right: self.x += 0.2
        if self.moving_left:  self.x -= 0.2
        self.score = max(self.score, int(self.x))

        if not self.on_ground:
            self.vel_y -= 0.025
        self.y += self.vel_y

        self.on_ground = False
        if self.y <= 0:
            self.y = 0; self.vel_y = 0; self.on_ground = True

        pr = {'l': self.x - 0.4, 'r': self.x + 0.4, 'b': self.y, 't': self.y + 1.0}
        for obs in self.obstacles:
            if (pr['r'] > obs['x'] - 0.5 and pr['l'] < obs['x'] + 0.5 and
                    pr['t'] > obs['bottom'] and pr['b'] < obs['top']):
                if self.vel_y <= 0 and self.y >= obs['top'] - 0.3:
                    self.y = obs['top']; self.vel_y = 0; self.on_ground = True
                else:
                    self.game_over = True

        if self.x + 30 > self.spawn_cursor:
            self.spawn_cursor += self.chunk_size
            self._spawn_chunk(self.spawn_cursor)

        await engine_websocket.send(json.dumps({"jobType": "UPDATE",        "payload": {"id": "player", "position": [self.x, self.y + 0.5, 0]}}))
        await engine_websocket.send(json.dumps({"jobType": "UPDATE_CAMERA", "payload": {"position": [self.x, 3, 12], "lookAt": [self.x + 2, 1, 0]}}))
        await engine_websocket.send(json.dumps({"jobType": "UPDATE_UI",     "payload": {"title": f"MARIO: {self.score} | [A/D] Move  [SPACE] Jump  [Q] Menu"}}))

    async def handle_input(self, key, action):
        if self.game_over:
            if key == "q" and action == "press": self.active = False
            return
        if   key in ("a", "left"):  self.moving_left  = (action == "press")
        elif key in ("d", "right"): self.moving_right = (action == "press")
        elif key == "space" and action == "press" and self.on_ground: self.vel_y = 0.6
        elif key == "q" and action == "press": self.active = False

# ---------------------------------------------------------------------------
# GAME 3: Third-person free roam with mouse look
# ---------------------------------------------------------------------------
class FreeRoamGame:
    def __init__(self):
        self.active    = True
        self.x         = 0.0
        self.y         = 0.0
        self.vel_y     = 0.0
        self.on_ground = True
        self.z         = 0.0
        self.yaw       = 0.0   # degrees, horizontal look
        self.pitch     = -15.0 # degrees, vertical tilt of camera
        self.speed     = 0.15
        self.moving    = {"w": False, "s": False, "a": False, "d": False}

    async def start(self):
        print("🌍 STARTING FREE ROAM...")
        # Capture mouse immediately
        await engine_websocket.send(set_mouse("capture"))
        await engine_websocket.send(json.dumps({"jobType": "BUILD_SCENE", "payload": {"id": "roam"}}))

        # Ground
        await engine_websocket.send(json.dumps(create_prop(
            "ground", 0, -0.5, 0, [0.25, 0.45, 0.25], [80, 1, 80], "plane"
        )))

        # Scatter props
        random.seed(42)
        colors = [
            [0.8, 0.3, 0.2], [0.2, 0.4, 0.8], [0.7, 0.6, 0.2],
            [0.5, 0.2, 0.7], [0.2, 0.7, 0.5], [0.8, 0.5, 0.2],
        ]
        for i in range(30):
            bx  = random.uniform(-25, 25)
            bz  = random.uniform(-25, 25)
            # keep a clear spawn area
            if abs(bx) < 3 and abs(bz) < 3:
                bz += 6
            h   = random.uniform(1.0, 5.0)
            col = random.choice(colors)
            w   = random.uniform(1.0, 3.0)
            await engine_websocket.send(json.dumps(create_prop(
                f"box_{i}", bx, h * 0.5, bz, col, [w, h, w]
            )))

        # Player stand-in (visible from behind)
        await engine_websocket.send(json.dumps(create_prop(
            "player", 0, 1.0, 0, [0.2, 0.5, 1.0], [0.8, 1.8, 0.8]
        )))

        await engine_websocket.send(json.dumps({"jobType": "UPDATE_UI", "payload": {
            "title": "FREE ROAM | WASD move | Mouse look | ESC release mouse | [Q] Menu"
        }}))

    async def update(self):
        if not self.active: return

        # Movement relative to yaw
        rad    = math.radians(self.yaw)
        fwd_x  =  math.sin(rad)
        fwd_z  = -math.cos(rad)
        rgt_x  =  math.cos(rad)
        rgt_z  =  math.sin(rad)

        if self.moving["w"]: self.x += fwd_x * self.speed; self.z += fwd_z * self.speed
        if self.moving["s"]: self.x -= fwd_x * self.speed; self.z -= fwd_z * self.speed
        if self.moving["a"]: self.x -= rgt_x * self.speed; self.z -= rgt_z * self.speed
        if self.moving["d"]: self.x += rgt_x * self.speed; self.z += rgt_z * self.speed

        # Gravity — same euler integration as runner/scroller
        self.vel_y    -= 0.025
        self.y        += self.vel_y
        self.on_ground = False
        if self.y <= 0.0:
            self.y        = 0.0
            self.vel_y    = 0.0
            self.on_ground = True

        # Third-person camera: orbit around player using yaw + pitch
        cam_dist   = 6.0
        pitch_rad  = math.radians(self.pitch)
        # horizontal distance scales with cos(pitch)
        h_dist     = cam_dist * math.cos(pitch_rad)
        cam_x      = self.x - fwd_x * h_dist
        cam_z      = self.z - fwd_z * h_dist
        cam_y      = self.y + 1.0 + cam_dist * math.sin(-pitch_rad)

        await engine_websocket.send(json.dumps({
            "jobType": "UPDATE",
            "payload": {"id": "player", "position": [self.x, self.y + 0.9, self.z]}
        }))
        await engine_websocket.send(json.dumps({
            "jobType": "UPDATE_CAMERA",
            "payload": {
                "position": [cam_x, cam_y, cam_z],
                "lookAt":   [self.x, self.y + 1.0, self.z]
            }
        }))

    async def handle_input(self, key, action):
        pressed  = (action == "press")
        released = (action == "release")

        if key in self.moving:
            if pressed:  self.moving[key] = True
            if released: self.moving[key] = False

        # Jump: set vel_y once; gravity in update() handles the arc
        if key == "space" and pressed and self.on_ground:
            self.vel_y = 0.45

        if key == "q" and pressed:
            await engine_websocket.send(set_mouse("free"))
            self.active = False

    async def handle_mouse(self, dx, dy):
        """Called from route_input when a 'mouse' telemetry event arrives."""
        self.yaw   += dx           # dx already scaled by MOUSE_SENSITIVITY in C++
        self.pitch  = max(-60.0, min(30.0, self.pitch - dy))  # clamp: don't flip over

# ---------------------------------------------------------------------------
# Main menu
# ---------------------------------------------------------------------------
_menu_queue = asyncio.Queue()

async def main_menu_loop():
    global current_game
    print("⏳ Waiting for engine connection...")
    while engine_websocket is None:
        await asyncio.sleep(0.1)

    while True:
        print("\n=== GAME CONSOLE ===")
        await engine_websocket.send(json.dumps({"jobType": "BUILD_SCENE",    "payload": {"id": "menu"}}))
        await engine_websocket.send(json.dumps({"jobType": "UPDATE_CAMERA",  "payload": {"position": [0, 2, 10], "lookAt": [0, 0, 0]}}))
        await engine_websocket.send(json.dumps({"jobType": "UPDATE_UI",      "payload": {
            "title": "MENU: [W] Runner  [S] Mario  [D] Free Roam  [Q] Quit"
        }}))

        choice = await _menu_queue.get()

        if   choice == 1: current_game = RunnerGame()
        elif choice == 2: current_game = ScrollerGame()
        elif choice == 3: current_game = FreeRoamGame()
        elif choice == "QUIT":
            print("🛑 Shutting down...")
            os.system("taskkill /f /im ttg.exe 2>nul")
            sys.exit(0)
        else:
            continue

        await current_game.start()
        while current_game.active:
            await current_game.update()
            await asyncio.sleep(0.016)  # ~60 Hz logic tick
        current_game = None

# ---------------------------------------------------------------------------
# Input router
# ---------------------------------------------------------------------------
async def route_input(key, action):
    if current_game is None:
        if action == "press":
            if   key == "w": await _menu_queue.put(1)
            elif key == "s": await _menu_queue.put(2)
            elif key == "d": await _menu_queue.put(3)
            elif key == "q": await _menu_queue.put("QUIT")
        return
    await current_game.handle_input(key, action)

async def route_mouse(dx, dy):
    if current_game is not None and hasattr(current_game, "handle_mouse"):
        await current_game.handle_mouse(dx, dy)

# ---------------------------------------------------------------------------
# WebSocket server (engine connects here)
# ---------------------------------------------------------------------------
async def engine_handler(websocket):
    global engine_websocket
    engine_websocket = websocket
    print("✅ Engine connected!")
    try:
        async for message in websocket:
            try:
                data = json.loads(message)
                if data.get("type") == "TELEMETRY":
                    event = data.get("event")
                    if event == "input":
                        await route_input(
                            data["data"]["key"].lower(),
                            data["data"]["action"]
                        )
                    elif event == "mouse":
                        await route_mouse(
                            data["data"]["dx"],
                            data["data"]["dy"]
                        )
            except (json.JSONDecodeError, KeyError) as e:
                print(f"Message parse error: {e}")
    except websockets.exceptions.ConnectionClosed:
        print("Engine disconnected.")
    finally:
        engine_websocket = None

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
async def main():
    print(f"📡 Logic server listening on ws://localhost:{ENGINE_PORT}")
    server = await websockets.serve(engine_handler, "localhost", ENGINE_PORT)
    asyncio.create_task(main_menu_loop())
    await asyncio.Future()  # run forever

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServer stopped.")