#pragma once
#include "common.h"

#define RESP_MAX_ARGS 64
#define RESP_MAX_BULK (70 * 1024)

typedef struct {
    const uint8_t *p;
    uint32_t n;
} arg_t;

/* Parse ONE command from buf[0..len).
 * >0 : bytes consumed, *argc/argv filled (argv point INTO buf: zero copy)
 *  0 : incomplete, need more bytes  (*need = minimum total bytes if known, else 0)
 * <0 : protocol error */
int resp_parse(const uint8_t *buf, size_t len, arg_t *argv, int *argc, size_t *need);
