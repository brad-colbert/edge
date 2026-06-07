#ifndef ENGINE_VERSION_H
#define ENGINE_VERSION_H

// Canonical EDGE version — the single source of truth.
//
// CMakeLists.txt parses these macros (file(STRINGS) + regex) so project()
// stays in sync; do not duplicate the number elsewhere. Bump here, or run
// scripts/bump-version.sh, then add a CHANGELOG.md entry and tag the release.
//
// EDGE follows Semantic Versioning (https://semver.org/): MAJOR.MINOR.PATCH.

#define EDGE_VERSION_MAJOR 0
#define EDGE_VERSION_MINOR 4
#define EDGE_VERSION_PATCH 1
#define EDGE_VERSION_STRING "0.4.1"

#endif // ENGINE_VERSION_H
