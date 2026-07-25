option(RED4EXT_INSTALL "" OFF)
option(RED4EXT_USE_PCH "" ON)

# Point this at a checkout to build against a local SDK instead of fetching one. The macOS
# build needs it: upstream tag 1.0.0 does not compile with Clang.
set(RED4EXT_SDK_SOURCE_DIR "" CACHE PATH "Local RED4ext.SDK checkout to build against.")

if(RED4EXT_SDK_SOURCE_DIR)
  if(NOT EXISTS "${RED4EXT_SDK_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "RED4EXT_SDK_SOURCE_DIR is not an SDK checkout: ${RED4EXT_SDK_SOURCE_DIR}")
  endif()

  message(STATUS "Using local RED4ext.SDK: ${RED4EXT_SDK_SOURCE_DIR}")
  add_subdirectory("${RED4EXT_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/red4ext.sdk-build")
else()
  FetchContent_Declare(
    RED4ext.SDK
    GIT_REPOSITORY  https://github.com/wopss/RED4ext.SDK.git
    GIT_TAG         1.0.0
  )
  FetchContent_MakeAvailable(RED4ext.SDK)
endif()

set_target_properties(
  RED4ext.SDK
    PROPERTIES
      FOLDER "Dependencies"
)

mark_as_advanced(
  RED4EXT_BUILD_EXAMPLES
  RED4EXT_HEADER_ONLY
  RED4EXT_INSTALL
  RED4EXT_USE_PCH
)
