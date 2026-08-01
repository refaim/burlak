// Single source of truth for the version. Bump here; the plugin's Far version,
// the DLL resource and the release tag all read from this.
//
// Semantic versioning: MAJOR for changes that break how the plugin is used,
// MINOR for new behaviour, PATCH for fixes.
#pragma once

#define BURLAK_VERSION_MAJOR 1
#define BURLAK_VERSION_MINOR 0
#define BURLAK_VERSION_PATCH 1

#define BURLAK_STR2(x) #x
#define BURLAK_STR(x) BURLAK_STR2(x)

#define BURLAK_VERSION_STRING \
    BURLAK_STR(BURLAK_VERSION_MAJOR) "." \
    BURLAK_STR(BURLAK_VERSION_MINOR) "." \
    BURLAK_STR(BURLAK_VERSION_PATCH)
