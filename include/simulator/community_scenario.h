/* SPDX-License-Identifier: MIT */
#ifndef SIM_COMMUNITY_SCENARIO_H
#define SIM_COMMUNITY_SCENARIO_H
#include <errno.h>
#include <stdlib.h>

/* Private QA input addresses only emulated buttons inside this process.
 * Lines are decimal frame, hexadecimal pressed-button mask (A=1, Start=8).
 * No script expressions, paths, OS keys, mouse operations or commands. */
typedef struct { unsigned frame, mask; } SIM_CommunityScenarioRecord;
static inline int SIM_CommunityScenario_Parse(const char *line, SIM_CommunityScenarioRecord *out)
{
    char *end;
    if (!line || !out || *line < '0' || *line > '9') return 0;
    errno = 0;
    unsigned long frame = strtoul(line, &end, 10);
    if (errno || frame > 36000 || (*end != ' ' && *end != '\t')) return 0;
    while (*end == ' ' || *end == '\t') ++end;
    const char *maskStart = end;
    if (!((*end >= '0' && *end <= '9') || (*end >= 'a' && *end <= 'f') || (*end >= 'A' && *end <= 'F'))) return 0;
    errno = 0;
    unsigned long mask = strtoul(maskStart, &end, 16);
    if (errno || end == maskStart || mask > 0xfff) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end) return 0;
    out->frame = (unsigned)frame; out->mask = (unsigned)mask;
    return 1;
}
#endif
