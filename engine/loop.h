#ifndef ENGINE_LOOP_H
#define ENGINE_LOOP_H

// loop.h — the game loop (ARCHITECTURE.md "Game Loop").
//
// The engine's VBI service (engine/core.h) sets a frame-ready flag once per
// vertical blank. The loop polls that flag — the standard Atari "wait for VBI"
// pattern — and runs the game callback exactly once per frame, so game logic is
// frame-synchronised without the game author touching the interrupt. The render
// phase writes to buffers during the visible frame; the next VBI commits them
// (one-frame buffering, ADR-009).
//
// These are free function templates parameterised on the Core type so they can
// reach Core::frame_ready_, Core::input, and Core::hooks without loop.h
// depending on a concrete Core. engine::Core exposes thin static forwarders
// (run/run_until/frame_overrun) so game code writes `Game::run(...)`.
//
// NOTE: under `mos-sim` there is no VBI to set the flag, so run()/run_until()
// would spin forever — they are compile-only there and are never invoked by the
// unit tests, matching how the live interrupt path is exercised on hardware.
//
// Depends on nothing (the Core type is a template parameter).

namespace engine {

// Run `cb` once per frame forever. Each iteration waits for the VBI to set the
// frame-ready flag, clears it, runs the callback with the input snapshot, then
// runs the post-render hook if installed.
template <typename Core, typename Cb>
[[noreturn]] void run(Cb cb) {
    for (;;) {
        while (!Core::frame_ready_) { }
        Core::frame_ready_ = false;
        cb(Core::input);
        if (Core::hooks.post_render) Core::hooks.post_render();
    }
}

// As run(), but `cb` returns bool; the loop exits and returns when it is true.
template <typename Core, typename Cb>
void run_until(Cb cb) {
    for (;;) {
        while (!Core::frame_ready_) { }
        Core::frame_ready_ = false;
        const bool done = cb(Core::input);
        if (Core::hooks.post_render) Core::hooks.post_render();
        if (done) return;
    }
}

// True if a VBI already fired before the previous frame finished (the flag is
// set again while the loop is still mid-callback): the frame overran its budget.
template <typename Core>
bool frame_overrun() {
    return Core::frame_ready_;
}

} // namespace engine

#endif // ENGINE_LOOP_H
