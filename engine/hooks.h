#ifndef ENGINE_HOOKS_H
#define ENGINE_HOOKS_H

// hooks.h — render-phase hook storage (ARCHITECTURE.md "Data Flow Per Frame").
//
// Two optional engine callbacks the game can install to run engine-defined work
// at fixed points in the frame:
//   - pre_sprite_commit: runs inside the VBI, immediately before the sprite
//     manager commits buffered positions to player/missile memory. Use it to
//     finalise sprite state with the previous frame's commit already latched.
//   - post_render: runs in the game loop, immediately after the per-frame
//     callback returns. Use it for bookkeeping that must follow the render.
//
// Both are plain function pointers, nullable (a null hook is skipped), and must
// be non-capturing functions or lambdas — capture state lives in statics
// (DECISIONS.md ADR-020), exactly as DLI/VBI handlers do. This header depends on
// nothing.

namespace engine {

struct Hooks {
    void (*pre_sprite_commit)() = nullptr;   // VBI, before sprite commit
    void (*post_render)()       = nullptr;   // loop, after the frame callback
};

} // namespace engine

#endif // ENGINE_HOOKS_H
