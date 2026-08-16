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

elseif(CASE STREQUAL "length_prefix")
  # Extended output: the 32-byte digest is the prefix of any longer output.
  file(WRITE "${WORK}/f" "extended output prefix property")
  run_sum(short res ARGS --length 32 f)
  run_sum(long res2 ARGS --length 64 f)
  string(SUBSTRING "${short}" 0 64 short_hex)
  string(SUBSTRING "${long}" 0 64 long_prefix)
  if(NOT res EQUAL 0 OR NOT res2 EQUAL 0 OR NOT short_hex STREQUAL long_prefix)
    message(FATAL_ERROR "--length 64 does not extend --length 32")
  endif()

elseif(CASE STREQUAL "keyed_roundtrip")
  # Key as 64 hex chars; keyed sums verify only with the same key.
  file(WRITE "${WORK}/key"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
  file(WRITE "${WORK}/f" "authenticated payload")
  run_sum(sums res ARGS --keyed key f)
  file(WRITE "${WORK}/sums" "${sums}")
  run_sum(out res2 ARGS --keyed key --check sums)
  if(NOT res EQUAL 0 OR NOT res2 EQUAL 0)
    message(FATAL_ERROR "keyed round trip failed: ${out}")
  endif()
  run_sum(out res3 ARGS --check sums)  # without the key: must FAIL
  if(res3 EQUAL 0)
    message(FATAL_ERROR "unkeyed --check accepted a keyed sum")
  endif()

elseif(CASE STREQUAL "derive_key")
  # Different contexts must derive different digests from the same data.
  file(WRITE "${WORK}/f" "master key material")
  run_sum(a res ARGS --derive-key "ctx one" f)
  run_sum(b res2 ARGS --derive-key "ctx two" f)
  if(NOT res EQUAL 0 OR NOT res2 EQUAL 0 OR a STREQUAL b)
    message(FATAL_ERROR "derive-key contexts not domain-separated")
  endif()

elseif(CASE STREQUAL "gen_deterministic")
  execute_process(COMMAND "${GEN}" --seed smoke --length 64 --hex
    OUTPUT_VARIABLE a RESULT_VARIABLE r1)
  execute_process(COMMAND "${GEN}" --seed smoke --length 64 --hex
    OUTPUT_VARIABLE b RESULT_VARIABLE r2)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT a STREQUAL b)
    message(FATAL_ERROR "gen is not deterministic")
  endif()
  # Cross-check the tools: gen of the empty seed at length 32 IS the
  # blake3 digest of empty input.
  execute_process(COMMAND "${GEN}" --length 32 --hex
    OUTPUT_VARIABLE g RESULT_VARIABLE r3)
  if(NOT r3 EQUAL 0 OR NOT g MATCHES
     "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262")
    message(FATAL_ERROR "gen of empty seed != blake3 of empty input: ${g}")
  endif()

elseif(CASE STREQUAL "gen_seek")
  # O(1) seek: a slice at an offset equals that region of the full stream.
  execute_process(COMMAND "${GEN}" --seed smoke --length 148 --hex
    OUTPUT_VARIABLE full RESULT_VARIABLE r1)
  execute_process(COMMAND "${GEN}" --seed smoke --seek 100 --length 48 --hex
    OUTPUT_VARIABLE slice RESULT_VARIABLE r2)
  string(SUBSTRING "${full}" 200 96 expected)  # bytes 100..148 as hex
  string(SUBSTRING "${slice}" 0 96 got)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT expected STREQUAL got)
    message(FATAL_ERROR "seeked slice diverges from the stream")
  endif()

elseif(CASE STREQUAL "gen_threads")
  execute_process(COMMAND "${GEN}" --seed t --length 20000000 --threads 1
    OUTPUT_FILE "${WORK}/one" RESULT_VARIABLE r1)
  execute_process(COMMAND "${GEN}" --seed t --length 20000000 --threads 4
    OUTPUT_FILE "${WORK}/four" RESULT_VARIABLE r2)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
    "${WORK}/one" "${WORK}/four" RESULT_VARIABLE same)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT same EQUAL 0)
    message(FATAL_ERROR "threaded generation diverges from sequential")
  endif()

elseif(CASE STREQUAL "gen_output")
  # --output (direct async I/O when available) must be byte-identical to
  # the stdout stream. Odd length: exercises the unaligned-tail path and
  # the preallocate-then-trim size logic.
  execute_process(COMMAND "${GEN}" --seed o --length 20000003
    OUTPUT_FILE "${WORK}/stdout_stream" RESULT_VARIABLE r1)
  execute_process(COMMAND "${GEN}" --seed o --length 20000003
    --output "${WORK}/direct" RESULT_VARIABLE r2)
  execute_process(COMMAND "${GEN}" --seed o --length 20000003
    --output "${WORK}/threaded" --threads 4 RESULT_VARIABLE r3)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
    "${WORK}/stdout_stream" "${WORK}/direct" RESULT_VARIABLE same1)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
    "${WORK}/stdout_stream" "${WORK}/threaded" RESULT_VARIABLE same2)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT r3 EQUAL 0
     OR NOT same1 EQUAL 0 OR NOT same2 EQUAL 0)
    message(FATAL_ERROR "--output file diverges from the stdout stream")
  endif()

else()
  message(FATAL_ERROR "unknown CASE '${CASE}'")
endif()
