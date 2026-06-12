#ifndef ENGINE_NET_H
#define ENGINE_NET_H

// net.h — generic networking support layer (Stage 3).
//
// This header intentionally provides only backend-neutral support types and
// fixed-size rings. No transport integration (FujiNet/Netstream/CIO) is
// included here.

#include "net_types.h"
#include "net_ring.h"
#include "net_api.h"

namespace engine {

namespace net {
// Namespace anchor for generic net support types.
}

} // namespace engine

#endif // ENGINE_NET_H
