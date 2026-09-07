# cmake/toolchains/common.cmake
#
# Shared engine for every toolchain file in this directory. A concrete file sets
# a handful of TC_* variables and then includes this one:
#
#     set(TC_CXX_COMPILER  clang++-22)
#     set(TC_STDLIB        libc++)
#     set(TC_CXX_STANDARD  26)
#     set(TC_SANITIZERS    address undefined)
#     include("${CMAKE_CURRENT_LIST_DIR}/common.cmake")
#
# Everything is set inside the toolchain file itself rather than passed on the
# command line. That is deliberate:
#
#   * a consumer only has to pass -DCMAKE_TOOLCHAIN_FILE=<path>, nothing else;
#   * CMake re-reads the toolchain file inside try_compile sub-builds, and -D
#     variables from your command line are NOT visible there unless you list
#     them in CMAKE_TRY_COMPILE_PLATFORM_VARIABLES. Self-contained files avoid
#     that trap entirely, so the compiler-ABI check sees the same flags the
#     real build will use.
#
# ---------------------------------------------------------------------------
# Recognised inputs (all optional unless noted)
#
#   TC_C_COMPILER          e.g. gcc-16, clang-22            (required)
#   TC_CXX_COMPILER        e.g. g++-16, clang++-22          (required)
#   TC_STDLIB              libstdc++ | libc++   (Clang only)
#   TC_GCC_INSTALL_DIR     pin Clang to one GCC's libstdc++, e.g.
#                          /usr/lib/gcc/x86_64-linux-gnu/12
#   TC_CXX_STANDARD        20 | 23 | 26
#   TC_CXX_STANDARD_RAW    a literal -std= value for standards CMake does not
#                          know yet, e.g. c++2d. Mutually exclusive with above.
#   TC_LINKER              mold | lld | lld-22 | bfd | <empty for default>
#   TC_SANITIZERS          list: address undefined thread memory fuzzer
#   TC_COVERAGE            llvm | gcov | <empty>
#   TC_MSAN_LIBCXX_PREFIX  default /opt/libcxx-msan
#   TC_LAUNCHER            compiler launcher program; default "" (none)
#   TC_EXTRA_CXX_FLAGS     escape hatch, appended last
#   TC_EXTRA_LINK_FLAGS    escape hatch, appended last
# ---------------------------------------------------------------------------

if(NOT DEFINED TC_CXX_COMPILER)
  message(FATAL_ERROR "toolchain: TC_CXX_COMPILER must be set before including common.cmake")
endif()

set(CMAKE_C_COMPILER   "${TC_C_COMPILER}")
set(CMAKE_CXX_COMPILER "${TC_CXX_COMPILER}")

if(TC_CXX_COMPILER MATCHES "clang")
  set(_tc_is_clang TRUE)
else()
  set(_tc_is_clang FALSE)
endif()

set(_cxx_flags "")
set(_c_flags "")
set(_link_flags "")

# --------------------------------------------------------------- C++ standard
# Setting CMAKE_CXX_STANDARD here gives a project-wide default. A project that
# sets CMAKE_CXX_STANDARD itself, or calls target_compile_features(cxx_std_NN),
# still wins; that is normal CMake precedence, not a bug in this file.
if(DEFINED TC_CXX_STANDARD_RAW)
  if(DEFINED TC_CXX_STANDARD)
    message(FATAL_ERROR "toolchain: set TC_CXX_STANDARD or TC_CXX_STANDARD_RAW, not both")
  endif()
  # CMake appends its own -std= AFTER CMAKE_CXX_FLAGS, so a raw flag only sticks
  # while CMAKE_CXX_STANDARD stays unset and no target requests a cxx_std_NN
  # feature. Watch for that if a dependency asks for one.
  list(APPEND _cxx_flags "-std=${TC_CXX_STANDARD_RAW}")
elseif(DEFINED TC_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD ${TC_CXX_STANDARD})
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# ------------------------------------------------------------ standard library
if(_tc_is_clang AND DEFINED TC_STDLIB)
  list(APPEND _cxx_flags  "-stdlib=${TC_STDLIB}")
  list(APPEND _link_flags "-stdlib=${TC_STDLIB}")
endif()

if(_tc_is_clang AND DEFINED TC_GCC_INSTALL_DIR)
  # Without this, Clang picks the NEWEST libstdc++ on the system. In an image
  # carrying several GCCs that means an old Clang tries to parse headers from a
  # much newer libstdc++ and dies on built-ins it does not implement.
  if(NOT IS_DIRECTORY "${TC_GCC_INSTALL_DIR}")
    message(WARNING "toolchain: TC_GCC_INSTALL_DIR ${TC_GCC_INSTALL_DIR} does not exist")
  endif()
  list(APPEND _cxx_flags  "--gcc-install-dir=${TC_GCC_INSTALL_DIR}")
  list(APPEND _link_flags "--gcc-install-dir=${TC_GCC_INSTALL_DIR}")
endif()

# ------------------------------------------------------------------- linker
if(DEFINED TC_LINKER AND NOT TC_LINKER STREQUAL "")
  list(APPEND _link_flags "-fuse-ld=${TC_LINKER}")
endif()

# --------------------------------------------------------------- sanitizers
if(DEFINED TC_SANITIZERS AND NOT TC_SANITIZERS STREQUAL "")
  if("thread" IN_LIST TC_SANITIZERS AND "address" IN_LIST TC_SANITIZERS)
    message(FATAL_ERROR "toolchain: TSan and ASan cannot be combined")
  endif()
  if("memory" IN_LIST TC_SANITIZERS AND "address" IN_LIST TC_SANITIZERS)
    message(FATAL_ERROR "toolchain: MSan and ASan cannot be combined")
  endif()
  if("memory" IN_LIST TC_SANITIZERS AND NOT _tc_is_clang)
    message(FATAL_ERROR "toolchain: MemorySanitizer is Clang-only")
  endif()
  if("fuzzer" IN_LIST TC_SANITIZERS AND NOT _tc_is_clang)
    message(FATAL_ERROR "toolchain: libFuzzer requires Clang")
  endif()

  # libFuzzer's runtime is whole-archive-linked and carries its own main(),
  # so a global -fsanitize=fuzzer breaks every ordinary executable (starting
  # with CMake's own compiler check). Instrument globally with
  # fuzzer-no-link; dedicated fuzz-harness targets add -fsanitize=fuzzer on
  # their own link line.
  set(_san_for_flags "${TC_SANITIZERS}")
  list(TRANSFORM _san_for_flags REPLACE "^fuzzer$" "fuzzer-no-link")
  string(REPLACE ";" "," _san_csv "${_san_for_flags}")
  list(APPEND _cxx_flags
    "-fsanitize=${_san_csv}"
    "-fno-omit-frame-pointer"
    "-fno-optimize-sibling-calls"
    "-g")
  list(APPEND _c_flags "-fsanitize=${_san_csv}" "-fno-omit-frame-pointer" "-g")
  # Must also go on the link line, or CMake's own compiler check fails to link
  # before your project ever gets a chance to configure.
  list(APPEND _link_flags "-fsanitize=${_san_csv}")

  if("undefined" IN_LIST TC_SANITIZERS)
    list(APPEND _cxx_flags "-fno-sanitize-recover=undefined")
  endif()

  if("memory" IN_LIST TC_SANITIZERS)
    if(NOT DEFINED TC_MSAN_LIBCXX_PREFIX)
      set(TC_MSAN_LIBCXX_PREFIX "/opt/libcxx-msan")
    endif()
    if(NOT IS_DIRECTORY "${TC_MSAN_LIBCXX_PREFIX}/include/c++/v1")
      message(WARNING
        "toolchain: MSan build but no instrumented libc++ at ${TC_MSAN_LIBCXX_PREFIX}. "
        "Expect false positives. Run this preset via tools/tc (the clang22 "
        "toolchain image bakes /opt/libcxx-msan in), or run "
        "tools/build-msan-libcxx.sh locally.")
    endif()
    list(APPEND _cxx_flags
      "-fsanitize-memory-track-origins=2"
      "-nostdinc++"
      "-isystem ${TC_MSAN_LIBCXX_PREFIX}/include/c++/v1")
    list(APPEND _link_flags
      "-L${TC_MSAN_LIBCXX_PREFIX}/lib"
      "-Wl,-rpath,${TC_MSAN_LIBCXX_PREFIX}/lib")
  endif()
endif()

# ----------------------------------------------------------------- coverage
if(DEFINED TC_COVERAGE AND NOT TC_COVERAGE STREQUAL "")
  if(TC_COVERAGE STREQUAL "llvm")
    list(APPEND _cxx_flags  "-fprofile-instr-generate" "-fcoverage-mapping")
    list(APPEND _c_flags    "-fprofile-instr-generate" "-fcoverage-mapping")
    list(APPEND _link_flags "-fprofile-instr-generate")
  elseif(TC_COVERAGE STREQUAL "gcov")
    list(APPEND _cxx_flags  "--coverage")
    list(APPEND _c_flags    "--coverage")
    list(APPEND _link_flags "--coverage")
  else()
    message(FATAL_ERROR "toolchain: TC_COVERAGE must be llvm, gcov or empty")
  endif()
endif()

# ------------------------------------------------------------------ extras
if(DEFINED TC_EXTRA_CXX_FLAGS)
  list(APPEND _cxx_flags ${TC_EXTRA_CXX_FLAGS})
endif()
if(DEFINED TC_EXTRA_LINK_FLAGS)
  list(APPEND _link_flags ${TC_EXTRA_LINK_FLAGS})
endif()

# ------------------------------------------------------------------- emit
# The _INIT variants are the documented way for a toolchain file to seed flags:
# CMake folds them into the CMAKE_*_FLAGS cache entries on first configure, so
# a user can still add their own flags afterwards without them being clobbered
# on every re-run. Assigning CMAKE_CXX_FLAGS directly here would fight that.
list(JOIN _cxx_flags  " " _cxx_flags_str)
list(JOIN _c_flags    " " _c_flags_str)
list(JOIN _link_flags " " _link_flags_str)

set(CMAKE_C_FLAGS_INIT   "${_c_flags_str}")
set(CMAKE_CXX_FLAGS_INIT "${_cxx_flags_str}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_link_flags_str}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link_flags_str}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_link_flags_str}")

# No launcher by default: local compiler caching was removed deliberately
# (remote execution via cmake-re is planned and would conflict). The knob
# stays for ad-hoc use: -DTC_LAUNCHER=<program>.
if(NOT DEFINED TC_LAUNCHER)
  set(TC_LAUNCHER "")
endif()
if(NOT TC_LAUNCHER STREQUAL "")
  find_program(_tc_launcher_bin "${TC_LAUNCHER}")
  if(_tc_launcher_bin)
    set(CMAKE_C_COMPILER_LAUNCHER   "${_tc_launcher_bin}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_tc_launcher_bin}")
  endif()
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ------------------------------------------------------- default build type
# Normally a build type is a per-invocation concern, not a toolchain one. But
# when presets are unavailable the toolchain file is the only thing a consumer
# passes, and a sanitizer build without a build type would land on no
# optimisation at all, so instrumented toolchains carry a default: the same
# RelWithDebInfo as the plain presets (sanitizers want -O1 or better; see
# INSTRUMENTATIONS in tools/gen-toolchains.py), Debug for coverage. An
# explicit -DCMAKE_BUILD_TYPE always wins: cache entries from the command
# line are already populated by the time this file is read.
if(DEFINED TC_DEFAULT_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "${TC_DEFAULT_BUILD_TYPE}" CACHE STRING "Build type" FORCE)
endif()
