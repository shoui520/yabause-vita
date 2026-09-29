# Runtime Cg shaders on Vita

`YABAUSE_RUNTIME_SHADERS=ON` removes the build-time dependency on `psp2cgc` for
the VitaGL renderer. The build embeds the existing Cg sources, recursively
expanded local includes, and the same variant definitions used by the offline
path. A readable `.source.cg` accompanies each generated header. No GXP file is
claimed or generated in this mode.

On the graphics owner, native programs are created with `GL_CG_VERTEX_SHADER_EXT`
and `GL_CG_FRAGMENT_SHADER_EXT`, then use `glShaderSource` and `glCompileShader`.
Standard GL shader kinds select GLSL translation in the pinned VitaGL and are
not suitable for these Cg payloads. VitaGL/VitaShaRK owns initialization and
compiler output, including reflected matrix metadata. The compiler module must
be available on the Vita as required by VitaGL; this repository does not supply
that module. Missing compiler or rejected Cg is a fatal, logged error, not a
silent software fallback.

The loader requests O3 shader optimization with fast math, fast precision and
fast integers disabled. This states the runtime policy; it does not prove
binary or pixel equivalence to an offline `psp2cgc` build. The precision-sensitive
palette, matrix, blending and rotation paths still require physical validation.

## Build

Use the existing private dependency builder and the pinned VitaGL/VitaShaRK
revisions. Do not replace the SDK's installed copies. With those dependencies
available:

```sh
VITASDK=/path/to/vitasdk cmake --preset vita-baseline \
  -DYABAUSE_RUNTIME_SHADERS=ON \
  -DYABAUSE_VITAGL_ROOT=/path/to/private-vitagl \
  -DVITA_CCACHE=OFF -DCMAKE_BUILD_TYPE=Release
VITASDK=/path/to/vitasdk cmake --build --preset vita-baseline
```

This option is for VitaGL only. Native GXM probe/compositor configurations still
need offline GXP and reject the runtime option. The offline build path remains
available with `YABAUSE_RUNTIME_SHADERS=OFF` and a supplied `YABAUSE_PSP2CGC`.

## Timing and scope

Program objects retain the existing reuse/lifetime rules. Rotation variants
created lazily can compile after startup.

`YABAUSE_SHADER_CACHE` (default ON with runtime shaders) uses vitaGL's own
`HAVE_SHADER_CACHE`: build the private vitaGL with `--shader-cache`. Compiled
GXP go to `ux0:data/yabause-vita/shader_cache/cg-o3-strict`, keyed by a hash of
the expanded source, variant definitions included. The first launch compiles
each distinct source once; a vertex source shared by several programs is read
back from the cache after its first compilation. Later launches compile nothing.
The key does not cover compiler options or the shacccg module: the directory
names the policy, and must be deleted if either changes.
`vitagl-shader-cache.patch` keeps a failed compilation out of the cache, so
the next launch still reports it instead of loading an empty program, and
stores entries in flat `v/` and `f/` directories. Upstream creates 512 hash
subdirectories on the first launch; on the Vita that stalled `vglInit` before
the first frame.

Every compilation emits `runtime_shader_begin`/`runtime_shader_complete` with
source fingerprint, type and process timestamps; diagnostics include line
origins. A measurement containing compilation is a cold/runtime-compilation
measurement, not clean steady-state gameplay. Do not merely subtract compile
time from frame time; warm the relevant path and measure a compilation-free
window separately. Framebuffer correctness must be established before accepting
performance results.

## Host checks

```sh
python3 -m unittest discover -s tests -p 'test_*shader*build.py' -v
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/runtime_shader_stubs tests/test_runtime_shader_loader.c -o /tmp/runtime-loader
/tmp/runtime-loader
```

The packaging tests cover every Cg template, include expansion, definition
preservation and rejected inputs. The loader test uses GL/compiler doubles and
checks calls/failure handling. Neither test executes the Vita compiler or GPU.
