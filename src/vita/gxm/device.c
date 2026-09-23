/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "device.h"
#include "tile_batch.h"
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <stdlib.h>
#include <string.h>
#include "tile_v.h"
#include "tile_f.h"
#include "compose_f.h"
#include "present_f.h"

extern void YuiMsg(const char *, ...);
typedef struct {
   SceUID id;
   void *base;
   unsigned size, usse_offset;
   int mapped, usse_kind; /* 0=data, 1=fragment, 2=vertex */
} GpuMemory;

struct VitaGxmDevice {
   int initialized;
   SceGxmContext *context;
   SceGxmRenderTarget *target;
   SceGxmRenderTarget *display_target;
   void *display_base;
   unsigned display_bytes;
   void *host;
   GpuMemory memory[13]; /* rings, surfaces, patcher, atlas/geometry, layers/lines */
   unsigned width, height, stride;
   SceGxmShaderPatcher *patcher;
   SceGxmShaderPatcherId vertex_id, fragment_id;
   SceGxmVertexProgram *vertex_program;
   SceGxmFragmentProgram *fragment_program;
   SceGxmShaderPatcherId composite_id;
   SceGxmFragmentProgram *composite_program;
   SceGxmShaderPatcherId present_id;
   SceGxmFragmentProgram *present_program;
   SceGxmColorSurface color;
   SceGxmDepthStencilSurface depth;
};

static int allocate(GpuMemory *m, unsigned bytes, int usse_kind) {
   if (!bytes || bytes > 0x7ffff000u) return -1;
   m->size = (bytes + 4095u) & ~4095u;
   m->usse_kind = usse_kind;
   m->id = sceKernelAllocMemBlock("yabause-gxm", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                 m->size, NULL);
   if (m->id < 0) return m->id;
   int rc = sceKernelGetMemBlockBase(m->id, &m->base);
   if (rc < 0) return rc;
   memset(m->base, 0, m->size);
   if (usse_kind == 1) rc = sceGxmMapFragmentUsseMemory(m->base, m->size, &m->usse_offset);
   else if (usse_kind == 2) rc = sceGxmMapVertexUsseMemory(m->base, m->size, &m->usse_offset);
   else rc = sceGxmMapMemory(m->base, m->size, SCE_GXM_MEMORY_ATTRIB_RW);
   if (rc >= 0) m->mapped = 1;
   return rc;
}

static int release(GpuMemory *m) {
   if (m->id < 0) return 0;
   if (m->mapped) {
      int rc = m->usse_kind == 1 ? sceGxmUnmapFragmentUsseMemory(m->base) :
               m->usse_kind == 2 ? sceGxmUnmapVertexUsseMemory(m->base) : sceGxmUnmapMemory(m->base);
      if (rc < 0) return rc; /* Never free a still-mapped block. */
      m->mapped = 0;
   }
   int rc = sceKernelFreeMemBlock(m->id);
   if (rc >= 0) { m->id = -1; m->base = NULL; }
   return rc;
}

int VitaGxmDeviceDestroy(VitaGxmDevice **owner) {
   if (!owner || !*owner) return 0;
   VitaGxmDevice *d = *owner;
   int rc;
   if (d->context) sceGxmFinish(d->context);
   rc = VitaGxmUnbindDisplay(d);
   if (rc < 0) return rc;
   if (d->display_target) {
      rc = sceGxmDestroyRenderTarget(d->display_target);
      if (rc < 0) return rc;
      d->display_target = NULL;
   }
   if (d->target) {
      rc = sceGxmDestroyRenderTarget(d->target);
      if (rc < 0) return rc;
      d->target = NULL;
   }
   if (d->context) {
      rc = sceGxmDestroyContext(d->context);
      if (rc < 0) return rc;
      d->context = NULL;
   }
   if (d->fragment_program) {
      rc = sceGxmShaderPatcherReleaseFragmentProgram(d->patcher, d->fragment_program);
      if (rc < 0) return rc;
      d->fragment_program = NULL;
   }
   if (d->composite_program) {
      rc = sceGxmShaderPatcherReleaseFragmentProgram(d->patcher, d->composite_program);
      if (rc < 0) return rc;
      d->composite_program = NULL;
   }
   if (d->present_program) {
      rc = sceGxmShaderPatcherReleaseFragmentProgram(d->patcher, d->present_program);
      if (rc < 0) return rc;
      d->present_program = NULL;
   }
   if (d->present_id) {
      rc = sceGxmShaderPatcherUnregisterProgram(d->patcher, d->present_id);
      if (rc < 0) return rc;
      d->present_id = NULL;
   }
   if (d->composite_id) {
      rc = sceGxmShaderPatcherUnregisterProgram(d->patcher, d->composite_id);
      if (rc < 0) return rc;
      d->composite_id = NULL;
   }
   if (d->vertex_program) {
      rc = sceGxmShaderPatcherReleaseVertexProgram(d->patcher, d->vertex_program);
      if (rc < 0) return rc;
      d->vertex_program = NULL;
   }
   if (d->fragment_id) {
      rc = sceGxmShaderPatcherUnregisterProgram(d->patcher, d->fragment_id);
      if (rc < 0) return rc;
      d->fragment_id = NULL;
   }
   if (d->vertex_id) {
      rc = sceGxmShaderPatcherUnregisterProgram(d->patcher, d->vertex_id);
      if (rc < 0) return rc;
      d->vertex_id = NULL;
   }
   if (d->patcher) {
      rc = sceGxmShaderPatcherDestroy(d->patcher);
      if (rc < 0) return rc;
      d->patcher = NULL;
   }
   for (int i = 12; i >= 0; --i) {
      rc = release(&d->memory[i]);
      if (rc < 0) return rc;
   }
   free(d->host); d->host = NULL;
   if (d->initialized) {
      rc = sceGxmTerminate();
      if (rc < 0) return rc;
      d->initialized = 0;
   }
   free(d); *owner = NULL;
   return 0;
}

static void *patcher_alloc(void *user, SceSize size) { (void)user; return malloc(size); }
static void patcher_free(void *user, void *memory) { (void)user; free(memory); }

static int create_programs(VitaGxmDevice *d) {
   SceGxmShaderPatcherParams p = {0};
   p.hostAllocCallback = patcher_alloc; p.hostFreeCallback = patcher_free;
   p.bufferMem = d->memory[6].base; p.bufferMemSize = d->memory[6].size;
   p.vertexUsseMem = d->memory[7].base; p.vertexUsseMemSize = d->memory[7].size;
   p.vertexUsseOffset = d->memory[7].usse_offset;
   p.fragmentUsseMem = d->memory[8].base; p.fragmentUsseMemSize = d->memory[8].size;
   p.fragmentUsseOffset = d->memory[8].usse_offset;
   int rc = sceGxmShaderPatcherCreate(&p, &d->patcher);
   if (rc < 0) return rc;
   const SceGxmProgram *vp = (const SceGxmProgram *)yabause_tile_v;
   const SceGxmProgram *fp = (const SceGxmProgram *)yabause_tile_f;
   rc = sceGxmShaderPatcherRegisterProgram(d->patcher, vp, &d->vertex_id);
   if (rc < 0) return rc;
   rc = sceGxmShaderPatcherRegisterProgram(d->patcher, fp, &d->fragment_id);
   if (rc < 0) return rc;
   const SceGxmProgramParameter *position = sceGxmProgramFindParameterByName(vp, "position");
   const SceGxmProgramParameter *uv = sceGxmProgramFindParameterByName(vp, "uv");
   if (!position || !uv) return -1;
   SceGxmVertexAttribute attributes[2] = {0};
   attributes[0].format = attributes[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
   attributes[0].componentCount = attributes[1].componentCount = 2;
   attributes[0].regIndex = sceGxmProgramParameterGetResourceIndex(position);
   attributes[1].regIndex = sceGxmProgramParameterGetResourceIndex(uv);
   attributes[1].offset = 2 * sizeof(float);
   SceGxmVertexStream stream = {0};
   stream.stride = 4 * sizeof(float); stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
   rc = sceGxmShaderPatcherCreateVertexProgram(d->patcher, d->vertex_id,
      attributes, 2, &stream, 1, &d->vertex_program);
   if (rc < 0) return rc;
   rc = sceGxmShaderPatcherCreateFragmentProgram(d->patcher, d->fragment_id,
      SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE, NULL, vp, &d->fragment_program);
   if (rc < 0) return rc;
   rc = sceGxmShaderPatcherRegisterProgram(d->patcher,
      (const SceGxmProgram *)yabause_compose_f, &d->composite_id);
   if (rc < 0) return rc;
   rc = sceGxmShaderPatcherCreateFragmentProgram(d->patcher, d->composite_id,
      SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE, NULL, vp, &d->composite_program);
   if (rc < 0) return rc;
   rc = sceGxmShaderPatcherRegisterProgram(d->patcher,
      (const SceGxmProgram *)yabause_present_f, &d->present_id);
   if (rc < 0) return rc;
   return sceGxmShaderPatcherCreateFragmentProgram(d->patcher, d->present_id,
      SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE, NULL, vp, &d->present_program);
}

int VitaGxmDeviceCreate(unsigned width, unsigned height, VitaGxmDevice **out) {
   if (!out || *out || !width || !height || width > 704 || height > 512) return -1;
   VitaGxmDevice *d = calloc(1, sizeof(*d));
   if (!d) return -1;
   for (unsigned i = 0; i < 13; ++i) d->memory[i].id = -1;
   d->width = width; d->height = height; d->stride = (width + 63u) & ~63u;
   *out = d;
   SceGxmInitializeParams init = {0};
   init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
   int rc = sceGxmInitialize(&init);
   if (rc < 0) goto fail;
   d->initialized = 1;
   const unsigned sizes[] = { SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE,
      SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE,
      SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE,
      ((width + 63u) & ~63u) * height * 4u,
      ((width + 31u) & ~31u) * ((height + 31u) & ~31u) * 4u,
      64 * 1024, 64 * 1024, 64 * 1024 };
   for (unsigned i = 0; i < 9; ++i) {
      rc = allocate(&d->memory[i], sizes[i], i == 7 ? 2 : (i == 3 || i == 8));
      if (rc < 0) goto fail;
   }
   d->host = calloc(1, SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
   if (!d->host) { rc = -1; goto fail; }
   SceGxmContextParams context = {0};
   context.hostMem = d->host;
   context.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
   context.vdmRingBufferMem = d->memory[0].base;
   context.vdmRingBufferMemSize = d->memory[0].size;
   context.vertexRingBufferMem = d->memory[1].base;
   context.vertexRingBufferMemSize = d->memory[1].size;
   context.fragmentRingBufferMem = d->memory[2].base;
   context.fragmentRingBufferMemSize = d->memory[2].size;
   context.fragmentUsseRingBufferMem = d->memory[3].base;
   context.fragmentUsseRingBufferMemSize = d->memory[3].size;
   context.fragmentUsseRingBufferOffset = d->memory[3].usse_offset;
   rc = sceGxmCreateContext(&context, &d->context);
   if (rc < 0) goto fail;
   SceGxmRenderTargetParams target = {0};
   target.width = width; target.height = height; target.scenesPerFrame = 1;
   target.driverMemBlock = -1;
   rc = sceGxmCreateRenderTarget(&target, &d->target);
   if (rc < 0) goto fail;
   rc = sceGxmColorSurfaceInit(&d->color, SCE_GXM_COLOR_FORMAT_A8B8G8R8,
      SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
      SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, width, height, (width + 63u) & ~63u,
      d->memory[4].base);
   if (rc < 0) goto fail;
   rc = sceGxmDepthStencilSurfaceInit(&d->depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
      SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, (width + 31u) & ~31u, d->memory[5].base, NULL);
   if (rc < 0) goto fail;
   rc = create_programs(d);
   if (rc < 0) goto fail;
   return 0;
fail:
   { int cleanup = VitaGxmDeviceDestroy(out);
     if (cleanup < 0) YuiMsg("gxm_cleanup_failed=%08x original=%08x", cleanup, rc); }
   return rc;
}

static int reserve_upload(GpuMemory *m, unsigned size) {
   if (m->mapped && m->size >= size) return 0;
   int rc = release(m);
   return rc < 0 ? rc : allocate(m, size, 0);
}

const uint32_t *VitaGxmReadback(VitaGxmDevice *d, unsigned *stride) {
   if (!d || !d->context || !stride) return NULL;
   sceGxmFinish(d->context);
   *stride = d->stride;
   return d->memory[4].base;
}

int VitaGxmSubmitTiles(VitaGxmDevice *d, const uint32_t *rgba,
   unsigned aw, unsigned ah, const VitaTileBatch *batch) {
   if (!d || !d->context || !rgba || !batch || !batch->vertices || !batch->indices ||
       !batch->count || batch->count > batch->capacity || batch->capacity > 16384 ||
       !aw || !ah || aw > 1024 || ah > 1024 || (aw & 7)) return -1;
   for (unsigned i = 0; i < batch->count * 6; ++i)
      if (batch->indices[i] >= batch->count * 4) return -1;
   sceGxmFinish(d->context); /* Retire the owned upload slot before reuse. */
   unsigned vb = batch->count * 4 * sizeof(VitaTileVertex);
   unsigned ib = batch->count * 6 * sizeof(uint16_t);
   int rc = reserve_upload(&d->memory[9], aw * ah * 4);
   if (rc < 0) return rc;
   rc = reserve_upload(&d->memory[10], vb + ib);
   if (rc < 0) return rc;
   memcpy(d->memory[9].base, rgba, aw * ah * 4);
   memcpy(d->memory[10].base, batch->vertices, vb);
   void *indices = (char *)d->memory[10].base + vb;
   memcpy(indices, batch->indices, ib);
   SceGxmTexture atlas;
   rc = sceGxmTextureInitLinear(&atlas, d->memory[9].base, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, aw, ah, 0);
   if (rc < 0) return rc;
   if ((rc = sceGxmTextureSetMinFilter(&atlas, SCE_GXM_TEXTURE_FILTER_POINT)) < 0 ||
       (rc = sceGxmTextureSetMagFilter(&atlas, SCE_GXM_TEXTURE_FILTER_POINT)) < 0 ||
       (rc = sceGxmTextureSetUAddrMode(&atlas, SCE_GXM_TEXTURE_ADDR_CLAMP)) < 0 ||
       (rc = sceGxmTextureSetVAddrMode(&atlas, SCE_GXM_TEXTURE_ADDR_CLAMP)) < 0) return rc;
   const SceGxmProgramParameter *viewport = sceGxmProgramFindParameterByName(
      (const SceGxmProgram *)yabause_tile_v, "viewport_size");
   if (!viewport) return -1;
   rc = sceGxmBeginScene(d->context, 0, d->target, NULL, NULL, NULL, &d->color, &d->depth);
   if (rc < 0) return rc;
   sceGxmSetViewport(d->context, d->width * .5f, d->width * .5f,
                    d->height * .5f, d->height * -.5f, .5f, .5f);
   sceGxmSetCullMode(d->context, SCE_GXM_CULL_NONE);
   sceGxmSetFrontDepthFunc(d->context, SCE_GXM_DEPTH_FUNC_ALWAYS);
   sceGxmSetFrontDepthWriteEnable(d->context, SCE_GXM_DEPTH_WRITE_DISABLED);
   sceGxmSetVertexProgram(d->context, d->vertex_program);
   sceGxmSetFragmentProgram(d->context, d->fragment_program);
   rc = sceGxmSetVertexStream(d->context, 0, d->memory[10].base);
   if (rc >= 0) rc = sceGxmSetFragmentTexture(d->context, 0, &atlas);
   void *uniform = NULL;
   if (rc >= 0) rc = sceGxmReserveVertexDefaultUniformBuffer(d->context, &uniform);
   const float size[2] = {d->width, d->height};
   if (rc >= 0) rc = sceGxmSetUniformDataF(uniform, viewport, 0, 2, size);
   if (rc >= 0) rc = sceGxmDraw(d->context, SCE_GXM_PRIMITIVE_TRIANGLES,
                               SCE_GXM_INDEX_FORMAT_U16, indices, batch->count * 6);
   int end = sceGxmEndScene(d->context, NULL, NULL);
   if (end < 0) {
      YuiMsg("gxm_scene_end_failed=%08x", end);
      __builtin_trap();
   }
   return rc;
}

int VitaGxmUnbindDisplay(VitaGxmDevice *d) {
   if (!d || !d->display_base) return 0;
   if (d->context) sceGxmFinish(d->context);
   int rc = sceGxmUnmapMemory(d->display_base);
   if (rc >= 0) { d->display_base = NULL; d->display_bytes = 0; }
   return rc;
}

int VitaGxmBindDisplay(VitaGxmDevice *d, void *base, unsigned bytes) {
   if (!d || !d->context || !base || ((uintptr_t)base & 4095) ||
       (bytes & 4095) || bytes < 512 * 272 * 4) return -1;
   if (d->display_base == base && d->display_bytes == bytes && d->display_target) return 0;
   int rc = VitaGxmUnbindDisplay(d);
   if (rc < 0) return rc;
   rc = sceGxmMapMemory(base, bytes, SCE_GXM_MEMORY_ATTRIB_RW);
   if (rc < 0) return rc;
   d->display_base = base; d->display_bytes = bytes;
   if (!d->display_target) {
      SceGxmRenderTargetParams p = {0};
      p.width = 480; p.height = 272; p.scenesPerFrame = 1; p.driverMemBlock = -1;
      rc = sceGxmCreateRenderTarget(&p, &d->display_target);
   }
   return rc;
}

static int point_texture(SceGxmTexture *texture, void *data, unsigned w, unsigned h) {
   int rc = sceGxmTextureInitLinear(texture, data, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, w, h, 0);
   if (rc >= 0) rc = sceGxmTextureSetMinFilter(texture, SCE_GXM_TEXTURE_FILTER_POINT);
   if (rc >= 0) rc = sceGxmTextureSetMagFilter(texture, SCE_GXM_TEXTURE_FILTER_POINT);
   if (rc >= 0) rc = sceGxmTextureSetUAddrMode(texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
   if (rc >= 0) rc = sceGxmTextureSetVAddrMode(texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
   return rc;
}

static int uniform2(VitaGxmDevice *d, int fragment, const SceGxmProgram *program,
                    void *buffer, const char *name, float x, float y) {
   (void)d; (void)fragment;
   const SceGxmProgramParameter *p = sceGxmProgramFindParameterByName(program, name);
   float v[2] = {x, y};
   return p ? sceGxmSetUniformDataF(buffer, p, 0, 2, v) : -1;
}

static int end_scene(VitaGxmDevice *d, int status) {
   int rc = sceGxmEndScene(d->context, NULL, NULL);
   if (rc < 0) {
      YuiMsg("gxm_scene_end_failed=%08x", rc);
      __builtin_trap(); /* Scene lifetime is unknown; never free live storage. */
   }
   return status;
}

static int fullscreen(VitaGxmDevice *d, SceGxmRenderTarget *target,
   SceGxmColorSurface *color, unsigned w, unsigned h,
   SceGxmFragmentProgram *fragment, unsigned vertex_offset,
   SceGxmTexture *textures, unsigned count, const VitaGxmCompositeFrame *frame) {
   int rc = sceGxmBeginScene(d->context, 0, target, NULL, NULL, NULL, color, NULL);
   if (rc < 0) return rc;
   sceGxmSetViewport(d->context, w * .5f, w * .5f, h * .5f, h * -.5f, .5f, .5f);
   sceGxmSetCullMode(d->context, SCE_GXM_CULL_NONE);
   sceGxmSetFrontDepthFunc(d->context, SCE_GXM_DEPTH_FUNC_ALWAYS);
   sceGxmSetFrontDepthWriteEnable(d->context, SCE_GXM_DEPTH_WRITE_DISABLED);
   sceGxmSetVertexProgram(d->context, d->vertex_program);
   sceGxmSetFragmentProgram(d->context, fragment);
   rc = sceGxmSetVertexStream(d->context, 0, (char *)d->memory[10].base + vertex_offset);
   for (unsigned i = 0; rc >= 0 && i < count; ++i)
      rc = sceGxmSetFragmentTexture(d->context, i, textures + i);
   void *uniform = NULL;
   if (rc >= 0) rc = sceGxmReserveVertexDefaultUniformBuffer(d->context, &uniform);
   if (rc >= 0) rc = uniform2(d, 0, (const SceGxmProgram *)yabause_tile_v,
                               uniform, "viewport_size", w, h);
   if (rc >= 0 && frame) {
      rc = sceGxmReserveFragmentDefaultUniformBuffer(d->context, &uniform);
      const SceGxmProgram *p = (const SceGxmProgram *)yabause_compose_f;
      if (rc >= 0) rc = uniform2(d, 1, p, uniform, "source_size", frame->width, frame->height);
      if (rc >= 0) rc = uniform2(d, 1, p, uniform, "field_state", frame->line_increment, frame->field);
      if (rc >= 0) rc = uniform2(d, 1, p, uniform, "controls", frame->blend_mode, frame->sprite_window);
   }
   if (rc >= 0) rc = sceGxmDraw(d->context, SCE_GXM_PRIMITIVE_TRIANGLES,
      SCE_GXM_INDEX_FORMAT_U16, (char *)d->memory[10].base + 8 * sizeof(VitaTileVertex), 6);
   return end_scene(d, rc);
}

int VitaGxmComposite(VitaGxmDevice *d, const VitaGxmCompositeFrame *f, uint32_t *out) {
   if (!d || !f || !d->display_base || !d->display_target || !out ||
       f->width != d->width || f->height != d->height || (f->width & 7) ||
       (f->line_increment != 1 && f->line_increment != 2) ||
       f->field >= f->line_increment || f->height % f->line_increment ||
       f->height / f->line_increment > 256 || f->blend_mode > 2 || !f->back) return -1;
   uintptr_t dest = (uintptr_t)out, base = (uintptr_t)d->display_base;
   if ((dest & 255) || dest < base || dest - base > d->display_bytes - 512 * 272 * 4) return -1;
   for (unsigned i = 0; i < 6; ++i) if (!f->layers[i]) return -1;
   for (unsigned i = 0; i < 3; ++i) if (!f->line[i]) return -1;
   // A frame owns the slot until the final Finish. No intermediate readback.
   sceGxmFinish(d->context);
   unsigned rows = f->height / f->line_increment;
   unsigned layer_bytes = f->width * rows * sizeof(VitaGxmPixel);
   // Each layer's texture base is separately 64-byte aligned (native widths
   // are multiples of eight and each pixel occupies eight bytes).
   int rc = reserve_upload(&d->memory[11], layer_bytes * 6);
   if (rc >= 0) rc = reserve_upload(&d->memory[12], f->height * 8 * sizeof(uint32_t));
   if (rc >= 0) rc = reserve_upload(&d->memory[10], 8 * sizeof(VitaTileVertex) + 6 * sizeof(uint16_t));
   if (rc < 0) return rc;
   SceGxmTexture textures[7];
   for (unsigned i = 0; i < 6; ++i) {
      void *p = (char *)d->memory[11].base + i * layer_bytes;
      memcpy(p, f->layers[i], layer_bytes);
      rc = point_texture(textures + i, p, f->width * 2, rows);
      if (rc < 0) return rc;
   }
   uint32_t *lines = d->memory[12].base;
   for (unsigned y = 0; y < f->height; ++y) {
      memcpy(lines + y * 8, f->back + y * f->width, sizeof(VitaGxmPixel));
      for (unsigned i = 0; i < 3; ++i) lines[y * 8 + 2 + i] = f->line[i][y];
   }
   rc = point_texture(textures + 6, lines, 8, f->height);
   if (rc < 0) return rc;
   const VitaTileVertex quad[8] = {
      {0,0,0,0}, {f->width,0,1,0}, {0,f->height,0,1}, {f->width,f->height,1,1},
      {0,0,0,0}, {480,0,(float)d->width/d->stride,0},
      {0,272,0,1}, {480,272,(float)d->width/d->stride,1}
   };
   const uint16_t index[6] = {0,1,2,2,1,3};
   memcpy(d->memory[10].base, quad, sizeof(quad));
   memcpy((char *)d->memory[10].base + sizeof(quad), index, sizeof(index));
   rc = fullscreen(d, d->target, &d->color, d->width, d->height,
      d->composite_program, 0, textures, 7, f);
   if (rc < 0) { sceGxmFinish(d->context); return rc; }
   SceGxmColorSurface display;
   rc = sceGxmColorSurfaceInit(&display, SCE_GXM_COLOR_FORMAT_A8B8G8R8,
      SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
      SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, 480, 272, 512, out);
   SceGxmTexture source;
   if (rc >= 0) rc = point_texture(&source, d->memory[4].base, d->stride, d->height);
   if (rc >= 0) rc = fullscreen(d, d->display_target, &display, 480, 272,
      d->present_program, 4 * sizeof(VitaTileVertex), &source, 1, NULL);
   sceGxmFinish(d->context);
   return rc;
}

static int probe_tile(VitaGxmDevice *d, int layered) {
   int rc;
   uint32_t texels[128];
   VitaTileVertex vertices[64];
   uint16_t indices[96];
   for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x)
         texels[y * 8 + x] = 0xff000000u | ((x * 31u) << 16) | ((y * 29u) << 8) | (x + y * 8);
   if (layered)
      for (unsigned y = 0; y < 8; ++y)
         for (unsigned x = 0; x < 8; ++x)
            texels[64 + y * 8 + x] = (((x + y) & 1) ? 0xff000000u : 0) |
                                      0x00553700u | (x + 8 * y);
   const int positions[8][2] = {{8,8}, {24,8}, {8,24}, {24,24},
                               {-3,40}, {316,40}, {40,-3}, {40,220}};
   VitaTileBatch batch = {vertices, indices, 0, 16};
   unsigned atlas_height = layered ? 16 : 8;
   for (unsigned layer = 0; layer < (layered ? 2u : 1u); ++layer)
   for (unsigned i = 0; i < 8; ++i) {
      if (VitaTileBatchAdd(&batch, positions[i][0], positions[i][1],
                          0, layer * 8, 8, atlas_height, 320, 224, (i + layer) & 3) != 1) {
         rc = -1; goto cleanup;
      }
   }
   rc = VitaGxmSubmitTiles(d, texels, 8, atlas_height, &batch);
   if (rc >= 0) {
      unsigned stride;
      const unsigned *pixels = VitaGxmReadback(d, &stride);
      unsigned checked = 0;
      for (unsigned tile = 0; tile < 8; ++tile)
      for (unsigned y = 0; y < 8; ++y)
         for (unsigned x = 0; x < 8; ++x) {
            int dx = positions[tile][0] + (int)x, dy = positions[tile][1] + (int)y;
            if (dx < 0 || dy < 0 || dx >= 320 || dy >= 224) continue;
            unsigned actual = pixels[dy * stride + dx];
            unsigned tx = (tile & 1) ? 7 - x : x;
            unsigned ty = (tile & 2) ? 7 - y : y;
            unsigned expected = texels[ty * 8 + tx];
            if (layered) {
               unsigned flip = (tile + 1) & 3;
               unsigned ox = (flip & 1) ? 7 - x : x;
               unsigned oy = (flip & 2) ? 7 - y : y;
               unsigned top = texels[64 + oy * 8 + ox];
               if (top >> 24) expected = top;
            }
            if (actual != expected) {
               YuiMsg("gxm_pixel_mismatch x=%u y=%u actual=%08x expected=%08x", x, y, actual, expected);
               rc = -1; goto cleanup;
            }
            ++checked;
         }
      if (layered) YuiMsg("gxm_tile_overlay_pixels_pass count=%u tiles=%u", checked, batch.count);
      else YuiMsg("gxm_tile_batch_pixels_pass count=%u tiles=%u", checked, batch.count);
   }
cleanup:
   /* Finish is deliberately used for this diagnostic readback, not as a proposed
    * per-tile production synchronization strategy. */
   sceGxmFinish(d->context);
   return rc;
}

int VitaGxmProbe(void) {
   int rc = sceGxmProgramCheck((const SceGxmProgram *)yabause_tile_v);
   if (rc < 0) return rc;
   rc = sceGxmProgramCheck((const SceGxmProgram *)yabause_tile_f);
   if (rc < 0) return rc;
   VitaGxmDevice *device = NULL;
   rc = VitaGxmDeviceCreate(320, 224, &device);
   if (rc >= 0) rc = probe_tile(device, 0);
   if (rc >= 0) rc = probe_tile(device, 1);
   int cleanup = VitaGxmDeviceDestroy(&device);
   if (cleanup < 0) {
      YuiMsg("gxm_probe_cleanup_failed=%08x", cleanup);
      __builtin_trap(); /* Do not abandon live GPU ownership and continue. */
   }
   return rc;
}
