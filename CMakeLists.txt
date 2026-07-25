cmake_minimum_required(VERSION 3.26)

# ------------------------------------------------------------------------------
# Project variables
# ------------------------------------------------------------------------------
set(RED4EXT_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(RED4EXT_CMAKE_DEPS_DIR "${RED4EXT_CMAKE_DIR}/deps")
set(RED4EXT_CMAKE_TEMPLATES_DIR "${RED4EXT_CMAKE_DIR}/templates")

# ------------------------------------------------------------------------------
# Modules
# ------------------------------------------------------------------------------
include("${RED4EXT_CMAKE_DIR}/ConfigureDefaultOutputDirectories.cmake")
include("${RED4EXT_CMAKE_DIR}/ConfigureResourceRc.cmake")
include("${RED4EXT_CMAKE_DIR}/ConfigureVersionFile.cmake")
include("${RED4EXT_CMAKE_DIR}/ConfigureVersionFromGit.cmake")
include("${RED4EXT_CMAKE_DIR}/TargetAppendOutputDirectory.cmake")

# ------------------------------------------------------------------------------
# Versioning
# ------------------------------------------------------------------------------
red4ext_configure_version_from_git()

# ------------------------------------------------------------------------------
# Project definition
# ------------------------------------------------------------------------------
project(
  RED4ext
  VERSION "${RED4EXT_GIT_VERSION_MAJOR}.${RED4EXT_GIT_VERSION_MINOR}.${RED4EXT_GIT_VERSION_PATCH}"
  LANGUAGES CXX
)

# ------------------------------------------------------------------------------
# General configuration
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")

red4ext_configure_default_output_directories()

# ------------------------------------------------------------------------------
# Options
# ------------------------------------------------------------------------------
option(RED4EXT_GIT_METADATA "Append Git metadata to 'RED4EXT_GIT_VERSION' CMake variable." ON)

option(RED4EXT_EXTRA_WARNINGS "Enable extra warnings." ON)
option(RED4EXT_TREAT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors." OFF)

# ------------------------------------------------------------------------------
# Toolchain settings for all targets
# ------------------------------------------------------------------------------
if(WIN32)
add_compile_definitions(
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN

  # Support Windows 7 and above.
  WINVER=0x0601
  _WIN32_WINNT=0x0601
)
endif()

if(MSVC)
add_link_options(
  /DEBUG:FULL

  $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:/INCREMENTAL:NO>
)
endif()

if(APPLE)
  # The runtime is injected with DYLD_INSERT_LIBRARIES and hooks arm64 code in place.
  # Both of those are architecture-specific; there is no x86_64 path.
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
  endif()
endif()

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
include(FetchContent)

# Detours is x86/x64-only and WIL is a Windows library. Everything else builds anywhere.
# The macOS build supplies its own hooking engine (src/dll/Platform/HookEngineArm64.cpp) and
# does not use WIL at all.
if(WIN32)
  include("${RED4EXT_CMAKE_DEPS_DIR}/Detours.cmake")
  include("${RED4EXT_CMAKE_DEPS_DIR}/Wil.cmake")
endif()

include("${RED4EXT_CMAKE_DEPS_DIR}/Fmt.cmake")
include("${RED4EXT_CMAKE_DEPS_DIR}/RED4extSdk.cmake")
include("${RED4EXT_CMAKE_DEPS_DIR}/Redscript.cmake")
include("${RED4EXT_CMAKE_DEPS_DIR}/Simdjson.cmake")
include("${RED4EXT_CMAKE_DEPS_DIR}/Spdlog.cmake")
include("${RED4EXT_CMAKE_DEPS_DIR}/Toml11.cmake")

# ------------------------------------------------------------------------------
# Toolchain settings for this project only
# ------------------------------------------------------------------------------
if(WIN32)
add_compile_definitions(
  NOMINMAX
)
endif()

if(MSVC)
add_compile_options(
  /GS
  /sdl

  /permissive-
  /Zc:__cplusplus
  /Zc:__STDC__
  /Zc:enumTypes
  /Zc:inline
  /Zc:preprocessor
  /Zc:templateScope

  $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:/Gy>
  $<$<CONFIG:Release,MinSizeRel,RelWithDebInfo>:/Oi>

  # Disable some warnings that are not relevant to us.
  /external:I${FETCHCONTENT_BASE_DIR}
  /external:W0
)

if(RED4EXT_EXTRA_WARNINGS)
  add_compile_options(/W4)
endif()

if(RED4EXT_TREAT_WARNINGS_AS_ERRORS)
  add_compile_options(/WX)
endif()
else()
  # The SDK needs these three even on Clang; see RED4ext.SDK's own build.
  add_compile_options(
    -fdeclspec
    -fms-extensions
    -Wno-unknown-pragmas
  )

  if(RED4EXT_EXTRA_WARNINGS)
    add_compile_options(-Wall -Wextra)
  endif()

  if(RED4EXT_TREAT_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

# ------------------------------------------------------------------------------
# Main projects
# ------------------------------------------------------------------------------
add_subdirectory(src)

# ------------------------------------------------------------------------------
# Install
# ------------------------------------------------------------------------------
if(NOT CMAKE_SKIP_INSTALL_RULES)
  set(RED4EXT_INSTALL_X64_DIR bin/x64)
  set(RED4EXT_INSTALL_BIN_DIR red4ext)

  if(WIN32)
  install(
    TARGETS RED4ext.Loader
    RUNTIME DESTINATION "${RED4EXT_INSTALL_X64_DIR}"
  )

  install(
    FILES $<TARGET_PDB_FILE:RED4ext.Loader>
    DESTINATION "${RED4EXT_INSTALL_X64_DIR}"
  )
  endif()

  install(
    TARGETS RED4ext.Dll
    RUNTIME DESTINATION "${RED4EXT_INSTALL_BIN_DIR}"
    LIBRARY DESTINATION "${RED4EXT_INSTALL_BIN_DIR}"
  )

  if(MSVC)
  install(
    FILES $<TARGET_PDB_FILE:RED4ext.Dll>
    DESTINATION "${RED4EXT_INSTALL_BIN_DIR}"
  )
  endif()

  install(
    FILES "${PROJECT_SOURCE_DIR}/LICENSE.md"
    DESTINATION "${RED4EXT_INSTALL_BIN_DIR}"
    RENAME LICENSE.txt
  )

  install(
    FILES "${PROJECT_SOURCE_DIR}/THIRD_PARTY_LICENSES.md"
    DESTINATION "${RED4EXT_INSTALL_BIN_DIR}"
    RENAME THIRD_PARTY_LICENSES.txt
  )
endif()
