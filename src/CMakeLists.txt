add_subdirectory(dll)

# The loader is a winmm.dll proxy: it exists purely so Windows' DLL search order pulls RED4ext
# into the game. macOS uses DYLD_INSERT_LIBRARIES, which needs no proxy. The playground is a
# Windows console harness.
if(WIN32)
  add_subdirectory(loader)
  add_subdirectory(playground)
endif()
