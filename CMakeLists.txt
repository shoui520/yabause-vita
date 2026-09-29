cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "Set VITASDK to the VitaSDK installation")
  endif()
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
endif()
project(yabause_vita LANGUAGES C CXX ASM)
# Compiler cache shared by every build directory: with base_dir at the source
# tree, per-directory paths (generated c68k tables, sources) hash the same, so
# a variant directory recompiles only objects whose flags actually differ.
# Settings are passed per invocation (no global ccache configuration).
option(VITA_CCACHE "Compile through ccache when available" ON)
find_program(VITA_CCACHE_PROGRAM ccache HINTS "$ENV{HOME}/.local/bin")
if(VITA_CCACHE AND VITA_CCACHE_PROGRAM)
  set(VITA_CCACHE_LAUNCHER env "CCACHE_BASEDIR=${CMAKE_CURRENT_SOURCE_DIR}" CCACHE_NOHASHDIR=1
      "CCACHE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/build/ccache" CCACHE_MAXSIZE=20G
      CCACHE_SLOPPINESS=time_macros,include_file_mtime,include_file_ctime "${VITA_CCACHE_PROGRAM}")
  set(CMAKE_C_COMPILER_LAUNCHER ${VITA_CCACHE_LAUNCHER})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${VITA_CCACHE_LAUNCHER})
endif()
option(VITA_SH2_READ_HELPER_O3 "Optimize SH-2 read helpers for Cortex-A9 without changing write helpers" OFF)
if(VITA_SH2_READ_HELPER_O3)
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2CInterface.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_READ_HELPER_O3)
endif()
option(VITA_ROTATION_UNIFORMS "Job-constant rotation kx/ky as uniforms (two fewer dependent reads)" ON)
option(VITA_ROTATION_WORD_SELECT "Experimental select-before-decode rotation word reads" OFF)
option(VITA_ROTATION_PATTERN_CACHE "Experimental decoded rotation tile descriptors" OFF)
option(VITA_ROTATION_COEFF_PAIR "Experimental paired F32 rotation row coordinates" OFF)
option(VITA_ROTATION_COEFF_LINEAR "Experimental single-row paired coefficient texture" OFF)
option(VITA_ROTATION_COEFF_VARYING "Experimental vertex-provided coefficient texture coordinates" OFF)
if(VITA_ROTATION_COEFF_VARYING)
  if(NOT VITA_ROTATION_COEFF_LINEAR)
    message(FATAL_ERROR "Coefficient varying requires single-row coefficients")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_COEFF_VARYING)
endif()
if(VITA_ROTATION_COEFF_LINEAR)
  if(NOT VITA_ROTATION_COEFF_PAIR)
    message(FATAL_ERROR "Linear coefficients require paired coefficients")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_COEFF_LINEAR)
endif()
option(VITA_ROTATION_BYTE_VRAM "Experimental direct U8 rotation character fetches" OFF)
option(VITA_ROTATION_INTEGER_PATTERN "Experimental integer rotation descriptor queries" OFF)
option(VITA_ROTATION_OUTPUT_REUSE "Experimental exact private rotation output reuse" OFF)
option(VITA_ROTATION_OUTPUT_GENERATION "Experimental tracked VRAM identity for output reuse" OFF)
option(VITA_ROTATION_BYTE_RESIDENT "Reuse unchanged indexed rotation VRAM texture uploads" OFF)
if(VITA_ROTATION_BYTE_RESIDENT)
  if(NOT VITA_ROTATION_BYTE_VRAM OR NOT VITA_ROTATION_OUTPUT_GENERATION OR VITA_ROTATION_VRAM_REUSE)
    message(FATAL_ERROR "Byte residency requires byte VRAM, output identity and no legacy split reuse")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_BYTE_RESIDENT)
endif()
option(VITA_ROTATION_SNAPSHOT_REUSE "Reuse unchanged owned rotation input snapshots" OFF)
if(VITA_ROTATION_SNAPSHOT_REUSE)
  if(NOT VITA_ROTATION_OUTPUT_GENERATION OR VITA_ROTATION_REUSE_PROBE)
    message(FATAL_ERROR "Snapshot reuse requires output generation and no copy probe")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_SNAPSHOT_REUSE)
endif()
if(VITA_ROTATION_OUTPUT_GENERATION)
  if(NOT VITA_ROTATION_OUTPUT_REUSE OR NOT VITA_ROTATION_PATTERN_CACHE)
    message(FATAL_ERROR "Output generation requires output reuse and tracked pattern-cache writes")
  endif()
  set_property(SOURCE src/core/vdp2.cpp src/video/opengl/vidogl.c
    src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_OUTPUT_GENERATION)
endif()
set(VITA_M68K_CODE_RESERVE_KIB "0" CACHE STRING
  "Reserve-only 68K executable arena partition, 0..15360 KiB in 4 KiB steps (does not enable native 68K)")
if(NOT VITA_M68K_CODE_RESERVE_KIB MATCHES "^(0|[1-9][0-9]*)$")
  message(FATAL_ERROR "VITA_M68K_CODE_RESERVE_KIB must be a nonnegative decimal integer")
endif()
math(EXPR M68K_RESERVE_REMAINDER "${VITA_M68K_CODE_RESERVE_KIB} % 4")
if(VITA_M68K_CODE_RESERVE_KIB GREATER 15360 OR NOT M68K_RESERVE_REMAINDER EQUAL 0)
  message(FATAL_ERROR "68K reserve must be 4 KiB aligned and leave at least 1 MiB for SH-2")
endif()
add_compile_definitions(VITA_M68K_CODE_RESERVE_KIB=${VITA_M68K_CODE_RESERVE_KIB})
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-O3 -g -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -ffunction-sections -fdata-sections -fno-strict-aliasing)
# The C68K interpreter takes minutes to compile. It is its own object library,
# created before the LTO/PGO options below so it keeps one command line (and
# its ccache entry) across those variants; it takes the app's definitions and
# include directories.
# Profile-guided optimization. generate: the app writes .gcda files to
# ux0:data/yabause-vita/pgo when a benchmark window completes (runs merge);
# use: build from those files copied to VITA_PGO_DIR on the host.
set(VITA_PGO "" CACHE STRING "Profile-guided optimization: empty, generate or use")
set(VITA_PGO_DIR "" CACHE PATH "Host directory with the .gcda files (VITA_PGO=use)")
if(VITA_PGO STREQUAL "generate")
  # vitasdk's libgcov has no value or time profilers: arc counts only.
  # vitasdk's libgcov has no file I/O (src/vita/pgo_dump.c writes the data)
  # and no value or time profilers: arc counts only.
  add_compile_options(-fprofile-generate=ux0:data/yabause-vita/pgo -fprofile-info-section=gcov_info
    -fno-profile-values -fno-profile-reorder-functions)
  add_link_options(-fprofile-generate=ux0:data/yabause-vita/pgo)
  add_compile_definitions(VITA_PGO_GENERATE)
elseif(VITA_PGO STREQUAL "use")
  # main.c differs from the generate build (the dump call): its profile is dropped.
  add_compile_options(-fprofile-use=${VITA_PGO_DIR} -fprofile-partial-training -Wno-missing-profile
    -Wno-error=coverage-mismatch)
elseif(NOT VITA_PGO STREQUAL "")
  message(FATAL_ERROR "VITA_PGO must be empty, generate or use")
endif()
add_compile_definitions(VITA HAVE_STDINT_H HAVE_SYS_TIME_H HAVE_STRCASECMP
  HAVE_C99_VARIADIC_MACROS HAVE_FLOORF HAVE_LROUND NO_CLI
  PACKAGE_VERSION="0.1.0" VERSION="0.1.0")
# GCC explicitly recommends considering -fno-gcse for computed-goto interpreters.
# Keep the giant C68K dispatch function out of O3's loop transformations.
option(VITA_C68K_ARM "Compile the C68K interpreter as A32 instead of Thumb-2" OFF)
if(VITA_C68K_ARM)
  set_source_files_properties(src/core/c68k/c68kexec.c PROPERTIES
    COMPILE_OPTIONS "-O2;-fno-gcse;-fno-crossjumping;-marm")
else()
  set_source_files_properties(src/core/c68k/c68kexec.c PROPERTIES
    COMPILE_OPTIONS "-O2;-fno-gcse;-fno-crossjumping")
endif()
option(VITA_C68K_RAM_READS "Inline ordinary C68K sound-RAM reads (experimental)" OFF)
option(VITA_M68K_NATIVE_GUARDS "Experimental native-tier synchronization only; does not enable native execution" OFF)
option(VITA_M68K_NATIVE_EXECUTION "Experimental cached A32 68K tier, CPU-paced sound only" OFF)
if(VITA_C68K_RAM_READS)
  set_property(SOURCE src/core/c68k/c68kexec.c src/vita/c68k_read_test.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_C68K_RAM_READS)
endif()

# C68K's opcode tables are generated on the build host, never on the Vita.
find_program(HOST_CC NAMES cc gcc REQUIRED NO_CMAKE_FIND_ROOT_PATH)
set(C68K "${CMAKE_CURRENT_SOURCE_DIR}/src/core/c68k")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/c68k/generated")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/c68k/gen68k"
  COMMAND "${HOST_CC}" -O2 -DC68K_GEN -DHAVE_STDINT_H
  "${C68K}/gen68k.c" "${C68K}/c68k.c" "${C68K}/c68kexec.c"
  -o "${CMAKE_CURRENT_BINARY_DIR}/c68k/gen68k"
  DEPENDS "${C68K}/gen68k.c" "${C68K}/c68k.c" "${C68K}/c68kexec.c"
    "${C68K}/c68k.h" "${C68K}/c68kmac.inc" "${C68K}/sound_ram_read.h" "${C68K}/../core.h"
  VERBATIM)
set(C68K_TABLES "${CMAKE_CURRENT_BINARY_DIR}/c68k/c68k_ini.inc")
set(C68K_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/c68k/generated/c68k_ini.inc")
foreach(op RANGE 0 15)
  string(SUBSTRING "0123456789ABCDEF" ${op} 1 hex)
  list(APPEND C68K_TABLES "${CMAKE_CURRENT_BINARY_DIR}/c68k/c68k_op${hex}.inc")
  list(APPEND C68K_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/c68k/generated/c68k_op${hex}.inc")
endforeach()
add_custom_command(OUTPUT ${C68K_TABLES}
  COMMAND "${CMAKE_CURRENT_BINARY_DIR}/c68k/gen68k"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${C68K_GENERATED}
    "${CMAKE_CURRENT_BINARY_DIR}/c68k"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/c68k/generated"
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/c68k/gen68k" VERBATIM)
add_custom_target(c68k_tables DEPENDS ${C68K_TABLES})

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/core/*.c src/core/*.cpp)
option(VITA_STACK_CHECKS "Trap on reference-core stack corruption" ON)
option(VITA_SH2_DYNAREC "Use the ARM SH-2 dynarec (OFF selects the reference interpreter)" ON)
option(VITA_SH2_INLINE_DISPATCH "Inline the SH-2 block dispatcher into its budget loop (experimental)" OFF)
option(VITA_SH2_CACHED_DISPATCH "Split SH-2 cached dispatch from compilation and uncommon mappings (experimental)" OFF)
option(VITA_SH2_CHAIN_ABI "Enable experimental private SH-2 native block entry/return ABI" OFF)
option(VITA_SH2_NATIVE_CHAIN "Run cached SH-2 blocks inside the A32 scheduling loop (experimental)" OFF)
option(VITA_SH2_POLL_STEP "Execute exact single-iteration polling recipes without native block entry (experimental)" OFF)
option(VITA_SH2_POLL_FUSION "Fuse exact polling recipes into an A32 block body (experimental)" OFF)
option(VITA_SH2_STAGE_CLOCKS "Keep duplicate legacy per-deciline SH2 wall-clock measurements" ON)
if(NOT VITA_SH2_STAGE_CLOCKS)
  set_property(SOURCE src/core/yabause.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_NO_SH2_STAGE_CLOCKS)
endif()
if(VITA_SH2_POLL_FUSION)
  if(NOT VITA_SH2_DYNAREC OR VITA_SH2_POLL_STEP)
    message(FATAL_ERROR "Polling fusion requires the dynarec and excludes C++ polling steps")
  endif()
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp src/vita/sh2_compiler_test.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_POLL_FUSION)
endif()
if(VITA_SH2_POLL_STEP)
  if(NOT VITA_SH2_DYNAREC OR VITA_SH2_NATIVE_CHAIN)
    message(FATAL_ERROR "Polling steps require the dynarec and standalone dispatch")
  endif()
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp src/vita/sh2_compiler_test.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_POLL_STEP)
endif()
if(VITA_SH2_NATIVE_CHAIN)
  if(NOT VITA_SH2_CHAIN_ABI OR NOT VITA_SH2_DYNAREC)
    message(FATAL_ERROR "Native chaining requires CHAIN_ABI and the SH-2 dynarec")
  endif()
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_NATIVE_CHAIN)
endif()
if(VITA_SH2_CHAIN_ABI)
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_CHAIN_ABI)
  set_property(SOURCE src/core/sh2_dynarec/dynalib_arm.s APPEND PROPERTY
    COMPILE_OPTIONS "-Wa,-defsym,VITA_SH2_CHAIN_ABI=1")
endif()
if(VITA_SH2_CACHED_DISPATCH)
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_CACHED_DISPATCH)
endif()
option(VITA_SH2_RAM_LOADS "Use guarded A32 cached work-RAM loads (experimental)" OFF)
option(VITA_SH2_WRITE_PREFLIGHT "Check empty high-RAM code ownership before invalidation calls" OFF)
option(VITA_SH2_WRITE_HELPER_O3 "Optimize SH-2 write helpers while preserving invalidation" OFF)
if(VITA_SH2_WRITE_HELPER_O3)
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2CInterface.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_WRITE_HELPER_O3)
endif()
if(VITA_SH2_WRITE_PREFLIGHT)
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2CInterface.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_WRITE_PREFLIGHT)
endif()
if(VITA_SH2_RAM_LOADS)
  if(NOT VITA_SH2_DYNAREC)
    message(FATAL_ERROR "RAM load lowering requires the SH-2 dynarec")
  endif()
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp src/vita/sh2_compiler_test.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_RAM_LOADS)
endif()
if(VITA_SH2_INLINE_DISPATCH)
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_INLINE_DISPATCH)
endif()
option(VITA_SH2_A9_REGIONS "Use validated A32 arithmetic register regions (experimental)" OFF)
option(VITA_SH2_IMMEDIATE_LOGIC "Keep immediate AND/OR/XOR in A32 register regions" OFF)
if(VITA_SH2_IMMEDIATE_LOGIC)
  if(NOT VITA_SH2_A9_REGIONS)
    message(FATAL_ERROR "Immediate logic requires A9 regions")
  endif()
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_IMMEDIATE_LOGIC)
endif()
option(VITA_SH2_PC_LOADS "Direct live cached-WRAM PC-relative loads in A32 regions" OFF)
if(VITA_SH2_PC_LOADS)
  if(NOT VITA_SH2_A9_REGIONS OR VITA_SH2_NATIVE_CHAIN)
    message(FATAL_ERROR "PC-relative loads require A9 regions and exclude alias-unchecked native chains")
  endif()
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp src/vita/sh2_compiler_test.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_PC_LOADS)
endif()
option(VITA_SH2_MAC_REGIONS "Keep MUL.L/MAC reads resident (experimental; boot regression)" OFF)
if(VITA_SH2_MAC_REGIONS)
  if(NOT VITA_SH2_A9_REGIONS)
    message(FATAL_ERROR "MAC regions require A9_REGIONS")
  endif()
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_MAC_REGIONS)
endif()
option(VITA_SH2_RESIDENT_LOOPS "Keep register-only SH-2 self-loops resident in A32 registers (experimental)" OFF)
option(VITA_SH2_POLL_SKIP "Skip proven stable polling iterations within an SH-2 slice (experimental)" OFF)
if(VITA_SH2_POLL_SKIP)
  if(NOT VITA_SH2_DYNAREC OR VITA_SH2_NATIVE_CHAIN)
    message(FATAL_ERROR "Poll skipping requires the dynarec and excludes native chaining")
  endif()
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp src/vita/sh2_compiler_test.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_POLL_SKIP)
endif()
if(VITA_SH2_RESIDENT_LOOPS)
  if(NOT VITA_SH2_A9_REGIONS OR VITA_SH2_NATIVE_CHAIN)
    message(FATAL_ERROR "Resident loops require A9_REGIONS and currently exclude NATIVE_CHAIN")
  endif()
  set_property(SOURCE src/core/sh2_dynarec/DynarecSh2.cpp src/vita/sh2_compiler_test.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SH2_RESIDENT_LOOPS)
endif()
if(VITA_SH2_A9_REGIONS)
  if(NOT VITA_SH2_DYNAREC)
    message(FATAL_ERROR "A9 regions require the SH-2 dynarec")
  endif()
  add_compile_definitions(VITA_SH2_A9_REGIONS)
endif()
if(VITA_STACK_CHECKS)
  set_source_files_properties(${CORE_SOURCES} src/vita/main.c PROPERTIES
    COMPILE_OPTIONS -fstack-protector-strong)
endif()
file(GLOB CHDR_SOURCES CONFIGURE_DEPENDS third_party/libchdr/src/*.c)
add_executable(yabause-vita ${CORE_SOURCES} src/core/titan/titan.c
  src/core/c68k/c68k.c src/core/c68k/c68kexec.c
  ${CHDR_SOURCES} third_party/libchdr/deps/lzma-19.00/src/LzmaDec.c
  third_party/libchdr/deps/lzma-19.00/src/LzmaEnc.c
  third_party/libchdr/deps/lzma-19.00/src/LzFind.c
  src/vita/main.c src/vita/threads.c src/vita/present.c src/vita/stack_guard.c
  src/vita/telemetry.c src/vita/c68k_read_test.c)
option(VITA_ASYNC_AUDIO_OUTPUT "Decouple host PCM playback from the SCSP completion barrier" OFF)
if(VITA_ASYNC_AUDIO_OUTPUT)
  target_sources(yabause-vita PRIVATE src/vita/audio_output_queue.c)
  set_property(SOURCE src/vita/main.c APPEND PROPERTY COMPILE_DEFINITIONS VITA_ASYNC_AUDIO_OUTPUT)
endif()
if(VITA_M68K_NATIVE_GUARDS)
  target_compile_definitions(yabause-vita PRIVATE VITA_M68K_NATIVE_GUARDS)
  target_sources(yabause-vita PRIVATE src/vita/c68k_native_guard.cpp)
endif()
if(VITA_M68K_NATIVE_EXECUTION)
  if(NOT VITA_M68K_NATIVE_GUARDS OR NOT VITA_SH2_DYNAREC OR VITA_M68K_CODE_RESERVE_KIB LESS 516)
    message(FATAL_ERROR "Native 68K requires guards, SH-2 VM arena, and at least 516 KiB reserved code")
  endif()
  target_compile_definitions(yabause-vita PRIVATE VITA_M68K_NATIVE_EXECUTION C68K_NATIVE_REGIONS)
  target_sources(yabause-vita PRIVATE src/vita/c68k_runtime.cpp)
endif()
set_source_files_properties(src/vita/telemetry.c PROPERTIES
  COMPILE_OPTIONS "-fstack-usage;-Werror=frame-larger-than=4096")
if(VITA_SH2_DYNAREC)
  target_sources(yabause-vita PRIVATE
    src/core/sh2_dynarec/DynarecSh2.cpp
    src/core/sh2_dynarec/DynarecSh2CInterface.cpp
    src/core/sh2_dynarec/dynalib_arm.s
    src/vita/sh2_code_memory.cpp src/vita/sh2_compiler_test.cpp)
  target_compile_definitions(yabause-vita PRIVATE VITA_SH2_DYNAREC)
  if(VITA_SH2_NATIVE_CHAIN)
    target_sources(yabause-vita PRIVATE src/core/sh2_dynarec/chain_runner.S)
  endif()
  target_link_libraries(yabause-vita PRIVATE SceSysmem_stub)
endif()
set_source_files_properties(third_party/libchdr/deps/lzma-19.00/src/LzmaEnc.c
  PROPERTIES COMPILE_DEFINITIONS _7ZIP_ST)
add_dependencies(yabause-vita c68k_tables)
set(YABAUSE_PSP2CGC "$ENV{YABAUSE_PSP2CGC}" CACHE FILEPATH "User-supplied PSP2 shader compiler")
option(YABAUSE_GXM_PROBE "Validate GXM resource setup before starting emulation" OFF)
option(YABAUSE_GXM_COMPOSITOR "Development integration: GPU composition/presentation, CPU rasterization" OFF)
if((YABAUSE_GXM_PROBE OR YABAUSE_GXM_COMPOSITOR) AND NOT YABAUSE_PSP2CGC)
  message(FATAL_ERROR "GXM probing requires YABAUSE_PSP2CGC")
endif()
if(YABAUSE_PSP2CGC)
  if(NOT EXISTS "${YABAUSE_PSP2CGC}")
    message(FATAL_ERROR "YABAUSE_PSP2CGC must name an existing shader compiler")
  endif()
  find_program(HOST_PYTHON NAMES python3 REQUIRED NO_CMAKE_FIND_ROOT_PATH)
  foreach(shader_name IN ITEMS tile_v tile_f compose_f present_f)
    if(shader_name MATCHES "_v$")
      set(profile sce_vp_psp2)
    else()
      set(profile sce_fp_psp2)
    endif()
    set(shader "${CMAKE_CURRENT_BINARY_DIR}/gxm/${shader_name}.h")
    add_custom_command(OUTPUT "${shader}"
      BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/gxm/${shader_name}.gxp"
      COMMAND "${HOST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_shader.py"
        --compiler "${YABAUSE_PSP2CGC}" --profile "${profile}"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/src/vita/gxm/shaders/${shader_name}.cg"
        --output "${shader}" --symbol "yabause_${shader_name}"
      DEPENDS "src/vita/gxm/shaders/${shader_name}.cg" tools/compile_shader.py
      VERBATIM)
    list(APPEND YABAUSE_GXM_HEADERS "${shader}")
  endforeach()
  add_custom_target(yabause-gxm-shaders DEPENDS ${YABAUSE_GXM_HEADERS})
  # Native program families for the in-progress vitaGL renderer. Kept separate
  # from the currently selected renderer until complete integration is ready.
  option(VITA_NORMAL_DIRECT_UV "Experimental vertex-normalized nearest atlas sampling" OFF)
  option(VITA_FB_BALANCED_SELECT "Experimental balanced framebuffer register selection" OFF)
  option(VITA_PALETTE_FLOAT_ADDRESS "Experimental exact float palette address assembly" OFF)
  foreach(shader_name IN ITEMS normal_v normal_direct_v normal_reference_f normal_f cram_f opaque_normal_f opaque_cram_f priority_f line_offset_f add_color_f
      sprite_v sprite_normal_f sprite_gouraud_f sprite_spd_f sprite_half_f
      sprite_gouraud_half_f sprite_shadow_f sprite_mesh_f sprite_luminance_f
      window_v window_f line_insert_f line_insert_cram_f
      line_insert_dest_f line_insert_dest_cram_f rbg_line_f framebuffer_v
      fb_normal_f
      fb_dest_f
      fb_rate_0_f
      fb_add_0_f
      fb_rate_1_f
      fb_add_1_f
      fb_rate_2_f
      fb_add_2_f
      fb_rate_3_f
      fb_add_3_f
      fb_normal_hblank_f
      fb_dest_hblank_f
      fb_rate_0_hblank_f
      fb_add_0_hblank_f
      fb_rate_1_hblank_f
      fb_add_1_hblank_f
      fb_rate_2_hblank_f
      fb_add_2_hblank_f
      fb_rate_3_hblank_f
      fb_add_3_hblank_f
      fb_dest_line_0_f
      fb_rate_line_0_f
      fb_add_line_0_f
      fb_dest_line_1_f
      fb_rate_line_1_f
      fb_add_line_1_f
      fb_dest_line_2_f
      fb_rate_line_2_f
      fb_add_line_2_f
      fb_dest_line_3_f
      fb_rate_line_3_f
      fb_add_line_3_f
      fb_shadow_f utility_v back_f copy_f copy_written_f blur_f mosaic_f perline_f
      add_shadow_f rotation_bitmap_v rotation_coeff_v rotation_bitmap_f rotation_rows_v rotation_rows_f
      rotation_split_f rotation_constk_f rotation_constk_planes_f rotation_constk_float_f
      rotation_decode_map_f rotation_cached_map_f)
    if(shader_name MATCHES "_v$")
      set(profile sce_vp_psp2)
    else()
      set(profile sce_fp_psp2)
    endif()
    set(shader "${CMAKE_CURRENT_BINARY_DIR}/vitagl/${shader_name}.h")
    set(shader_deps)
    if(shader_name MATCHES "^rotation_(rows|split|constk|constk_planes|constk_float|decode_map|cached_map)_f$")
      list(APPEND shader_deps src/video/opengl/shaders/rotation_bitmap_f.cg
        src/video/opengl/shaders/packed_texture.h)
    endif()
    if(shader_name MATCHES "^(normal_reference_f|normal_f|cram_f|opaque_normal_f|opaque_cram_f|priority_f|line_offset_f|add_color_f)$")
      list(APPEND shader_deps src/video/opengl/shaders/normal_common.cg)
    elseif(shader_name MATCHES "^sprite_.*_f$")
      list(APPEND shader_deps src/video/opengl/shaders/sprite_common.cg)
    elseif(shader_name MATCHES "^line_insert.*_f$")
      list(APPEND shader_deps src/video/opengl/shaders/line_insert_common.cg)
    elseif(shader_name MATCHES "^fb_.*_f$")
      list(APPEND shader_deps src/video/opengl/shaders/framebuffer_common.cg)
    elseif(shader_name MATCHES "^(blur|mosaic|perline)_f$")
      list(APPEND shader_deps src/video/opengl/shaders/effect_common.cg)
      if(shader_name STREQUAL "mosaic_f")
        list(APPEND shader_deps src/video/opengl/shaders/mosaic_cell.cg)
      endif()
    endif()
    if(shader_name MATCHES "^(normal_reference_f|normal_f|cram_f|opaque_normal_f|opaque_cram_f|priority_f|line_offset_f|add_color_f|sprite_.*_f|line_insert.*_f|fb_.*_f|add_shadow_f|rbg_line_f|rotation_bitmap_f)$")
      list(APPEND shader_deps src/video/opengl/shaders/packed_texture.h)
    endif()
    set(normal_defines)
    if(VITA_PALETTE_FLOAT_ADDRESS AND shader_name MATCHES "^(normal_f|cram_f|priority_f|line_offset_f|add_color_f)$")
      list(APPEND normal_defines --define "PALETTE_FLOAT_ADDRESS=1")
    endif()
    if(VITA_FB_BALANCED_SELECT AND shader_name MATCHES "^fb_.*_f$")
      list(APPEND normal_defines --define "FB_BALANCED_SELECT=1")
    endif()
    if(VITA_NORMAL_DIRECT_UV AND shader_name MATCHES "^(normal_f|cram_f|priority_f|line_offset_f|add_color_f)$")
      list(APPEND normal_defines --define "NORMAL_DIRECT_UV=1")
    endif()
    add_custom_command(OUTPUT "${shader}"
      BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/vitagl/${shader_name}.gxp"
      COMMAND "${HOST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_shader.py"
        --compiler "${YABAUSE_PSP2CGC}" --profile "${profile}"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/src/video/opengl/shaders/${shader_name}.cg"
        --output "${shader}" --symbol "ygl_vita_${shader_name}" ${normal_defines}
      DEPENDS "src/video/opengl/shaders/${shader_name}.cg"
        ${shader_deps} tools/compile_shader.py
      VERBATIM)
    list(APPEND YABAUSE_VITAGL_HEADERS "${shader}")
  endforeach()
  # Bounded native specializations: four formats x (bitmap + six tile layouts).
  # All are compiled offline; only used programs are registered on the Vita.
  set(rotation_variant_defines)
  set(rotation_pair_defines)
  if(VITA_ROTATION_COEFF_VARYING)
    list(APPEND rotation_pair_defines --define "ROTATION_COEFF_VARYING=1")
  endif()
  if(VITA_ROTATION_COEFF_LINEAR)
    list(APPEND rotation_pair_defines --define "ROTATION_COEFF_LINEAR=1")
  endif()
  if(VITA_ROTATION_INTEGER_PATTERN)
    list(APPEND rotation_pair_defines --define "ROTATION_INTEGER_PATTERN=1")
  endif()
  if(VITA_ROTATION_BYTE_VRAM)
    list(APPEND rotation_pair_defines --define "ROTATION_BYTE_VRAM=1")
  endif()
  if(VITA_ROTATION_WORD_SELECT)
    list(APPEND rotation_variant_defines --define "ROTATION_WORD_SELECT=1")
  endif()
  if(VITA_ROTATION_UNIFORMS)
    list(APPEND rotation_variant_defines --define "ROTATION_CONST_K=1")
  endif()
  set(rotation_includes "/* Generated rotation shader table. */\n#pragma once\n")
  set(rotation_table "typedef struct { const unsigned char *data; unsigned size; } VitaRotationBinary;\nstatic const VitaRotationBinary vita_rotation_binaries[] = {\n")
  set(pattern_table "")
  set(pair_table "")
  if(VITA_ROTATION_COEFF_PAIR)
    set(pair_table "static const VitaRotationBinary vita_rotation_pair_binaries[] = {\n")
  endif()
  if(VITA_ROTATION_PATTERN_CACHE)
    set(pattern_table "static const VitaRotationBinary vita_rotation_pattern_binaries[] = {\n")
  endif()
  foreach(format 0 1 3 4)
    foreach(layout RANGE 0 6)
      set(name "rotation_${format}_${layout}_f")
      set(bitmap 0)
      set(cells 1)
      set(words 1)
      set(aux 0)
      if(layout EQUAL 0)
        set(bitmap 1)
      else()
        math(EXPR cells "1 + (${layout}-1)/3")
        math(EXPR name_mode "(${layout}-1)%3")
        if(name_mode EQUAL 2)
          set(words 2)
        else()
          set(aux ${name_mode})
        endif()
      endif()
      set(header "${CMAKE_CURRENT_BINARY_DIR}/vitagl/${name}.h")
      add_custom_command(OUTPUT "${header}"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/vitagl/${name}.gxp"
        COMMAND "${HOST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_shader.py"
          --compiler "${YABAUSE_PSP2CGC}" --profile sce_fp_psp2
          --source "${CMAKE_CURRENT_SOURCE_DIR}/src/video/opengl/shaders/rotation_bitmap_f.cg"
          --output "${header}" --symbol "ygl_vita_${name}"
          --define "ROTATION_FORMAT=${format}" --define "ROTATION_BITMAP=${bitmap}"
          --define "ROTATION_CELLS=${cells}" --define "ROTATION_WORDS=${words}"
          --define "ROTATION_AUX=${aux}" ${rotation_variant_defines}
        DEPENDS src/video/opengl/shaders/rotation_bitmap_f.cg
          src/video/opengl/shaders/packed_texture.h tools/compile_shader.py VERBATIM)
      list(APPEND YABAUSE_VITAGL_HEADERS "${header}")
      string(APPEND rotation_includes "#include \"${name}.h\"\n")
      string(APPEND rotation_table "{ygl_vita_${name}, sizeof(ygl_vita_${name})},\n")
      if(VITA_ROTATION_PATTERN_CACHE)
        set(pattern_name "rotation_pattern_${format}_${layout}_f")
        set(pattern_header "${CMAKE_CURRENT_BINARY_DIR}/vitagl/${pattern_name}.h")
        add_custom_command(OUTPUT "${pattern_header}"
          BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/vitagl/${pattern_name}.gxp"
          COMMAND "${HOST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_shader.py"
            --compiler "${YABAUSE_PSP2CGC}" --profile sce_fp_psp2
            --source "${CMAKE_CURRENT_SOURCE_DIR}/src/video/opengl/shaders/rotation_bitmap_f.cg"
            --output "${pattern_header}" --symbol "ygl_vita_${pattern_name}"
            --define "ROTATION_PATTERN_CACHE=1"
            --define "ROTATION_FORMAT=${format}" --define "ROTATION_BITMAP=${bitmap}"
            --define "ROTATION_CELLS=${cells}" --define "ROTATION_WORDS=${words}"
            --define "ROTATION_AUX=${aux}" ${rotation_variant_defines}
          DEPENDS src/video/opengl/shaders/rotation_bitmap_f.cg
            src/video/opengl/shaders/packed_texture.h tools/compile_shader.py VERBATIM)
        list(APPEND YABAUSE_VITAGL_HEADERS "${pattern_header}")
        string(APPEND rotation_includes "#include \"${pattern_name}.h\"\n")
        string(APPEND pattern_table "{ygl_vita_${pattern_name}, sizeof(ygl_vita_${pattern_name})},\n")
        if(VITA_ROTATION_COEFF_PAIR)
          set(pair_name "rotation_pair_${format}_${layout}_f")
          set(pair_header "${CMAKE_CURRENT_BINARY_DIR}/vitagl/${pair_name}.h")
          add_custom_command(OUTPUT "${pair_header}"
            BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/vitagl/${pair_name}.gxp"
            COMMAND "${HOST_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_shader.py"
              --compiler "${YABAUSE_PSP2CGC}" --profile sce_fp_psp2
              --source "${CMAKE_CURRENT_SOURCE_DIR}/src/video/opengl/shaders/rotation_bitmap_f.cg"
              --output "${pair_header}" --symbol "ygl_vita_${pair_name}"
              --define "ROTATION_PATTERN_CACHE=1" --define "ROTATION_COEFF_PAIR=1"
              --define "ROTATION_FORMAT=${format}" --define "ROTATION_BITMAP=${bitmap}"
              --define "ROTATION_CELLS=${cells}" --define "ROTATION_WORDS=${words}"
              --define "ROTATION_AUX=${aux}" ${rotation_variant_defines} ${rotation_pair_defines}
            DEPENDS src/video/opengl/shaders/rotation_bitmap_f.cg
              src/video/opengl/shaders/packed_texture.h tools/compile_shader.py VERBATIM)
          list(APPEND YABAUSE_VITAGL_HEADERS "${pair_header}")
          string(APPEND rotation_includes "#include \"${pair_name}.h\"\n")
          string(APPEND pair_table "{ygl_vita_${pair_name}, sizeof(ygl_vita_${pair_name})},\n")
        endif()
      endif()
    endforeach()
  endforeach()
  if(VITA_ROTATION_PATTERN_CACHE)
    string(APPEND pattern_table "};\n")
  endif()
  if(VITA_ROTATION_COEFF_PAIR)
    string(APPEND pair_table "};\n")
  endif()
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vitagl/rotation_variants.h"
    CONTENT "${rotation_includes}${rotation_table}};\n${pattern_table}${pair_table}")
  add_custom_target(yabause-vitagl-shaders DEPENDS ${YABAUSE_VITAGL_HEADERS})
  add_library(yabause-gxm OBJECT src/vita/gxm/device.c src/vita/gxm/texture.c ${YABAUSE_GXM_HEADERS})
  target_include_directories(yabause-gxm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gxm")
  if(YABAUSE_GXM_PROBE OR YABAUSE_GXM_COMPOSITOR)
    target_sources(yabause-vita PRIVATE $<TARGET_OBJECTS:yabause-gxm>)
    target_link_libraries(yabause-vita PRIVATE SceGxm_stub)
  endif()
  if(YABAUSE_GXM_PROBE)
    set_property(SOURCE src/vita/main.c APPEND PROPERTY COMPILE_DEFINITIONS YABAUSE_GXM_PROBE)
  endif()
  if(YABAUSE_GXM_COMPOSITOR)
    set_property(SOURCE src/vita/main.c src/core/titan/titan.c APPEND PROPERTY
      COMPILE_DEFINITIONS YABAUSE_GXM_COMPOSITOR)
  endif()
endif()
include(cmake/VitaGL.cmake)
if(VITA_ROTATION_INTEGER_PATTERN AND NOT VITA_ROTATION_COEFF_PAIR)
  message(FATAL_ERROR "Integer descriptors require paired-coefficient variants")
endif()
if(VITA_ROTATION_BYTE_VRAM)
  if(NOT VITA_ROTATION_COEFF_PAIR)
    message(FATAL_ERROR "Byte VRAM requires paired coefficients and pattern descriptors")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_BYTE_VRAM)
endif()
if(VITA_ROTATION_COEFF_PAIR)
  if(NOT VITA_ROTATION_PATTERN_CACHE)
    message(FATAL_ERROR "Paired coefficients require the pattern-cache experiment")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_COEFF_PAIR)
endif()
option(VITA_ROTATION_VARIANTS "Use exact format/layout rotation shader specializations" ${VITA_ROTATION_UNIFORMS})
if(VITA_ROTATION_PATTERN_CACHE)
  if(NOT VITA_ROTATION_VARIANTS OR NOT VITA_ROTATION_UNIFORMS OR
      VITA_ROTATION_MAP_CACHE OR VITA_ROTATION_FLOAT_COEFFICIENTS OR
      VITA_ROTATION_PLANE_UNIFORMS OR VITA_ROTATION_VRAM_REUSE OR VITA_ROTATION_ROWS)
    message(FATAL_ERROR "Pattern cache requires constant-k variants without other rotation experiments")
  endif()
  add_compile_definitions(VITA_ROTATION_PATTERN_CACHE)
endif()
option(VITA_ROTATION_ROWS "Known-incorrect row-geometry experiment (diagnostics only)" OFF)
option(VITA_ATLAS_REPLACE "Replace CPU-authoritative atlas storage without old GPU backing copy" ON)
option(VITA_ATLAS_TRIM "Upload only occupied CPU atlas rows" ${VITA_ATLAS_REPLACE})
option(VITA_ATLAS_PRIVATE_TRIM "Exclude private rotation rectangles from atlas upload extent" OFF)
option(VITA_ATLAS_CONTENT_REUSE "Reuse byte-identical CPU-authoritative atlas uploads" OFF)
if(VITA_ATLAS_CONTENT_REUSE)
  if(NOT VITA_ATLAS_REPLACE OR NOT VITA_ATLAS_TRIM)
    message(FATAL_ERROR "Atlas content reuse requires replacement and trimming")
  endif()
  set_property(SOURCE src/video/opengl/ygles.c APPEND PROPERTY COMPILE_DEFINITIONS VITA_ATLAS_CONTENT_REUSE)
endif()
if(VITA_ATLAS_PRIVATE_TRIM)
  if(NOT VITA_ATLAS_TRIM)
    message(FATAL_ERROR "Private atlas trim requires atlas trimming")
  endif()
  set_property(SOURCE src/video/opengl/ygles.c APPEND PROPERTY COMPILE_DEFINITIONS VITA_ATLAS_PRIVATE_TRIM)
endif()
if(VITA_ATLAS_TRIM)
  if(NOT VITA_ATLAS_REPLACE)
    message(FATAL_ERROR "Trimmed atlas requires authoritative CPU replacement")
  endif()
  set_property(SOURCE src/video/opengl/ygles.c APPEND PROPERTY COMPILE_DEFINITIONS VITA_ATLAS_TRIM)
endif()
option(VITA_DIAG_AUDIO_UNPACED "Diagnostics: hash but do not play audio, removing real-time pacing" OFF)
if(VITA_DIAG_AUDIO_UNPACED)
  set_property(SOURCE src/vita/main.c APPEND PROPERTY COMPILE_DEFINITIONS VITA_DIAG_AUDIO_UNPACED)
endif()
option(VITA_DIAG_NO_VSYNC "Diagnostics: disable vitaGL vsync to test display pacing" OFF)
if(VITA_DIAG_NO_VSYNC)
  set_property(SOURCE src/vita/main.c APPEND PROPERTY COMPILE_DEFINITIONS VITA_DIAG_NO_VSYNC)
endif()
option(VITA_DIAG_GPU_STAGES "Diagnostics: forced glFinish completion waits per render stage" OFF)
option(VITA_FB_PRIORITY_CULL "Skip sprite composition intervals with no selectable priority" OFF)
option(VITA_VDP1_RETAIN_ATTACHMENTS "Avoid retiring unchanged VDP1 depth/stencil attachments" OFF)
option(VITA_VDP1_RETAIN_COLOR "Avoid reattaching unchanged VDP1 color backing" OFF)
option(VITA_VDP2_INDEXED_QUADS "Reuse duplicate normal VDP2 quad vertices" OFF)
if(VITA_VDP2_INDEXED_QUADS)
  set_property(SOURCE src/video/opengl/ygles.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_VDP2_INDEXED_QUADS)
endif()
if(VITA_VDP1_RETAIN_COLOR)
  set_property(SOURCE src/video/opengl/ygles.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_VDP1_RETAIN_COLOR)
endif()
option(VITA_VDP1_SKIP_EMPTY_FEEDBACK "Avoid destination snapshots for empty VDP1 batches" OFF)
if(VITA_VDP1_SKIP_EMPTY_FEEDBACK)
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_VDP1_SKIP_EMPTY_FEEDBACK)
endif()
option(VITA_VDP1_DEFER_FLUSH "Avoid starting an unused display scene after VDP1" OFF)
if(VITA_VDP1_DEFER_FLUSH)
  set_property(SOURCE src/video/opengl/ygles.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_VDP1_DEFER_FLUSH)
endif()
if(VITA_VDP1_RETAIN_ATTACHMENTS)
  set_property(SOURCE src/video/opengl/ygles.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_VDP1_RETAIN_ATTACHMENTS)
endif()
if(VITA_DIAG_LAYERS)
  set_property(SOURCE src/core/vdp2.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_DIAG_LAYERS)
endif()
if(VITA_FB_PRIORITY_CULL)
  set_property(SOURCE src/video/opengl/ygles.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_FB_PRIORITY_CULL)
endif()
if(VITA_NORMAL_DIRECT_UV)
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_NORMAL_DIRECT_UV)
endif()
if(VITA_DIAG_GPU_STAGES)
  set_property(SOURCE src/video/opengl/ygles.c src/vita/main.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_DIAG_GPU_STAGES)
endif()
option(VITA_ROTATION_MAP_CACHE "Experimental GPU-resident decoded rotation map" OFF)
if(VITA_ROTATION_MAP_CACHE)
  if(NOT VITA_ROTATION_UNIFORMS OR VITA_ROTATION_FLOAT_COEFFICIENTS OR VITA_ROTATION_PLANE_UNIFORMS)
    message(FATAL_ERROR "Map cache requires constant-k and excludes other uniform experiments")
  endif()
  add_compile_definitions(VITA_ROTATION_MAP_CACHE)
endif()
option(VITA_ROTATION_FLOAT_COEFFICIENTS "Experimental exact F32 rotation coefficient texture" OFF)
if(VITA_ROTATION_FLOAT_COEFFICIENTS)
  if(NOT VITA_ROTATION_UNIFORMS OR VITA_ROTATION_PLANE_UNIFORMS)
    message(FATAL_ERROR "Float coefficients require constant-k and no plane-uniform experiment")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_FLOAT_COEFFICIENTS)
endif()
option(VITA_ROTATION_PLANE_UNIFORMS "Experimental explicit float4 rotation plane addresses" OFF)
if(VITA_ROTATION_PLANE_UNIFORMS)
  if(NOT VITA_ROTATION_UNIFORMS)
    message(FATAL_ERROR "Plane uniforms require constant-k rotation uniforms")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_PLANE_UNIFORMS)
endif()
if(VITA_ROTATION_UNIFORMS)
  if(VITA_ROTATION_ROWS OR VITA_ROTATION_VRAM_REUSE)
    message(FATAL_ERROR "Constant-k uniforms cannot combine with row geometry or split VRAM delivery")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_UNIFORMS)
endif()
option(VITA_POWER_MODE_C "Request Sony power configuration Mode C (high GPU clock, WLAN kept)" OFF)
if(VITA_POWER_MODE_C)
  set_property(SOURCE src/vita/main.c APPEND PROPERTY COMPILE_DEFINITIONS VITA_POWER_MODE_C)
endif()
option(VITA_SKIP_TRANSPARENT_CELLS "Do not queue VDP2 pattern quads whose decoded texels are all alpha 0" ON)
option(VITA_DIAG_CELL_ALPHA "Diagnostics: classify normal VDP2 cell alpha coverage" OFF)
option(VITA_PALETTE_UPLOAD_REUSE "Skip exact duplicate CRAM texture uploads" OFF)
if(VITA_PALETTE_UPLOAD_REUSE)
  set_property(SOURCE src/video/opengl/ygles.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_PALETTE_UPLOAD_REUSE)
endif()
option(VITA_VDP2_OPAQUE_RUNS "Experimental order-preserving no-discard VDP2 cell runs" OFF)
option(VITA_VDP2_OPAQUE_GROUPS "Experimental two-pass batching for proven disjoint opaque cells" OFF)
if(VITA_VDP2_OPAQUE_GROUPS)
  if(NOT VITA_VDP2_OPAQUE_RUNS)
    message(FATAL_ERROR "Opaque grouping requires opaque runs")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_VDP2_OPAQUE_GROUPS)
endif()
if(VITA_VDP2_OPAQUE_RUNS)
  if(VITA_NORMAL_DIRECT_UV OR VITA_PALETTE_FLOAT_ADDRESS)
    message(FATAL_ERROR "Opaque runs require the reference normal shader addressing")
  endif()
  set_property(SOURCE src/video/opengl/ygles.c src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_VDP2_OPAQUE_RUNS)
endif()
if(VITA_DIAG_CELL_ALPHA)
  if(NOT VITA_SKIP_TRANSPARENT_CELLS)
    message(FATAL_ERROR "Cell alpha census requires decoded-cell tracking")
  endif()
  set_property(SOURCE src/video/opengl/ygles.c src/video/opengl/vidogl.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_DIAG_CELL_ALPHA)
endif()
if(VITA_SKIP_TRANSPARENT_CELLS)
  set_property(SOURCE src/video/opengl/ygles.c src/video/opengl/vidogl.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_SKIP_TRANSPARENT_CELLS)
endif()
option(VITA_ROTATION_TARGET "Render GPU rotation jobs into exact-size textures instead of the atlas" ON)
if(VITA_ROTATION_TARGET)
  set_property(SOURCE src/video/opengl/ygles.c src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_TARGET)
endif()
option(VITA_DIAG_TIMERS "Diagnostics: wall-clock a few coarse emulator functions per telemetry window" OFF)
if(VITA_DIAG_TIMERS)
  add_compile_definitions(VITA_DIAG_TIMERS)
  target_link_libraries(yabause-vita PRIVATE SceKernelDmacMgr_stub)
endif()
option(VITA_STACK_PROFILE "Diagnostics: clockless phase-stack sampling profiler (never benchmark)" OFF)
option(VITA_STACK_PROFILE_RENDER "Diagnostics: the stack profiler's second thread is the render thread, not the sound worker" OFF)
if(VITA_STACK_PROFILE)
  add_compile_definitions(VITA_STACK_PROFILE)
  if(VITA_STACK_PROFILE_RENDER)
    add_compile_definitions(VITA_STACK_PROFILE_RENDER)
  endif()
endif()
option(VITA_ROTATION_REUSE_PROBE "Measure rotation VRAM snapshot reuse (diagnostics only)" OFF)
if(VITA_ROTATION_REUSE_PROBE)
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_REUSE_PROBE)
endif()
option(VITA_ROTATION_VRAM_REUSE "Upload rotation VRAM only when its bytes change" OFF)
if(VITA_ROTATION_OUTPUT_REUSE)
  if(NOT VITA_ROTATION_TARGET OR NOT VITA_ROTATION_VARIANTS OR
      VITA_ROTATION_ROWS OR VITA_ROTATION_VRAM_REUSE)
    message(FATAL_ERROR "Output reuse requires private targets, variants and complete immutable snapshots")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_OUTPUT_REUSE)
endif()
if(VITA_ROTATION_VRAM_REUSE)
  if(VITA_ROTATION_ROWS OR VITA_ROTATION_VARIANTS OR VITA_ROTATION_REUSE_PROBE)
    message(FATAL_ERROR "VITA_ROTATION_VRAM_REUSE uses only the generic split shader")
  endif()
  # Global: RBGDrawInfo layout and the VRAM writers must agree in every TU.
  add_compile_definitions(VITA_ROTATION_VRAM_REUSE)
endif()
option(VITA_ROTATION_VRAM_FORCE_COPY "Diagnostics: split rotation textures but copy/upload VRAM every job" OFF)
if(VITA_ROTATION_VRAM_FORCE_COPY)
  if(NOT VITA_ROTATION_VRAM_REUSE)
    message(FATAL_ERROR "FORCE_COPY only isolates the split-texture cost of VITA_ROTATION_VRAM_REUSE")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_VRAM_FORCE_COPY)
endif()
if(VITA_ATLAS_REPLACE)
  set_property(SOURCE src/video/opengl/ygles.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ATLAS_REPLACE)
endif()
if(VITA_ROTATION_ROWS AND VITA_ROTATION_VARIANTS)
  message(FATAL_ERROR "Test rotation row delivery separately from shader specialization")
endif()
if(VITA_ROTATION_ROWS)
  message(WARNING "Row geometry fails the live-input pixel comparison; do not use for normal emulation")
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_ROWS)
endif()
if(VITA_ROTATION_VARIANTS)
  if(VITA_ROTATION_MAP_CACHE OR VITA_ROTATION_FLOAT_COEFFICIENTS OR VITA_ROTATION_PLANE_UNIFORMS)
    message(FATAL_ERROR "Test format specializations separately from other rotation experiments")
  endif()
  set_property(SOURCE src/video/opengl/yglshaderes.c APPEND PROPERTY
    COMPILE_DEFINITIONS VITA_ROTATION_VARIANTS)
endif()
if(YABAUSE_VITAGL OR YABAUSE_GXM_PROBE OR YABAUSE_GXM_COMPOSITOR)
  target_sources(yabause-vita PRIVATE src/vita/telemetry_gxm.c)
  foreach(symbol IN ITEMS sceGxmFinish sceGxmNotificationWait sceGxmDisplayQueueFinish)
    target_link_options(yabause-vita PRIVATE "-Wl,--wrap=${symbol}")
  endforeach()
endif()
target_include_directories(yabause-vita PRIVATE src/core
  "${CMAKE_CURRENT_BINARY_DIR}/c68k" third_party/libchdr/include
  third_party/libchdr/include/libchdr third_party/libchdr/deps/lzma-19.00/include)
set_property(SOURCE src/core/sh2_dynarec/dynalib_arm.s APPEND PROPERTY
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/core/sh2_dynarec/div1_a9.inc")
target_link_options(yabause-vita PRIVATE -Wl,--gc-sections)
# libgcc's emutls uses weak pthread references. In a static link, merely
# linking libpthread does not extract pthread_cancel, its threading probe.
# Keep every emutls dependency resolvable; otherwise _Thread_local is global.
foreach(symbol IN ITEMS pthread_cancel pthread_getspecific pthread_key_create
    pthread_mutex_init pthread_mutex_lock pthread_mutex_unlock pthread_once
    pthread_setspecific)
  target_link_options(yabause-vita PRIVATE "-Wl,-u,${symbol}")
endforeach()
if(VITA_STACK_CHECKS)
  target_link_options(yabause-vita PRIVATE -Wl,--wrap=__stack_chk_fail)
endif()
# Drain the asynchronous run log before any abort (src/vita/main.c).
target_link_options(yabause-vita PRIVATE -Wl,--wrap=abort)
target_link_libraries(yabause-vita PRIVATE FLAC ogg z m pthread
  SceDisplay_stub SceCtrl_stub SceAudio_stub ScePower_stub SceAppUtil_stub
  SceCommonDialog_stub SceAppMgr_stub)
if(VITA_PGO STREQUAL "generate")
  target_sources(yabause-vita PRIVATE src/vita/pgo_dump.c)
  set_property(SOURCE src/vita/pgo_dump.c APPEND PROPERTY COMPILE_OPTIONS -fno-profile-arcs)
endif()
vita_create_self(eboot.bin yabause-vita UNSAFE)
vita_create_vpk(yabause-vita.vpk YABA00001 eboot.bin
  VERSION 00.10 NAME "Yabause Vita")
