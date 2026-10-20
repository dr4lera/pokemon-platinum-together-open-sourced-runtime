/* SPDX-License-Identifier: MIT */
#ifndef COMMUNITY_ASSET_OVERLAY_H
#define COMMUNITY_ASSET_OVERLAY_H

#include <stddef.h>
#include <stdio.h>

#define COMMUNITY_ASSET_PATH_LIMIT 512
#define COMMUNITY_ASSET_ROOT_ENV "PLATINUM_COMMUNITY_ASSET_ROOT"

/* Accept a relative game resource, optionally prefixed with rom:/ or rom:. */
int CommunityAsset_Normalize(const char *path, char *output, size_t capacity);
int CommunityAsset_IsOverlayResource(const char *path);

/* runtimeRoot is an explicitly chosen absolute runtime directory, or NULL.
 * Missing/rejected private files fall back to ordinary cwd resources. */
FILE *CommunityAsset_Open(const char *path, const char *runtimeRoot);

#endif
