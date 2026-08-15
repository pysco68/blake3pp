# Cross-platform smoke tests for blake3ppsum, run in CMake script mode so
# the same driver works on Linux, macOS and Windows (no shell involved).
#
#   cmake -DSUM=<path-to-blake3ppsum> -DWORK=<scratch-dir> -DCASE=<name>
#         -P smoketest.cmake

if(NOT DEFINED SUM OR NOT DEFINED WORK OR NOT DEFINED CASE)
  message(FATAL_ERROR "smoketest.cmake needs -DSUM, -DWORK and -DCASE")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run_sum out_var result_var)
  cmake_parse_arguments(RS "" "STDIN" "ARGS" ${ARGN})
  set(input_args "")
  if(RS_STDIN)
    set(input_args INPUT_FILE "${RS_STDIN}")
  endif()
  execute_process(
    COMMAND "${SUM}" ${RS_ARGS}
    WORKING_DIRECTORY "${WORK}"
    ${input_args}
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE res)
  set(${out_var} "${out}" PARENT_SCOPE)
  set(${result_var} "${res}" PARENT_SCOPE)
endfunction()

if(CASE STREQUAL "version")
  run_sum(out res ARGS --version)
  if(NOT res EQUAL 0 OR NOT out MATCHES "blake3ppsum")
    message(FATAL_ERROR "--version failed (rc=${res}): ${out}")
  endif()

elseif(CASE STREQUAL "empty_stdin")
  # Known answer: BLAKE3 of the empty input.
  file(WRITE "${WORK}/empty" "")
  run_sum(out res STDIN "${WORK}/empty")
  if(NOT res EQUAL 0 OR NOT out MATCHES
     "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262")
    message(FATAL_ERROR "empty-stdin digest wrong (rc=${res}): ${out}")
  endif()

elseif(CASE STREQUAL "check_roundtrip")
  file(WRITE "${WORK}/f" "hello blake3pp")
  run_sum(sums res ARGS f)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "hashing failed (rc=${res})")
  endif()
  file(WRITE "${WORK}/sums" "${sums}")
  run_sum(out res ARGS --check sums)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "--check rejected its own output (rc=${res}): ${out}")
  endif()

  # Corrupt the first hex digit; --check must now fail with exit code 1.
  string(SUBSTRING "${sums}" 0 1 first)
  if(first STREQUAL "0")
    set(flipped "1")
  else()
    set(flipped "0")
  endif()
  string(SUBSTRING "${sums}" 1 -1 rest)
  file(WRITE "${WORK}/bad_sums" "${flipped}${rest}")
  run_sum(out res ARGS --check bad_sums)
  if(res EQUAL 0)
    message(FATAL_ERROR "--check accepted a corrupted checksum")
  endif()

else()
  message(FATAL_ERROR "unknown CASE '${CASE}'")
endif()
