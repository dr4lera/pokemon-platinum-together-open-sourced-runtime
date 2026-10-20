/* SPDX-License-Identifier: MIT */
#ifndef SIM_COMMUNITY_CONTROLLER_H
#define SIM_COMMUNITY_CONTROLLER_H
#include <stddef.h>
#include <stdio.h>

/* One complete record: decimal id, one space, 1..3 hex mask digits, one
 * space, decimal duration, LF. This format has no executable expressions. */
typedef struct { unsigned id, mask, duration; } SIM_CommunityControllerRecord;
typedef struct { unsigned lastId, mask, remaining; } SIM_CommunityControllerState;

static inline int SIM_CommunityController_Digit(unsigned char c, unsigned base)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (base == 16 && c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (base == 16 && c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static inline int SIM_CommunityController_Number(const char *bytes, size_t length, size_t *position, unsigned base, unsigned limit, unsigned maxDigits, unsigned *out)
{
    unsigned number = 0, digits = 0;
    while (*position < length) {
        int digit = SIM_CommunityController_Digit((unsigned char)bytes[*position], base);
        if (digit < 0 || (unsigned)digit >= base) break;
        if (++digits > maxDigits || number > (limit - (unsigned)digit) / base) return 0;
        number = number * base + (unsigned)digit;
        ++*position;
    }
    if (!digits) return 0;
    *out = number;
    return 1;
}
static inline int SIM_CommunityController_Parse(const char *bytes, size_t length, SIM_CommunityControllerRecord *out)
{
    SIM_CommunityControllerRecord record;
    size_t p = 0;
    if (!bytes || !out || length < 6 || length > 64) return 0;
    if (!SIM_CommunityController_Number(bytes, length, &p, 10, 2147483647u, 10, &record.id) || !record.id || p >= length || bytes[p++] != ' ') return 0;
    if (!SIM_CommunityController_Number(bytes, length, &p, 16, 4095, 3, &record.mask) || p >= length || bytes[p++] != ' ') return 0;
    if (!SIM_CommunityController_Number(bytes, length, &p, 10, 180, 3, &record.duration) || !record.duration || p + 1 != length || bytes[p] != '\n') return 0;
    *out = record;
    return 1;
}
static inline int SIM_CommunityController_Read(FILE *file, SIM_CommunityControllerRecord *out)
{
    char bytes[65];
    size_t length;
    if (!file || !out) return 0;
    length = fread(bytes, 1, sizeof(bytes), file);
    return !ferror(file) && length <= 64 && SIM_CommunityController_Parse(bytes, length, out);
}
static inline int SIM_CommunityController_Accept(SIM_CommunityControllerState *state, const SIM_CommunityControllerRecord *record)
{
    if (!state || !record || !record->id || record->id > 2147483647u || record->id <= state->lastId
        || record->mask > 4095 || !record->duration || record->duration > 180) return 0;
    state->lastId = record->id;
    state->mask = record->mask;
    state->remaining = record->duration;
    return 1;
}
/* Call exactly once per emulated frame. Missing, malformed, or replayed files
 * never extend a hold. After duration frames the following frame is released. */
static inline unsigned SIM_CommunityController_Frame(SIM_CommunityControllerState *state)
{
    unsigned mask;
    if (!state || !state->remaining) return 0;
    mask = state->mask;
    if (--state->remaining == 0) state->mask = 0;
    return mask;
}
#endif
