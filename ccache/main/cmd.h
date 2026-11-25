#pragma once
#include "common.h"
#include "resp.h"

#define CMD_MIN_ROOM 64 /* caller guarantees this much free space in o before cmd_exec */

/* Execute one command, append the RESP reply to o.
 * Returns 0 on success, KV_ENOROOM if the reply needs more space than is free
 * (only for idempotent read commands: caller flushes and retries), 1 if the
 * client asked to QUIT. */
int cmd_exec(int argc, const arg_t *argv, obuf_t *o);
