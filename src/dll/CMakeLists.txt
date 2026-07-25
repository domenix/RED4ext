add_library(RED4ext.Dll SHARED)

set_target_properties(
  RED4ext.Dll
    PROPERTIES
      OUTPUT_NAME ${PROJECT_NAME}
)

if(WIN32)
red4ext_configure_resource_rc(
  RED4ext.Dll
  "A script extender for REDengine 4."
)
endif()

red4ext_configure_version_file(RED4ext.Dll)

file(
  GLOB_RECURSE
    RED4EXT_FILES
      *.hpp
      *.cpp
)

# Each platform brings its own implementation of Platform.hpp; compiling both would give two
# definitions of everything in the Platform namespace.
if(WIN32)
  list(FILTER RED4EXT_FILES EXCLUDE REGEX "/Platform/Mac[^/]*\\.cpp$")
  list(FILTER RED4EXT_FILES EXCLUDE REGEX "/Platform/HookEngineArm64\\.cpp$")
else()
  list(FILTER RED4EXT_FILES EXCLUDE REGEX "/Platform/Win[^/]*\\.cpp$")
  list(FILTER RED4EXT_FILES EXCLUDE REGEX "/Platform/HookEngineDetours\\.cpp$")
endif()

source_group(CMake REGULAR_EXPRESSION cmake_pch.*)

source_group(
  TREE
    "${CMAKE_CURRENT_SOURCE_DIR}"
  FILES
    ${RED4EXT_FILES}
)

source_group(
  TREE
    "${CMAKE_CURRENT_BINARY_DIR}"
  FILES
    ${RED4EXT_RC_FILE}
    ${RED4EXT_VERSION_FILE}
)

target_include_directories(
  RED4ext.Dll
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_precompile_headers(
  RED4ext.Dll
    PRIVATE
      stdafx.hpp
)

target_sources(
  RED4ext.Dll
    PRIVATE
      ${RED4EXT_FILES}
      ${RED4EXT_RC_FILE}
      ${RED4EXT_VERSION_FILE}
)

target_link_libraries(
  RED4ext.Dll
    PRIVATE
      fmt::fmt
      RED4ext::SDK
      redscript
      simdjson::simdjson
      spdlog::spdlog
      toml11::toml11
)

if(WIN32)
  target_link_libraries(
    RED4ext.Dll
      PRIVATE
        ntdll
        version

        Detours
        WIL::WIL
  )
elseif(APPLE)
  # CoreFoundation supplies the alert box that replaces MessageBox.
  target_link_libraries(
    RED4ext.Dll
      PRIVATE
        "-framework CoreFoundation"
  )

  set_target_properties(
    RED4ext.Dll
      PROPERTIES
        SUFFIX ".dylib"
        PREFIX ""
  )
endif()
