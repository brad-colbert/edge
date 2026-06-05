#ifndef ENGINE_PLATFORM_ATARI_DISPLAY_TRAITS_H
#define ENGINE_PLATFORM_ATARI_DISPLAY_TRAITS_H

// platform/atari/display_traits.h — the Atari specialisation of the portable
// display trait seam (engine/display_traits.h).
//
// This is what binds the generic display layer to ANTIC mode geometry. It
// specialises `engine::display::traits<atari::Mode>` by forwarding to the
// constexpr helpers in antic.h (the single source of truth for ANTIC geometry).
// platform.h includes this header, so any game/test that uses the Atari platform
// pulls the specialisation in before it instantiates a DisplayLayout.
//
// Keeping the specialisation here (not in antic.h) lets antic.h stay a pure
// hardware-vocabulary header with no dependency on the engine display layer.

#include "../../display_traits.h"
#include "antic.h"

namespace engine {
namespace display {

template <>
struct traits<atari::Mode> {
    static constexpr bool is_text(atari::Mode m) { return atari::is_text(m); }
    static constexpr bool is_vbxe(atari::Mode m) { return atari::is_vbxe(m); }
    static constexpr engine::u16 bytes_per_line(atari::Mode m) {
        return atari::bytes_per_line(m);
    }
    static constexpr engine::u8 bits_per_pixel(atari::Mode m) {
        return atari::bits_per_pixel(m);
    }
    static constexpr engine::u8 scanlines_per_line(atari::Mode m) {
        return atari::scanlines_per_line(m);
    }
    static constexpr engine::u8 mode_opcode(atari::Mode m) {
        return atari::dl_mode_byte(m);
    }
    static constexpr engine::u8 fine_scroll_range(atari::Mode m) {
        return atari::fine_scroll_range(m);
    }
    static constexpr engine::u16 scroll_fetch_width(atari::Mode m) {
        return atari::scroll_fetch_width(m);
    }
    // ANTIC register conventions: raising HSCROL moves the picture opposite to the
    // LMS (coarse) pointer, so horizontal fine scroll inverts; VSCROL moves it the
    // same way as the LMS pointer, so vertical does not. (See platform/atari/antic.h.)
    static constexpr bool fine_scroll_inverts_x(atari::Mode) { return true; }
    static constexpr bool fine_scroll_inverts_y(atari::Mode) { return false; }
    static constexpr engine::u8 to_screen_code(char c) {
        return atari::ascii_to_internal(c);
    }
};

} // namespace display
} // namespace engine

#endif // ENGINE_PLATFORM_ATARI_DISPLAY_TRAITS_H
