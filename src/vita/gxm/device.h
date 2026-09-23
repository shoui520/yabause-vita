/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "tile_batch.h"
#include "composite.h"

typedef struct VitaGxmDevice VitaGxmDevice;
/* Single owner, no concurrent calls. On failure *out may retain allocations if
 * cleanup failed; call Destroy again rather than losing that owner. */
int VitaGxmDeviceCreate(unsigned width, unsigned height, VitaGxmDevice **out);
/* Finishes submitted work before releasing resources; retains ownership on error. */
int VitaGxmDeviceDestroy(VitaGxmDevice **device);
int VitaGxmProbe(void);
/* Copies CPU inputs into owned storage, then submits one ordered batch. Reuse
 * waits for the preceding batch; no per-tile waits. Atlas width must be a
 * multiple of 8. No explicit clear is issued: callers must draw a background
 * covering every pixel they will observe. */
int VitaGxmSubmitTiles(VitaGxmDevice *device, const uint32_t *rgba,
   unsigned atlas_width, unsigned atlas_height, const VitaTileBatch *batch);
/* Explicit diagnostic/hybrid readback; blocks until completion. Valid until
 * the next submission or destruction. Stride is in uint32_t pixels. */
const uint32_t *VitaGxmReadback(VitaGxmDevice *device, unsigned *stride);
/* Composite at native resolution, then scale on GPU into the supplied display
 * buffer. Display storage must remain alive until UnbindDisplay/Destroy. The
 * caller owns scanout rotation; destination must not be the scanned-out buffer.
 * Finish is once per completed frame, never once per tile or layer. */
int VitaGxmBindDisplay(VitaGxmDevice *device, void *base, unsigned bytes);
int VitaGxmUnbindDisplay(VitaGxmDevice *device);
int VitaGxmComposite(VitaGxmDevice *device, const VitaGxmCompositeFrame *frame,
                     uint32_t *destination);
