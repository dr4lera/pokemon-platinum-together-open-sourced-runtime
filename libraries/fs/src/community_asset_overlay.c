/* SPDX-License-Identifier: MIT
 * Original community-client resource resolver. No runtime assets are embedded. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "../include/community_asset_overlay.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char LowerASCII(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

int CommunityAsset_Normalize(const char *path, char *output, size_t capacity)
{
    const char *start;
    const char *part;
    size_t size;

    if (output == NULL || capacity == 0) return 0;
    output[0] = '\0';
    if (path == NULL) return 0;
    start = path;
    if (strncmp(start, "rom:", 4) == 0) {
        start += 4;
        if (*start == '/') start++;
    }
    if (*start == '\0' || *start == '/') return 0;
    if (strlen(start) >= 6 && LowerASCII(start[0]) == 'p' && LowerASCII(start[1]) == 'a'
        && LowerASCII(start[2]) == 't' && LowerASCII(start[3]) == 'c'
        && LowerASCII(start[4]) == 'h' && start[5] == '/') return 0;
    part = start;
    for (size = 0; start[size] != '\0'; size++) {
        char c = start[size];
        if (size >= COMMUNITY_ASSET_PATH_LIMIT - 1 || size >= capacity - 1) return 0;
        if (c == '/') {
            size_t length = (size_t)(&start[size] - part);
            if (length == 0 || (length == 1 && part[0] == '.')
                || (length == 2 && part[0] == '.' && part[1] == '.')) return 0;
            part = &start[size + 1];
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return 0;
        }
    }
    if (*part == '\0' || strcmp(part, ".") == 0 || strcmp(part, "..") == 0) return 0;
    /* Trailing dots alias other names on Windows; reject them on every host. */
    for (part = start; *part != '\0'; part++) {
        if (*part == '.' && (part[1] == '/' || part[1] == '\0')) return 0;
    }
    memcpy(output, start, size + 1);
    return 1;
}

int CommunityAsset_IsOverlayResource(const char *path)
{
    static const char *const allowed[] = {
        "poketool/personal/pl_personal.narc",
        "poketool/personal/wotbl.narc",
        "msgdata/pl_msg.narc",
        "battle/graphic/pl_batt_obj.narc"
    };
    char normalized[COMMUNITY_ASSET_PATH_LIMIT];
    size_t i;
    if (!CommunityAsset_Normalize(path, normalized, sizeof(normalized))) return 0;
    for (i = 0; normalized[i]; i++) normalized[i] = LowerASCII(normalized[i]);
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(normalized, allowed[i]) == 0) return 1;
    }
    return 0;
}

#ifdef _WIN32
/* Check the final opened handle, including junction/symlink resolution, before
 * exposing it to a reader. Comparing lexical paths alone would not confine it. */
static FILE *OpenWithinRoot(const char *root, const char *relative)
{
    char rootFinal[4096], fileFinal[4096], candidate[4096];
    HANDLE rootHandle;
    DWORD rootLength, fileLength;
    BY_HANDLE_FILE_INFORMATION information;
    FILE *file;
    size_t i;
    int length;
    if (root == NULL || !((root[0] && root[1] == ':' && (root[2] == '/' || root[2] == '\\'))
                         || (root[0] == '\\' && root[1] == '\\'))) return NULL;
    rootHandle = CreateFileA(root, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (rootHandle == INVALID_HANDLE_VALUE) return NULL;
    rootLength = GetFinalPathNameByHandleA(rootHandle, rootFinal, sizeof(rootFinal), FILE_NAME_NORMALIZED);
    CloseHandle(rootHandle);
    if (rootLength == 0 || rootLength >= sizeof(rootFinal)) return NULL;
    while (rootLength && (rootFinal[rootLength - 1] == '\\' || rootFinal[rootLength - 1] == '/')) rootLength--;
    rootFinal[rootLength] = '\0';
    length = snprintf(candidate, sizeof(candidate), "%s/%s", root, relative);
    if (length < 0 || (size_t)length >= sizeof(candidate)) return NULL;
    file = fopen(candidate, "rb");
    if (file == NULL) return NULL;
    if (!GetFileInformationByHandle((HANDLE)_get_osfhandle(_fileno(file)), &information)
        || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        fclose(file);
        return NULL;
    }
    fileLength = GetFinalPathNameByHandleA((HANDLE)_get_osfhandle(_fileno(file)),
        fileFinal, sizeof(fileFinal), FILE_NAME_NORMALIZED);
    if (fileLength > rootLength && fileLength < sizeof(fileFinal)) {
        for (i = 0; i < rootLength; i++) {
            if (LowerASCII(rootFinal[i]) != LowerASCII(fileFinal[i])) break;
        }
        if (i == rootLength && (fileFinal[i] == '\\' || fileFinal[i] == '/')) return file;
    }
    fclose(file);
    return NULL;
}
#else
/* Walk using directory handles and refuse symlinks at every relative component.
 * The user may explicitly choose a symlink as root; its opened directory is the
 * boundary. Later renames cannot redirect the relative traversal outside it. */
static FILE *OpenWithinRoot(const char *root, const char *relative)
{
    char path[COMMUNITY_ASSET_PATH_LIMIT + 6];
    char *part, *slash;
    int directory, next;
    struct stat information;
    FILE *file;
    if (root == NULL || root[0] != '/') return NULL;
    if (strlen(relative) >= sizeof(path)) return NULL;
    strcpy(path, relative);
    directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return NULL;
    part = path;
    while ((slash = strchr(part, '/')) != NULL) {
        *slash = '\0';
        next = openat(directory, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(directory);
        if (next < 0) return NULL;
        directory = next;
        part = slash + 1;
    }
    next = openat(directory, part, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(directory);
    if (next < 0) return NULL;
    if (fstat(next, &information) != 0 || !S_ISREG(information.st_mode)) {
        close(next);
        return NULL;
    }
    file = fdopen(next, "rb");
    if (file == NULL) close(next);
    return file;
}
#endif

FILE *CommunityAsset_Open(const char *path, const char *runtimeRoot)
{
    char original[COMMUNITY_ASSET_PATH_LIMIT];
    char lower[COMMUNITY_ASSET_PATH_LIMIT];
    char patched[COMMUNITY_ASSET_PATH_LIMIT + 6];
    FILE *file = NULL;
    size_t i;
    if (!CommunityAsset_Normalize(path, original, sizeof(original))) return NULL;
    for (i = 0; original[i]; i++) lower[i] = LowerASCII(original[i]);
    lower[i] = '\0';
    if (runtimeRoot != NULL && *runtimeRoot != '\0') {
        if (CommunityAsset_IsOverlayResource(original)) {
            snprintf(patched, sizeof(patched), "patch/%s", original);
            file = OpenWithinRoot(runtimeRoot, patched);
            if (file == NULL && strcmp(original, lower) != 0) {
                snprintf(patched, sizeof(patched), "patch/%s", lower);
                file = OpenWithinRoot(runtimeRoot, patched);
            }
        }
        if (file == NULL) file = OpenWithinRoot(runtimeRoot, original);
        if (file == NULL && strcmp(original, lower) != 0) file = OpenWithinRoot(runtimeRoot, lower);
    }
    if (file == NULL) file = fopen(original, "rb");
    if (file == NULL && strcmp(original, lower) != 0) file = fopen(lower, "rb");
    return file;
}
