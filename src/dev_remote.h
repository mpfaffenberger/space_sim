#pragma once
// -----------------------------------------------------------------------------
// dev_remote.h — tiny in-game HTTP control server for dev tooling.
//
// Lets an external process (Code Puppy, a shell script, curl) drive the
// camera, grab screenshots, and inspect runtime state while the game is
// running. Only bound to 127.0.0.1 so we don't expose anything to the
// network. Intended for development — in a ship build we'd compile it
// out entirely.
//
// Threading model
// ---------------
// A single background thread runs the HTTP accept loop. Incoming
// requests mutate nothing directly — they enqueue Commands to a
// mutex-guarded deque which the main thread drains once per frame via
// `drain_commands()`. Commands that need a reply (e.g. screenshot,
// state query) carry a condition variable the HTTP thread blocks on
// until the main thread fulfills them. This keeps every game-state
// mutation on the main thread, so nothing else in the renderer needs
// to know about thread safety.
//
// Endpoints (all respond with JSON unless noted)
//   GET  /state          → { pos, euler, fps, system }
//   POST /camera/set     → { x, y, z, yaw, pitch, roll }   (all optional)
//   POST /screenshot     → saves a PNG to /tmp/np_shot.png,
//                          returns { path, ok }
//
// Everything else 404s.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

struct Camera;

namespace dev_remote {

// Bring the server up on the given port. No-op if already running or if
// binding fails (e.g. another instance of the game is still listening).
// The failure path is non-fatal on purpose — the game should keep running
// even if the dev channel can't open.
void start(int port = 8765);

// Tear down the server cleanly. Safe to call unconditionally at shutdown.
void stop();

// Apply queued commands to game state. Call once per frame from the
// main thread, before you query the camera to render.
void drain_commands(Camera& cam);

// Publish the latest FPS reading for `/state` queries. Called whenever
// the game recomputes it (once per second is enough).
void publish_fps(int fps);

// End-of-frame hook. If a screenshot command is in flight, this is
// where it gets taken (after the current frame's swap has completed)
// and the requesting HTTP thread is woken up. Must be called AFTER
// `sg_commit()` so the final composited frame is on the window.
void maybe_capture_screenshot();

// For `/state` — main thread calls this each frame to publish the
// current system name (we don't hold a reference to the StarSystem
// to keep coupling low).
void publish_system_name(const char* name);

} // namespace dev_remote
