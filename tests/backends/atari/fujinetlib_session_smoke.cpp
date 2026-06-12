// fujinetlib_session_smoke.cpp — optional ON-mode compile/link smoke for adapter declarations.

#include <engine/platform/atari/fujinet_session_fujinetlib.h>

int main() {
#if defined(EDGE_ATARI_FUJINET_SESSION_FUJINETLIB)
    atari::fujinet_session::FujinetLibSessionAdapter::declaration_smoke_check();
    return atari::fujinet_session::FujinetLibSessionAdapter::state_size_bytes() > 0 ? 0 : 1;
#else
    return 0;
#endif
}
