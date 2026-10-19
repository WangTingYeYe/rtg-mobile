#pragma once
#include "../nvn/nvn_tex.h"
#include <stddef.h>
#include <stdint.h>

/* Optional, bounded preparation of immutable BC7 snapshots. A miss always
 * falls back to the normal decoder; callers never wait for decoding. */
typedef struct NvnBcPrepared NvnBcPrepared;
void nvn_bc_preload_offer(const nvn_texdesc *desc,const uint8_t *source,size_t bytes);
NvnBcPrepared *nvn_bc_preload_acquire(const nvn_texdesc *desc,const uint8_t *source,size_t bytes);
const uint8_t *nvn_bc_preload_pixels(const NvnBcPrepared *prepared);
void nvn_bc_preload_release(NvnBcPrepared *prepared);
/* Call after producers/consumers have stopped. Releases cache and joins worker. */
void nvn_bc_preload_shutdown(void);
