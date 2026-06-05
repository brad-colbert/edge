#ifndef ENGINE_DISPLAY_TRAITS_H
#define ENGINE_DISPLAY_TRAITS_H

// display_traits.h — the backend-neutral seam between the portable display layer
// and a platform's display modes.
//
// engine/display.h parameterises its region and view templates on a backend mode
// token (an enum value) and derives all per-mode geometry through
// `engine::display::traits<ModeT>`. The primary template is intentionally left
// undefined: a backend MUST provide a full specialisation for its mode enum (the
// backend ships it in a platform-owned header that the game includes via its
// platform header). A missing specialisation is then a hard compile error at the
// point a layout is instantiated, rather than a silent fallback.
//
// Required static members of a specialisation `traits<ModeT>` (all constexpr):
//   static constexpr bool is_text(ModeT);            // text vs bitmap mode
//   static constexpr bool is_vbxe(ModeT);            // overlay (VBXE) vs base mode
//   static constexpr u16  bytes_per_line(ModeT);     // screen-memory width
//   static constexpr u8   bits_per_pixel(ModeT);     // bitmap packing (text: n/a)
//   static constexpr u8   scanlines_per_line(ModeT); // mode-line height
//   static constexpr u8   mode_opcode(ModeT);        // backend display-program byte
//   static constexpr u8   to_screen_code(char);      // ASCII -> screen tile code
//   static constexpr u8   fine_scroll_range(ModeT);  // horizontal fine-scroll modulus
//   static constexpr u16  scroll_fetch_width(ModeT); // cells fetched per scrolled line
//   static constexpr bool fine_scroll_inverts_x(ModeT); // fine X register opposes coarse
//   static constexpr bool fine_scroll_inverts_y(ModeT); // fine Y register opposes coarse
//                                                    //   (these four only used by scroll-active screens)
//
// Depends only on types.h.

#include "types.h"

namespace engine {
namespace display {

// Primary template — declared, never defined. The backend supplies a full
// specialisation for its mode enum (see the header note above).
template <typename ModeT>
struct traits;

} // namespace display
} // namespace engine

#endif // ENGINE_DISPLAY_TRAITS_H
