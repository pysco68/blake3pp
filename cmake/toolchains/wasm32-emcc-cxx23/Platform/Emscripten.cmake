# Platform module shim, found ahead of emscripten's own because the
# toolchain puts this directory first on CMAKE_MODULE_PATH. CMake reloads
# Platform/Emscripten.cmake during project(), after the toolchain file, so
# a compiler override in the toolchain alone does not survive: it has to
# happen here, after the real module ran.
#
# The override: the drivers under clang-shaped names. reclient's
# dependency scanner classifies a compiler by its basename and aborts on
# emcc/em++, taking its scanner service down with it, so nothing can be
# distributed. The names exist only in the toolchain image
# (docker/emscripten.Dockerfile); elsewhere the stock drivers stay.
include(/usr/share/emscripten/cmake/Modules/Platform/Emscripten.cmake)
if(EXISTS /usr/local/bin/wasm32-emscripten-clang++)
  set(CMAKE_C_COMPILER /usr/local/bin/wasm32-emscripten-clang)
  set(CMAKE_CXX_COMPILER /usr/local/bin/wasm32-emscripten-clang++)
endif()
# The link launcher declares the .wasm sidecar to reclient (see the
# script); it goes in front of whatever launcher cmake-re injected.
# (CMake loads this module more than once per configure, hence the guard.)
if(EXISTS /usr/local/bin/emscripten-link-launcher)
  foreach(lang C CXX)
    if(NOT "/usr/local/bin/emscripten-link-launcher" IN_LIST CMAKE_${lang}_LINKER_LAUNCHER)
      list(PREPEND CMAKE_${lang}_LINKER_LAUNCHER /usr/local/bin/emscripten-link-launcher)
    endif()
  endforeach()
endif()
