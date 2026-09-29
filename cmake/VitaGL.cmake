# Private audited dependencies; do not accidentally pick an unrelated SDK copy.
set(YABAUSE_VITAGL_ROOT "" CACHE PATH "Private output from tools/build_vitagl.py")
option(YABAUSE_VITAGL "Experimental integrated accelerated renderer" OFF)
if(YABAUSE_VITAGL AND (YABAUSE_GXM_PROBE OR YABAUSE_GXM_COMPOSITOR))
  message(FATAL_ERROR "vitaGL must be the sole GXM/display owner; disable the GXM prototype")
endif()
if(YABAUSE_VITAGL AND NOT YABAUSE_VITAGL_ROOT)
  message(FATAL_ERROR "Build the private vitaGL dependencies and set YABAUSE_VITAGL_ROOT")
endif()
if(NOT YABAUSE_VITAGL_ROOT)
  return()
endif()
if(NOT TARGET yabause-vitagl-shaders)
  message(FATAL_ERROR "The vitaGL backend requires YABAUSE_PSP2CGC or YABAUSE_RUNTIME_SHADERS")
endif()
file(READ "${YABAUSE_VITAGL_ROOT}/build.json" vitagl_manifest)
file(SHA256 "${PROJECT_SOURCE_DIR}/tools/patches/vitagl-stencil-retirement.patch"
  expected_stencil_patch)
string(JSON actual_stencil_patch ERROR_VARIABLE stencil_patch_error
  GET "${vitagl_manifest}" patches "vitagl-stencil-retirement.patch")
if(stencil_patch_error OR NOT actual_stencil_patch STREQUAL expected_stencil_patch)
  message(FATAL_ERROR "Rebuild private vitaGL: required stencil-retirement patch is missing or stale")
endif()
string(JSON vitagl_revision GET "${vitagl_manifest}" revision)
string(JSON shark_revision GET "${vitagl_manifest}" shark_revision)
string(JSON vitagl_abi GET "${vitagl_manifest}" float_abi)
if(NOT vitagl_revision STREQUAL "16fe309d87761112b813be77842b20338659c460"
    OR NOT shark_revision STREQUAL "df24065e65098b2d1ac533760109ad4367573f28"
    OR NOT vitagl_abi STREQUAL "hard")
  message(FATAL_ERROR "Private graphics dependency revision/ABI does not match the renderer")
endif()
foreach(option IN ITEMS STORE_DEPTH_STENCIL HAVE_GLSL_TEXTURE_SIZE HAVE_GLSL_UBOS NO_DEBUG)
  string(JSON actual GET "${vitagl_manifest}" options "${option}")
  if(option STREQUAL "STORE_DEPTH_STENCIL")
    set(expected 1)
  else()
    set(expected 0)
  endif()
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Incompatible vitaGL option ${option}=${actual}")
  endif()
endforeach()
foreach(dependency IN ITEMS vitaGL vitashark)
  if(NOT EXISTS "${YABAUSE_VITAGL_ROOT}/lib/lib${dependency}.a"
      OR NOT EXISTS "${YABAUSE_VITAGL_ROOT}/include/${dependency}.h")
    message(FATAL_ERROR "Incomplete private graphics dependency: ${dependency}")
  endif()
  add_library(yabause-${dependency} STATIC IMPORTED)
  set_target_properties(yabause-${dependency} PROPERTIES
    IMPORTED_LOCATION "${YABAUSE_VITAGL_ROOT}/lib/lib${dependency}.a"
    INTERFACE_INCLUDE_DIRECTORIES "${YABAUSE_VITAGL_ROOT}/include")
endforeach()

target_link_libraries(yabause-vitaGL INTERFACE yabause-vitashark mathneon
  SceGxm_stub SceDisplay_stub SceAppMgr_stub SceSysmodule_stub SceCommonDialog_stub
  SceKernelDmacMgr_stub)
target_link_libraries(yabause-vitashark INTERFACE SceShaccCgExt taihen_stub
  SceShaccCg_stub)

# Real ARM renderer target; selected only by the experimental app option.
add_library(yabause-vitagl-renderer STATIC EXCLUDE_FROM_ALL
  ${PROJECT_SOURCE_DIR}/src/video/opengl/vidogl.c
  ${PROJECT_SOURCE_DIR}/src/video/opengl/ygles.c
  ${PROJECT_SOURCE_DIR}/src/video/opengl/yglshaderes.c
  ${PROJECT_SOURCE_DIR}/src/video/opengl/yglcache.c
  ${PROJECT_SOURCE_DIR}/src/vita/rotation_compute_guard.c)
add_dependencies(yabause-vitagl-renderer yabause-vitagl-shaders)
target_compile_definitions(yabause-vitagl-renderer PRIVATE YABAUSE_VITAGL HAVE_LIBGL)
if(YABAUSE_RUNTIME_SHADERS)
  if(NOT YABAUSE_VITAGL)
    message(FATAL_ERROR "Runtime Cg compilation requires YABAUSE_VITAGL")
  endif()
  target_compile_definitions(yabause-vitagl-renderer PRIVATE YABAUSE_RUNTIME_SHADERS)
  set_property(SOURCE src/vita/main.c APPEND PROPERTY COMPILE_DEFINITIONS YABAUSE_RUNTIME_SHADERS)
  option(YABAUSE_SHADER_CACHE "Reuse runtime-compiled shaders across launches" ON)
  if(YABAUSE_SHADER_CACHE)
    string(JSON shader_cache ERROR_VARIABLE shader_cache_error
      GET "${vitagl_manifest}" options HAVE_SHADER_CACHE)
    file(SHA256 "${PROJECT_SOURCE_DIR}/tools/patches/vitagl-shader-cache.patch"
      expected_cache_patch)
    string(JSON actual_cache_patch ERROR_VARIABLE cache_patch_error
      GET "${vitagl_manifest}" patches "vitagl-shader-cache.patch")
    if(shader_cache_error OR NOT shader_cache STREQUAL "1" OR cache_patch_error
        OR NOT actual_cache_patch STREQUAL expected_cache_patch)
      message(FATAL_ERROR "Rebuild private vitaGL with --shader-cache and the current patches")
    endif()
    set_property(SOURCE src/vita/main.c APPEND PROPERTY COMPILE_DEFINITIONS YABAUSE_SHADER_CACHE)
  endif()
endif()
target_compile_options(yabause-vitagl-renderer PRIVATE -Werror=implicit-function-declaration)
target_include_directories(yabause-vitagl-renderer PRIVATE
  "${PROJECT_SOURCE_DIR}/src/core" "${PROJECT_BINARY_DIR}/vitagl")
target_link_libraries(yabause-vitagl-renderer PUBLIC yabause-vitaGL yabause-vitashark)
if(YABAUSE_VITAGL)
  target_compile_definitions(yabause-vita PRIVATE YABAUSE_VITAGL)
  # HAVE_LIBGL on VIDSoft would select its unrelated desktop presentation path.
  set_property(SOURCE src/vita/main.c APPEND PROPERTY COMPILE_DEFINITIONS HAVE_LIBGL)
  target_link_libraries(yabause-vita PRIVATE yabause-vitagl-renderer)
endif()
