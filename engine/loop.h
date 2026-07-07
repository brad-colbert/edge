#ifndef ENGINE_LOOP_H
#define ENGINE_LOOP_H

// loop.h — the game loop (ARCHITECTURE.md "Game Loop").
//
// The engine's frame service (engine/core.h) sets a frame-ready flag once per
// frame interrupt. The loop polls that flag — the standard "wait for vertical
// blank" pattern — and runs the game callback exactly once per frame, so game
// logic is frame-synchronised without the game author touching the interrupt. The
// render phase writes to buffers during the visible frame; the next frame service
// commits them (one-frame buffering, ADR-009).
//
// These are free function templates parameterised on the Core type so they can
// reach Core::frame_ready_, Core::input, and Core::hooks without loop.h
// depending on a concrete Core. engine::Core exposes thin static forwarders
// (run/run_until/frame_overrun) so game code writes `Game::run(...)`.
//
// NOTE: under `mos-sim` there is no frame interrupt to set the flag, so
// run()/run_until() would spin forever — they are compile-only there and are
// never invoked by the unit tests, matching how the live interrupt path is
// exercised on hardware.
//
// Depends on nothing (the Core type is a template parameter).

namespace engine {

// Compiler memory barrier. The frame service (engine/core.h) is an asynchronous
// interrupt the optimiser cannot see being called — its address is only handed
// to install_frame_isr — so under whole-program optimisation it assumes the state
// the service writes (Core::input, Core::collisions_, …) is never modified between
// the loop's reads and would constant-fold those reads to the initial (zero) value.
// `frame_ready_` is volatile so the spin itself is honoured, but a volatile load
// is not a general barrier. This clobber forces the compiler to reload every
// service-updated object after the frame is released, before the callback reads it.
[[gnu::always_inline]] inline void sync_after_vbi() {
    __asm__ volatile("" ::: "memory");
}

// Run `cb` once per frame forever. Each iteration waits for the frame interrupt
// to set the frame-ready flag, clears it, runs the callback with the input
// snapshot, then runs the post-render hook if installed.
template <typename Core, typename Cb>
[[noreturn]] void run(Cb cb) {
    for (;;) {
        while (!Core::frame_ready_) { }
        Core::frame_ready_ = false;
        Core::frame_consumed();   // release the VBI guard now the frame is consumed
        sync_after_vbi();
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
        Core::frame_consumed();   // release the VBI guard now the frame is consumed
        sync_after_vbi();
        const bool done = cb(Core::input);
        if (Core::hooks.post_render) Core::hooks.post_render();
        if (done) return;
    }
}

// True if a frame interrupt already fired before the previous frame finished (the
// flag is set again while the loop is still mid-callback): the frame overran its
// budget.
template <typename Core>
bool frame_overrun() {
    return Core::frame_ready_;
}

} // namespace engine

#endif // ENGINE_LOOP_H
