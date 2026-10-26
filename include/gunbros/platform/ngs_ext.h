#ifndef GUNBROS_NGS_EXT_H
#define GUNBROS_NGS_EXT_H

#include "gunbros/common/common.h"

GUNBROS_EXTERN_C_BEGIN

/*
 * Requests to the replacement NGS server's extension endpoints (/gunbros/...), made by the
 * bridge itself rather than the game. The server is the one set with debug.gunbros.ngs_host
 * and debug.gunbros.ngs_port. Requests run on a background thread, one at a time: start one
 * with ngs_ext_start(), then poll ngs_ext_poll() each frame until it returns true.
 */
#define NGS_EXT_REPLY_MAX 512

/* Start PUT <path> with a JSON body. Returns false if a request is already running. */
bool ngs_ext_start(const char *path, const char *json_body);

/* True once the request has finished; copies the reply body (or "" on failure) to `reply`. */
bool ngs_ext_poll(char *reply, size_t reply_size);

/* The string value of `"key": "..."` in a flat JSON object, or false if absent. */
bool ngs_ext_json_string(const char *json, const char *key, char *out, size_t out_size);

GUNBROS_EXTERN_C_END

#endif // GUNBROS_NGS_EXT_H
