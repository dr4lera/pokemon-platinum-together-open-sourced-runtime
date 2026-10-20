#ifndef SIM_NET_VALIDATION_H
#define SIM_NET_VALIDATION_H

#include <stddef.h>

/* Bounds for the existing simulator protocol, before indexing or copying. */
static inline int SIM_Net_ValidPlayerId(int id)
{
    return id >= 0 && id < 16;
}

static inline int SIM_Net_ValidMPData(unsigned aid, unsigned port, int length,
                                    size_t available, size_t maximum)
{
    return aid < 16 && port < 16 && length >= 0
        && (size_t)length <= maximum && (size_t)length == available;
}

#endif
