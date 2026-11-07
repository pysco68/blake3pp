# Fault tests for blake3ppsum and blake3ppgen in a pipeline, run in CMake
# script mode. execute_process with several COMMANDs connects them with
# the platform's own anonymous pipe and reports every process's exit code,
# so each case checks what a pipefail shell would see without a shell.
#
#   cmake -DSUM= -DGEN= -DPUMP= -DDRAIN= -DRUX= [-DSHIM=] -DWORK= -DCASE=
#         -P faults.cmake
#
# The faults come from the bench instruments (bench/pipe/):
#   * pump writes an exact number of bytes and then closes, dies or
#     stalls;
#   * drain closes its end under a writer;
#   * rux starts a command with a chosen SIGPIPE, a signal storm,
#     non-blocking or no stdin, and reports its exit code;
#   * readshim, a shared library preloaded through LD_PRELOAD or
#     DYLD_INSERT_LIBRARIES, makes stdin's reads fail with EIO after N
#     bytes or makes a signal interrupt them, and counts the interrupted
#     reads and polls.
#
# The contract every case holds blake3ppsum to:
#   * a digest printed is the digest of exactly the bytes that arrived,
#     checked against blake3ppsum of the same bytes from a file, which
#     is a different read path;
#   * a failure blake3ppsum can see -- a read error, no usable stdin, a
#     write that failed on its own stdout -- prints no digest and exits
#     non-zero;
#   * its exit code and the producer's both reach the caller, so a
#     pipefail shell fails the pipeline when the producer did.
# A producer that dies is NOT visible to the reader: its end closes and
# the reader sees an ordinary end of stream, on every platform (the
# `producer_dies` case checks that drain sees the same end either way).
# The pipeline's exit status is what reports it.

cmake_minimum_required(VERSION 3.28)

foreach(v SUM GEN PUMP DRAIN RUX WORK CASE)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "faults.cmake needs -D${v}")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# A stream of 3 MiB plus a remainder. That is several of the sequential
# read's 1 MiB buffers and part of one default window, and a multiple of
# neither. --threads 4 reads stdin through the stream driver and
# --threads 1 through the sequential loop. Every case that reads runs
# both.
set(N 3145829)
set(GEN_ARGS --gen 1 --chunk 64)
set(MODES 4 1)  # --threads: pipelined, then sequential

if(CMAKE_HOST_APPLE)
  set(PRELOAD_VAR DYLD_INSERT_LIBRARIES)
else()
  set(PRELOAD_VAR LD_PRELOAD)
endif()
# A preloaded readshim comes before the sanitizer runtime in the library
# list. GCC's AddressSanitizer is a shared runtime that refuses to start
# unless it is first; clang's links into the binary and does not check.
# The order is required: readshim forwards each call to the next
# definition, which is the sanitizer's own interceptor. The check is
# switched off for the preloaded commands, keeping whatever else the lane
# set.
set(SHIM_ASAN "ASAN_OPTIONS=$ENV{ASAN_OPTIONS}:verify_asan_link_order=0")

# What a process that did not exit 0 looks like in RESULTS_VARIABLE: a
# number, or a description such as "Child killed".
function(expect_success what code)
  if(NOT code STREQUAL "0")
    message(FATAL_ERROR "${what}: expected exit 0, got '${code}'")
  endif()
endfunction()
function(expect_failure what code)
  if(code STREQUAL "0")
    message(FATAL_ERROR "${what}: expected a failure, got exit 0")
  endif()
endfunction()

# The digest blake3ppsum prints for a file holding the first `bytes` of the
# stream -- the reference a pipeline's digest is held to.
function(reference out_var bytes)
  set(f "${WORK}/ref-${bytes}.bin")
  execute_process(COMMAND "${PUMP}" ${GEN_ARGS} --close-after ${bytes}
                  OUTPUT_FILE "${f}" RESULT_VARIABLE r)
  expect_success("writing the reference" "${r}")
  execute_process(COMMAND "${SUM}" "${f}" OUTPUT_VARIABLE out RESULT_VARIABLE r)
  expect_success("hashing the reference" "${r}")
  string(REGEX MATCH "^[0-9a-f]+" d "${out}")
  set(${out_var} "${d}" PARENT_SCOPE)
endfunction()

function(digest_of out_var text)
  string(REGEX MATCH "^[0-9a-f]+" d "${text}")
  set(${out_var} "${d}" PARENT_SCOPE)
endfunction()

# The files the stdout_closed_* cases hash: all with the same content, so
# a --check list can name every one with the same digest. The names are
# long so that --check's short lines still add up to more than a pipe
# holds.
function(write_many_files out_var count)
  set(names "")
  foreach(i RANGE 1 ${count})
    set(f "${WORK}/file-${i}-padding-padding-padding-padding-padding-padding")
    file(WRITE "${f}" "the same few bytes in every file\n")
    list(APPEND names "${f}")
  endforeach()
  set(${out_var} "${names}" PARENT_SCOPE)
endfunction()

# blake3ppsum hashing (`hash`) or checking (`check`) many files into a
# stdout with no reader, in batch and per-file mode.
function(stdout_closed_many mode)
  if(CMAKE_HOST_WIN32)
    set(variants native)
  else()
    set(variants default ignore)
  endif()
  if(mode STREQUAL "hash")
    # 4 KiB of hex per line: 128 lines are 512 KiB.
    write_many_files(names 128)
    set(args --length 2048 ${names})
  else()
    # About 150 bytes per "name: OK" line: 3000 lines are 450 KiB.
    write_many_files(names 3000)
    list(GET names 0 first)
    execute_process(COMMAND "${SUM}" "${first}" OUTPUT_VARIABLE out RESULT_VARIABLE r)
    expect_success("hashing the --check reference" "${r}")
    digest_of(d "${out}")
    set(list_text "")
    foreach(f IN LISTS names)
      string(APPEND list_text "${d}  ${f}\n")
    endforeach()
    file(WRITE "${WORK}/list.b3" "${list_text}")
    set(args --check "${WORK}/list.b3")
  endif()
  foreach(t IN LISTS MODES)
    foreach(v IN LISTS variants)
      set(sig "")
      if(NOT v STREQUAL "native")
        set(sig --sigpipe ${v})
      endif()
      execute_process(
        COMMAND "${RUX}" ${sig} -- "${SUM}" --threads ${t} ${args}
        COMMAND "${DRAIN}" --widen 0 --close-after 0
        ERROR_VARIABLE err RESULTS_VARIABLE codes)
      list(GET codes 0 sc)
      set(what "blake3ppsum ${mode} --threads ${t} into a closed pipe (SIGPIPE ${v})")
      expect_failure("${what}" "${sc}")
      if(NOT v STREQUAL "default")
        # rux reports the child's status on stderr too; only the tool's
        # own lines count.
        string(REGEX MATCHALL "blake3ppsum: [^\n]*" said "${err}")
        list(LENGTH said n)
        if(NOT sc STREQUAL "1" OR NOT n EQUAL 1 OR NOT err MATCHES "blake3ppsum: write error")
          message(FATAL_ERROR "${what}: expected exit 1 and one write error, got exit ${sc} and ${n} messages: ${err}")
        endif()
      endif()
      message(STATUS "${what}: exited ${sc}")
    endforeach()
  endforeach()
endfunction()

# Whether readshim can be used: it is built on Unix where the toolchain
# makes shared libraries (bench/pipe/CMakeLists.txt), never on Windows.
set(HAVE_SHIM FALSE)
if(DEFINED SHIM AND NOT SHIM STREQUAL "" AND EXISTS "${SHIM}" AND NOT CMAKE_HOST_WIN32)
  set(HAVE_SHIM TRUE)
endif()

# ----------------------------------------------------------------------
if(CASE STREQUAL "short_stream")
  # The producer writes N bytes and closes: the digest is of those N.
  reference(want ${N})
  foreach(t IN LISTS MODES)
    execute_process(
      COMMAND "${PUMP}" ${GEN_ARGS} --close-after ${N}
      COMMAND "${SUM}" --threads ${t} -
      OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
    list(GET codes 0 pc)
    list(GET codes 1 sc)
    expect_success("pump" "${pc}")
    expect_success("blake3ppsum --threads ${t}" "${sc}")
    digest_of(got "${out}")
    if(NOT got STREQUAL want)
      message(FATAL_ERROR "--threads ${t}: digest ${got}, the bytes that arrived hash to ${want}")
    endif()
  endforeach()

elseif(CASE STREQUAL "empty_stream")
  # The producer writes nothing: the digest is the empty input's.
  foreach(t IN LISTS MODES)
    execute_process(
      COMMAND "${PUMP}" --gen 0
      COMMAND "${SUM}" --threads ${t} -
      OUTPUT_VARIABLE out RESULTS_VARIABLE codes)
    list(GET codes 1 sc)
    expect_success("blake3ppsum --threads ${t}" "${sc}")
    digest_of(got "${out}")
    if(NOT got STREQUAL "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262")
      message(FATAL_ERROR "--threads ${t}: empty input hashed to ${got}")
    endif()
  endforeach()

elseif(CASE STREQUAL "producer_dies")
  # The producer is killed after N bytes, with no clean exit. The reader
  # cannot tell: it hashes the N bytes and exits 0. The producer's own
  # non-zero status is what reports the failure.
  reference(want ${N})
  foreach(t IN LISTS MODES)
    execute_process(
      COMMAND "${PUMP}" ${GEN_ARGS} --die-after ${N}
      COMMAND "${SUM}" --threads ${t} -
      OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
    list(GET codes 0 pc)
    list(GET codes 1 sc)
    expect_failure("the killed pump's status" "${pc}")
    expect_success("blake3ppsum --threads ${t}" "${sc}")
    digest_of(got "${out}")
    if(NOT got STREQUAL want)
      message(FATAL_ERROR "--threads ${t}: digest ${got} after the producer died; "
                          "the ${N} bytes it wrote hash to ${want}")
    endif()
  endforeach()
  # And the reader sees the same end of stream either way. If a platform
  # ever reports a terminated writer differently from a clean close, this
  # fails -- and that platform could then refuse the digest.
  foreach(how close-after die-after)
    execute_process(
      COMMAND "${PUMP}" ${GEN_ARGS} --${how} ${N}
      COMMAND "${DRAIN}" --buf 64
      ERROR_VARIABLE err RESULTS_VARIABLE codes)
    string(REGEX MATCH "bytes=([0-9]+)" _ "${err}")
    set(bytes_${how} "${CMAKE_MATCH_1}")
    string(REGEX MATCH "end=([a-z0-9-]+)" _ "${err}")
    set(end_${how} "${CMAKE_MATCH_1}")
  endforeach()
  message(STATUS "a clean close ends as '${end_close-after}', a killed writer as '${end_die-after}'")
  if(NOT bytes_close-after STREQUAL "${N}" OR NOT bytes_die-after STREQUAL "${N}")
    message(FATAL_ERROR "drain read ${bytes_close-after} and ${bytes_die-after} bytes, expected ${N}")
  endif()
  if(NOT end_close-after STREQUAL end_die-after)
    message(FATAL_ERROR "a killed writer ends the stream as '${end_die-after}', a clean "
                        "close as '${end_close-after}': the reader CAN tell them apart here")
  endif()

elseif(CASE STREQUAL "pipefail")
  # The same through a real shell: under pipefail the pipeline fails
  # when the producer was killed, because blake3ppsum's 0 does not mask it.
  if(CMAKE_HOST_WIN32)
    message(STATUS "skipped: cmd.exe has no pipefail; producer_dies checks each status")
    return()
  endif()
  execute_process(COMMAND sh -c "set -o pipefail" RESULT_VARIABLE has)
  if(NOT has EQUAL 0)
    message(STATUS "skipped: this sh has no pipefail")
    return()
  endif()
  execute_process(
    COMMAND sh -c "set -o pipefail; \"$0\" --gen 1 --chunk 64 --die-after ${N} | \"$1\" - > /dev/null"
            "${PUMP}" "${SUM}"
    RESULT_VARIABLE r)
  expect_failure("the pipefail pipeline with a killed producer" "${r}")
  execute_process(
    COMMAND sh -c "set -o pipefail; \"$0\" --gen 1 --chunk 64 --close-after ${N} | \"$1\" - > /dev/null"
            "${PUMP}" "${SUM}"
    RESULT_VARIABLE r)
  expect_success("the pipefail pipeline with a clean producer" "${r}")

elseif(CASE STREQUAL "stall")
  # A producer that pauses mid-stream: waited for, nothing lost. The
  # pause is input; the assertion is the digest, which no timing moves.
  reference(want ${N})
  foreach(t IN LISTS MODES)
    execute_process(
      COMMAND "${PUMP}" ${GEN_ARGS} --close-after ${N} --stall-ms 300 --after 1000003
      COMMAND "${SUM}" --threads ${t} -
      OUTPUT_VARIABLE out RESULTS_VARIABLE codes)
    list(GET codes 1 sc)
    expect_success("blake3ppsum --threads ${t}" "${sc}")
    digest_of(got "${out}")
    if(NOT got STREQUAL want)
      message(FATAL_ERROR "--threads ${t}: digest ${got} across a stall, expected ${want}")
    endif()
  endforeach()

elseif(CASE STREQUAL "read_error")
  # stdin's reads fail with EIO after a million bytes: no digest, non-zero.
  if(NOT HAVE_SHIM)
    message(STATUS "skipped: readshim is not built here (Windows has no preload mechanism; a toolchain without shared libraries cannot build it)")
    return()
  endif()
  foreach(t IN LISTS MODES)
    execute_process(
      COMMAND "${PUMP}" ${GEN_ARGS} --close-after ${N}
      COMMAND "${CMAKE_COMMAND}" -E env READSHIM_FAIL_AFTER=1000003 "${SHIM_ASAN}" "${PRELOAD_VAR}=${SHIM}"
              "${SUM}" --threads ${t} -
      OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
    list(GET codes 1 sc)
    if(NOT err MATCHES "injected_eio=[1-9]")
      message(FATAL_ERROR "--threads ${t}: the EIO was never injected; readshim saw: ${err}")
    endif()
    expect_failure("blake3ppsum --threads ${t} after a read error" "${sc}")
    if(out MATCHES "[0-9a-f][0-9a-f][0-9a-f][0-9a-f]")
      message(FATAL_ERROR "--threads ${t}: printed a digest after a read error: ${out}")
    endif()
  endforeach()

elseif(CASE STREQUAL "eintr_storm")
  # Signals interrupt the reads (a SIGUSR1 handler without SA_RESTART):
  # every interrupted call is retried and the digest is unchanged.
  if(NOT HAVE_SHIM)
    message(STATUS "skipped: needs POSIX signals and readshim")
    return()
  endif()
  set(want_bytes 8388611)
  set(GEN_ARGS --gen 1 --chunk 4)
  reference(want ${want_bytes})
  foreach(t IN LISTS MODES)
    execute_process(
      COMMAND "${PUMP}" ${GEN_ARGS} --close-after ${want_bytes} --gap-us 200
      COMMAND "${CMAKE_COMMAND}" -E env READSHIM_EINTR=1 "${SHIM_ASAN}" "${PRELOAD_VAR}=${SHIM}"
              "${RUX}" --signal-storm 300 -- "${SUM}" --threads ${t} -
      OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
    list(GET codes 1 sc)
    expect_success("blake3ppsum --threads ${t} under a signal storm" "${sc}")
    digest_of(got "${out}")
    if(NOT got STREQUAL want)
      message(FATAL_ERROR "--threads ${t}: digest ${got} under a signal storm, expected ${want}")
    endif()
    # Proof the fault happened: interrupted calls, counted by readshim in
    # read() and in poll() (with their fortified forms and ppoll() on
    # Linux and FreeBSD).
    string(REGEX MATCH "fd0 reads=[1-9][0-9]*[^\n]*eintr=([0-9]+) poll_eintr=([0-9]+)" _ "${err}")
    if(NOT CMAKE_MATCH_COUNT EQUAL 2)
      message(FATAL_ERROR "--threads ${t}: no readshim report from blake3ppsum: ${err}")
    endif()
    math(EXPR interrupted "${CMAKE_MATCH_1}+${CMAKE_MATCH_2}")
    message(STATUS "--threads ${t}: ${interrupted} interrupted calls, all retried")
    if(interrupted EQUAL 0)
      message(FATAL_ERROR "--threads ${t}: no call was interrupted, so nothing was tested: ${err}")
    endif()
  endforeach()

elseif(CASE STREQUAL "nonblock_stdin")
  # stdin handed over non-blocking. POSIX: every EAGAIN is waited out and
  # the digest is unchanged. Windows (PIPE_NOWAIT): the reader waits out
  # ERROR_NO_DATA the same way; the case also accepts no digest and a
  # non-zero exit, and never a digest of a partial stream.
  set(GEN_ARGS --gen 1 --chunk 4)
  set(want_bytes 4194307)
  reference(want ${want_bytes})
  foreach(t IN LISTS MODES)
    execute_process(
      COMMAND "${PUMP}" ${GEN_ARGS} --close-after ${want_bytes} --gap-us 200
      COMMAND "${RUX}" --nonblock-stdin -- "${SUM}" --threads ${t} -
      OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes)
    list(GET codes 1 sc)
    digest_of(got "${out}")
    if(CMAKE_HOST_WIN32 AND NOT sc STREQUAL "0")
      if(NOT got STREQUAL "")
        message(FATAL_ERROR "--threads ${t}: failed with exit ${sc} yet printed ${got}")
      endif()
      message(STATUS "--threads ${t}: refused a non-blocking pipe: ${err}")
    else()
      expect_success("blake3ppsum --threads ${t} on non-blocking stdin" "${sc}")
      if(NOT got STREQUAL want)
        message(FATAL_ERROR "--threads ${t}: digest ${got} on non-blocking stdin, expected ${want}")
      endif()
    endif()
  endforeach()

elseif(CASE STREQUAL "closed_stdin")
  # No stdin at all: no digest, non-zero.
  foreach(t IN LISTS MODES)
    execute_process(
      COMMAND "${RUX}" --close-stdin -- "${SUM}" --threads ${t} -
      OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE sc)
    expect_failure("blake3ppsum --threads ${t} with no stdin" "${sc}")
    if(out MATCHES "[0-9a-f][0-9a-f][0-9a-f][0-9a-f]")
      message(FATAL_ERROR "--threads ${t}: printed a digest with no stdin: ${out}")
    endif()
  endforeach()

elseif(CASE STREQUAL "stdout_closed")
  # blake3ppsum's own stdout loses its reader before the digest is
  # written: it must not exit 0. With SIGPIPE at its default it dies of
  # the signal; ignored, and on Windows, it reports the write error.
  if(CMAKE_HOST_WIN32)
    set(variants native)
  else()
    set(variants default ignore)
  endif()
  foreach(v IN LISTS variants)
    set(sig "")
    if(NOT v STREQUAL "native")
      set(sig --sigpipe ${v})
    endif()
    # The ordering is a handshake, not a delay: drain closes its end and
    # then marks it, and pump holds the rest of the stream until the mark
    # exists, so blake3ppsum cannot reach its digest before its reader
    # is gone.
    set(mark "${WORK}/closed-${v}")
    file(REMOVE "${mark}")
    execute_process(
      COMMAND "${PUMP}" ${GEN_ARGS} --close-after ${N} --after 1048576 --wait-for "${mark}"
      COMMAND "${RUX}" ${sig} -- "${SUM}" -
      COMMAND "${DRAIN}" --close-after 0 --mark "${mark}"
      ERROR_VARIABLE err RESULTS_VARIABLE codes)
    list(GET codes 1 sc)
    expect_failure("blake3ppsum writing into a closed pipe (SIGPIPE ${v})" "${sc}")
    if(NOT v STREQUAL "default" AND NOT err MATCHES "blake3ppsum: write error")
      message(FATAL_ERROR "SIGPIPE ${v}: exited ${sc} without saying why: ${err}")
    endif()
    message(STATUS "SIGPIPE ${v}: blake3ppsum exited ${sc}")
  endforeach()

elseif(CASE STREQUAL "stdout_closed_batch")
  # Many digests into a stdout whose reader has gone, more than stdio and
  # an unwidened pipe hold between them, so writes fail while files are
  # still being hashed: through the batch (--threads 4) and file by file
  # (--threads 1). The run must end with exit 1 and the one message main
  # prints, with no abort and no per-file errors.
  stdout_closed_many(hash)

elseif(CASE STREQUAL "stdout_closed_check")
  # The same through --check: every line OK, and more of them than an
  # unwidened pipe holds. A failed write must end the run with exit 1 and
  # main's one message, never with a second write from the handler.
  stdout_closed_many(check)

elseif(CASE STREQUAL "gen_reader_closed")
  # blake3ppgen's reader goes away after 1 MiB of a 1 GiB stream: it must
  # stop and fail, not finish or exit 0.
  if(CMAKE_HOST_WIN32)
    set(variants native)
  else()
    set(variants default ignore)
  endif()
  foreach(v IN LISTS variants)
    set(sig "")
    if(NOT v STREQUAL "native")
      set(sig --sigpipe ${v})
    endif()
    execute_process(
      COMMAND "${RUX}" ${sig} -- "${GEN}" --length 1073741824
      COMMAND "${DRAIN}" --close-after 1048576
      ERROR_VARIABLE err RESULTS_VARIABLE codes)
    list(GET codes 0 gc)
    expect_failure("blake3ppgen writing into a closed pipe (SIGPIPE ${v})" "${gc}")
    if(NOT v STREQUAL "default" AND NOT err MATCHES "blake3ppgen: write error")
      message(FATAL_ERROR "SIGPIPE ${v}: exited ${gc} without saying why: ${err}")
    endif()
    message(STATUS "SIGPIPE ${v}: blake3ppgen exited ${gc}")
  endforeach()

else()
  message(FATAL_ERROR "faults.cmake: unknown case '${CASE}'")
endif()
