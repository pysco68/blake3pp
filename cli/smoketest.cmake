# Cross-platform smoke tests for blake3ppsum and blake3ppgen, run in CMake
# script mode so the same driver works on Linux, macOS and Windows (no
# shell involved).
#
#   cmake -DSUM=<blake3ppsum> -DGEN=<blake3ppgen> -DWORK=<scratch-dir>
#         -DCASE=<name> [-DEMULATOR=<prefix>] -P smoketest.cmake

# Script mode starts from no policy at all, so a CMake that still has the
# OLD behaviour of a policy this file depends on takes it: without this,
# `IN_LIST` below is parsed as three arguments rather than an operator on
# CMake 3.x, and every case fails before it runs a tool. The project's own
# minimum, so the two cannot drift.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SUM OR NOT DEFINED WORK OR NOT DEFINED CASE)
  message(FATAL_ERROR "smoketest.cmake needs -DSUM, -DWORK and -DCASE")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# EMULATOR (optional, e.g. "qemu-aarch64;-L;/usr/aarch64-linux-gnu")
# prefixes every tool invocation on cross builds; empty on native ones.

# What a smoke test's tools may take of the machine. ctest runs these cases in
# parallel and each tool defaults to a worker per core, so without a bound a
# 32-core box runs 32 cases x 32 workers, and the cases spend minutes being
# descheduled, which reads as a hang. Four exercises the parallel paths. The
# cases that are about thread counts pass their own.
set(SMOKE_THREADS 4)

# ERR_VAR names a variable to receive stderr, which is where --diag and
# the usage errors report. Output is decoded as UTF-8, which is what the
# tools write; on Windows, under this file's policy version, execute_process
# would otherwise decode it in the console's code page (CMP0176).
function(run_sum out_var result_var)
  cmake_parse_arguments(RS "" "STDIN;ERR_VAR" "ARGS" ${ARGN})
  if(NOT "--threads" IN_LIST RS_ARGS)
    list(APPEND RS_ARGS --threads ${SMOKE_THREADS})
  endif()
  set(input_args "")
  if(RS_STDIN)
    set(input_args INPUT_FILE "${RS_STDIN}")
  endif()
  execute_process(
    COMMAND ${EMULATOR} "${SUM}" ${RS_ARGS}
    WORKING_DIRECTORY "${WORK}"
    ${input_args}
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE res
    ENCODING UTF-8)
  set(${out_var} "${out}" PARENT_SCOPE)
  set(${result_var} "${res}" PARENT_SCOPE)
  if(RS_ERR_VAR)
    set(${RS_ERR_VAR} "${err}" PARENT_SCOPE)
  endif()
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

elseif(CASE STREQUAL "stdin_parallel")
  # A multi-window stream on stdin runs the window pipeline over the
  # stream driver when a pool exists; it must agree with the sequential
  # read and with the file pipeline over the same bytes.
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed stdin --length 20000000
    --threads ${SMOKE_THREADS}
    OUTPUT_FILE "${WORK}/stream" RESULT_VARIABLE r0)
  run_sum(par res1 ARGS --threads 4 - STDIN "${WORK}/stream")
  run_sum(seq res2 ARGS --threads 1 - STDIN "${WORK}/stream")
  run_sum(file res3 ARGS --threads 4 stream)
  string(SUBSTRING "${par}" 0 64 par_hex)
  string(SUBSTRING "${seq}" 0 64 seq_hex)
  string(SUBSTRING "${file}" 0 64 file_hex)
  if(NOT r0 EQUAL 0 OR NOT res1 EQUAL 0 OR NOT res2 EQUAL 0 OR NOT res3 EQUAL 0
     OR NOT par_hex STREQUAL seq_hex OR NOT par_hex STREQUAL file_hex)
    message(FATAL_ERROR "stdin digests diverge: par=${par_hex} seq=${seq_hex} file=${file_hex}")
  endif()

elseif(CASE STREQUAL "stdin_diag")
  # --diag over a stream: the stream driver supplies the counters, so a
  # diagnosis is printed. The reading depends on the machine and is not
  # asserted.
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed diag --length 20000000
    --threads ${SMOKE_THREADS}
    OUTPUT_FILE "${WORK}/stream" RESULT_VARIABLE r0)
  run_sum(out res ERR_VAR err
    ARGS --diag --window 1 --threads 4 - STDIN "${WORK}/stream")
  if(NOT r0 EQUAL 0 OR NOT res EQUAL 0)
    message(FATAL_ERROR "--diag over a stream failed (rc=${res}): ${err}")
  endif()
  if(NOT err MATCHES "diagnosis:" OR NOT err MATCHES "pool busy")
    message(FATAL_ERROR "--diag over a stream reported nothing: ${err}")
  endif()
  if(err MATCHES "sequential path")
    message(FATAL_ERROR "--diag over a stream found no driver: ${err}")
  endif()
  # Standard input with no FILE given is the same one run.
  run_sum(out res3 ERR_VAR err3 ARGS --diag --window 1 STDIN "${WORK}/stream")
  if(NOT res3 EQUAL 0 OR NOT err3 MATCHES "diagnosis:")
    message(FATAL_ERROR "--diag over implicit standard input failed (rc=${res3}): ${err3}")
  endif()
  # One run only, still: a checksum list is several.
  run_sum(out res2 ERR_VAR err2 ARGS --diag --check -)
  if(res2 EQUAL 0)
    message(FATAL_ERROR "--diag --check was accepted")
  endif()

elseif(CASE STREQUAL "trace_inputs")
  # Trace storage is sized before the run, so an input whose size is not
  # a FILE argument must still get room for its windows: standard input
  # with no FILE given, and the files a --check list names.
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed trace --length 20000000
    --threads ${SMOKE_THREADS}
    OUTPUT_FILE "${WORK}/stream" RESULT_VARIABLE r0)
  run_sum(sums res0 ARGS stream)
  file(WRITE "${WORK}/sums" "${sums}")
  run_sum(out res1 ERR_VAR err1 ARGS --trace t1.json --window 1
    STDIN "${WORK}/stream")
  run_sum(out res2 ERR_VAR err2 ARGS --trace t2.json --window 1 --check sums)
  if(NOT r0 EQUAL 0 OR NOT res0 EQUAL 0 OR NOT res1 EQUAL 0 OR NOT res2 EQUAL 0)
    message(FATAL_ERROR "a traced run failed (rc=${res1}, ${res2}): ${err1} ${err2}")
  endif()
  foreach(err IN ITEMS "${err1}" "${err2}")
    if(NOT err MATCHES "trace: ([0-9]+) window records" OR CMAKE_MATCH_1 LESS 19
       OR err MATCHES "dropped")
      message(FATAL_ERROR "the trace had no room for the run's windows: ${err}")
    endif()
  endforeach()
  # The driver event carries the device's depth, the lanes' queueing and
  # the backend that read.
  file(READ "${WORK}/t1.json" json)
  if(NOT json MATCHES "\"device_depth\":[0-9]+" OR NOT json MATCHES "\"lane_wait_ns\":[0-9]+"
     OR NOT json MATCHES "\"read_backend\":\"[^\"]+\"")
    message(FATAL_ERROR "the driver event lacks the device and backend fields")
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

elseif(CASE STREQUAL "check_batched")
  # Several files verify over one context (the batched path), and the
  # lines come back in list order with one bad digest among them. Each
  # file is written at a different size so the windows differ.
  file(WRITE "${WORK}/one" "first file")
  string(REPEAT "second file " 4000 bulk)
  file(WRITE "${WORK}/two" "${bulk}")
  file(WRITE "${WORK}/three" "third file")
  run_sum(sums res ARGS one two three)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "hashing failed (rc=${res})")
  endif()
  file(WRITE "${WORK}/sums" "${sums}")
  run_sum(out res ARGS --check sums)
  if(NOT res EQUAL 0 OR NOT out MATCHES "one: OK.*two: OK.*three: OK")
    message(FATAL_ERROR "batched --check lost files or their order (rc=${res}): ${out}")
  endif()

  # The middle line's digest is corrupted: that file alone must FAIL, and
  # the two around it must still be reported, in order.
  string(REPLACE "\n" ";" lines "${sums}")
  set(bad "")
  foreach(line ${lines})
    if(line MATCHES "two$")
      string(SUBSTRING "${line}" 1 -1 rest)
      string(SUBSTRING "${line}" 0 1 first)
      if(first STREQUAL "0")
        set(line "1${rest}")
      else()
        set(line "0${rest}")
      endif()
    endif()
    string(APPEND bad "${line}\n")
  endforeach()
  file(WRITE "${WORK}/bad_sums" "${bad}")
  run_sum(out res ARGS --check bad_sums)
  if(res EQUAL 0 OR NOT out MATCHES "one: OK.*two: FAILED.*three: OK")
    message(FATAL_ERROR "batched --check mislabelled the corrupted file (rc=${res}): ${out}")
  endif()

elseif(CASE STREQUAL "check_empty_list")
  # A list without one well-formed line verifies nothing and fails, as an
  # empty or comment-only list does in sha256sum -c; so does a list that
  # cannot be read (a directory).
  file(WRITE "${WORK}/empty_list" "")
  file(WRITE "${WORK}/comment_list" "# only a comment\n\n# and another\n")
  foreach(list IN ITEMS empty_list comment_list)
    run_sum(out res ERR_VAR err ARGS --check ${list})
    if(NOT res EQUAL 1 OR NOT err MATCHES "${list}: no properly formatted checksum lines found")
      message(FATAL_ERROR "--check of ${list} did not fail (rc=${res}): ${err}")
    endif()
  endforeach()
  run_sum(out res ERR_VAR err ARGS --check - STDIN "${WORK}/comment_list")
  if(NOT res EQUAL 1 OR NOT err MATCHES "no properly formatted checksum lines found")
    message(FATAL_ERROR "--check of a comment-only stdin did not fail (rc=${res}): ${err}")
  endif()
  file(MAKE_DIRECTORY "${WORK}/dir")
  run_sum(out res ERR_VAR err ARGS --check dir)
  if(NOT res EQUAL 1
     OR NOT err MATCHES "read error|cannot open|no properly formatted checksum lines found")
    message(FATAL_ERROR "--check of a directory did not fail (rc=${res}): ${err}")
  endif()

elseif(CASE STREQUAL "check_crlf")
  # A list with CRLF line endings verifies as a file and on stdin, over
  # the batched path (two files) and the sequential one.
  file(WRITE "${WORK}/one" "first file")
  file(WRITE "${WORK}/two" "second file")
  run_sum(sums res ARGS one two)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "hashing failed (rc=${res})")
  endif()
  # file(WRITE) on Windows writes every "\n" as CRLF already, so the
  # endings are set by NEWLINE_STYLE, which writes the same bytes on every
  # platform.
  file(CONFIGURE OUTPUT "${WORK}/crlf_sums" CONTENT "${sums}" @ONLY
       NEWLINE_STYLE CRLF)
  run_sum(out1 res1 ERR_VAR err1 ARGS --check crlf_sums)
  run_sum(out2 res2 ERR_VAR err2 ARGS --check - STDIN "${WORK}/crlf_sums")
  run_sum(out3 res3 ERR_VAR err3 ARGS --threads 1 --check crlf_sums)
  foreach(i 1 2 3)
    if(NOT res${i} EQUAL 0 OR NOT out${i} MATCHES "one: OK\ntwo: OK\n")
      message(FATAL_ERROR "CRLF list run ${i} failed (rc=${res${i}}): ${out${i}} ${err${i}}")
    endif()
  endforeach()

elseif(CASE STREQUAL "check_names")
  # Names the format has to carry exactly: a leading space everywhere; a
  # leading '*', a newline, a backslash and a carriage return where the
  # filesystem allows them. blake3ppsum writes them (escaped where the
  # name holds '\', newline or CR) and --check reads them back, on the
  # batched and the sequential path.
  set(names " lead")
  if(NOT CMAKE_HOST_WIN32)
    list(APPEND names "*star" "nl\nname" "back\\slash" "cr\rname")
  endif()
  foreach(name IN LISTS names)
    file(WRITE "${WORK}/${name}" "content of ${name}")
  endforeach()
  run_sum(sums res ARGS ${names})
  run_sum(seq res2 ARGS --threads 1 ${names})
  if(NOT res EQUAL 0 OR NOT res2 EQUAL 0 OR NOT sums STREQUAL seq)
    message(FATAL_ERROR "hashing the names failed (rc=${res}, ${res2}):\n${sums}\n${seq}")
  endif()
  # A newline in front, so that every line, the first included, follows one.
  set(lines "\n${sums}")
  if(NOT lines MATCHES "\n[0-9a-f]+   lead\n")
    message(FATAL_ERROR "the leading space was not written: ${sums}")
  endif()
  if(NOT CMAKE_HOST_WIN32)
    if(NOT lines MATCHES "\n[0-9a-f]+  \\*star\n"
       OR NOT lines MATCHES "\n\\\\[0-9a-f]+  nl\\\\nname\n"
       OR NOT lines MATCHES "\n\\\\[0-9a-f]+  back\\\\\\\\slash\n"
       OR NOT lines MATCHES "\n\\\\[0-9a-f]+  cr\\\\rname\n")
      message(FATAL_ERROR "the names were not written escaped: ${sums}")
    endif()
  endif()
  file(WRITE "${WORK}/sums" "${sums}")
  foreach(threads 4 1)
    run_sum(out res ERR_VAR err ARGS --threads ${threads} --check sums)
    if(NOT res EQUAL 0)
      message(FATAL_ERROR "--check --threads ${threads} rejected the names (rc=${res}): ${out} ${err}")
    endif()
    set(expect " lead: OK\n")
    if(NOT CMAKE_HOST_WIN32)
      list(APPEND expect "*star: OK\n" "\\nl\\nname: OK\n" "\\back\\\\slash: OK\n"
                         "\\cr\\rname: OK\n")
    endif()
    foreach(line IN LISTS expect)
      string(FIND "${out}" "${line}" at)
      if(at LESS 0)
        message(FATAL_ERROR "--check --threads ${threads} lacks '${line}': ${out}")
      endif()
    endforeach()
  endforeach()

elseif(CASE STREQUAL "check_escaped_line")
  # Hand-written lines in the GNU escaped form: the leading backslash is
  # not part of the digest, and "\\" in the name is one backslash. On
  # Windows that backslash is a path separator, so the name is a file in
  # a directory there. The digest is that of the empty input.
  set(empty_hex "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262")
  if(CMAKE_HOST_WIN32)
    file(WRITE "${WORK}/back/slash" "")
  else()
    file(WRITE "${WORK}/back\\slash" "")
  endif()
  file(WRITE "${WORK}/plain" "")
  file(WRITE "${WORK}/sums"
    "\\${empty_hex}  back\\\\slash\n\\${empty_hex} *back\\\\slash\n\\${empty_hex}  plain\n")
  run_sum(out res ERR_VAR err ARGS --threads 1 --check sums)
  string(FIND "${out}" "\\back\\\\slash: OK\n\\back\\\\slash: OK\nplain: OK\n" at)
  if(NOT res EQUAL 0 OR at LESS 0)
    message(FATAL_ERROR "escaped lines did not verify (rc=${res}): ${out} ${err}")
  endif()

elseif(CASE STREQUAL "check_nonhex")
  # A digest field of even length that is not hex is a malformed line, not
  # a mismatch.
  file(WRITE "${WORK}/f" "hello blake3pp")
  run_sum(sums res ARGS f)
  string(REPEAT "g0" 32 nonhex)
  file(WRITE "${WORK}/list" "${sums}${nonhex}  f\n")
  run_sum(out res ERR_VAR err ARGS --check list)
  if(NOT res EQUAL 1 OR NOT out STREQUAL "f: OK\n"
     OR NOT err MATCHES "list:2: malformed line"
     OR NOT err MATCHES "WARNING: 1 line is improperly formatted"
     OR err MATCHES "did NOT match")
    message(FATAL_ERROR "a non-hex digest was not reported malformed (rc=${res}): ${out} ${err}")
  endif()

elseif(CASE STREQUAL "check_counts")
  # The closing summary counts malformed lines, files that could not be
  # read and mismatches apart: here 2, 1 and 3, on the batched and the
  # sequential path. A list that cannot be opened is reported on its own
  # and is not a listed file.
  set(empty_hex "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262")
  string(REPEAT "0" 64 zeros)
  file(WRITE "${WORK}/f" "hello blake3pp")
  run_sum(sums res ARGS f)
  file(WRITE "${WORK}/list"
    "${sums}abc f\n\\${empty_hex}  bad\\qescape\n${zeros}  nofile\n"
    "${zeros}  f\n${zeros} *f\n${empty_hex}  f\n")
  foreach(threads 4 1)
    run_sum(out res ERR_VAR err ARGS --threads ${threads} --check list)
    if(NOT res EQUAL 1 OR NOT out MATCHES "^f: OK\n"
       OR NOT err MATCHES "WARNING: 2 lines are improperly formatted"
       OR NOT err MATCHES "WARNING: 1 listed file could not be read"
       OR NOT err MATCHES "WARNING: 3 computed checksums did NOT match")
      message(FATAL_ERROR "--check --threads ${threads} miscounted (rc=${res}): ${out} ${err}")
    endif()
  endforeach()
  run_sum(out res ERR_VAR err ARGS --check list nolist)
  if(NOT res EQUAL 1 OR NOT err MATCHES "nolist: cannot open"
     OR NOT err MATCHES "WARNING: 1 listed file could not be read")
    message(FATAL_ERROR "a missing list was not counted (rc=${res}): ${err}")
  endif()

elseif(CASE STREQUAL "utf8_names")
  # A name no single-byte code page holds (Greek, Latin-1, CJK), given on
  # the command line, opens and comes back in the digest line as its
  # UTF-8 bytes, on the sequential and the batched path; the list so
  # written verifies. On Windows this takes the tools' UTF-8 process code
  # page: under an ANSI one the name reaches the tool lossy, and what it
  # prints is not UTF-8. The output goes to a file and is compared as
  # bytes, so no decoding stands between the tool and the check.
  set(name "Ω-é-文.bin")
  file(WRITE "${WORK}/${name}" "content of a UTF-8 name")
  file(WRITE "${WORK}/plain.bin" "content of a UTF-8 name")
  run_sum(ref res ARGS plain.bin)
  string(SUBSTRING "${ref}" 0 64 hex)
  if(NOT res EQUAL 0 OR NOT hex MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "hashing the ASCII name failed (rc=${res}): ${ref}")
  endif()
  string(HEX "${hex}  ${name}\n${hex}  plain.bin\n" expect)
  foreach(threads 1 4)
    execute_process(
      COMMAND ${EMULATOR} "${SUM}" --threads ${threads} "${name}" plain.bin
      WORKING_DIRECTORY "${WORK}"
      OUTPUT_FILE "${WORK}/sums${threads}"
      ERROR_VARIABLE err
      RESULT_VARIABLE res
      ENCODING UTF-8)
    file(READ "${WORK}/sums${threads}" got HEX)
    if(NOT res EQUAL 0 OR NOT got STREQUAL expect)
      file(READ "${WORK}/sums${threads}" text)
      message(FATAL_ERROR "--threads ${threads}: the UTF-8 name did not "
                          "hash or print as written (rc=${res}): ${text} ${err}\n"
                          "bytes ${got}\nwanted ${expect}")
    endif()
    run_sum(out res ERR_VAR err ARGS --threads ${threads} --check sums${threads})
    if(NOT res EQUAL 0 OR NOT out STREQUAL "${name}: OK\nplain.bin: OK\n")
      message(FATAL_ERROR "--threads ${threads}: the list written with the "
                          "UTF-8 name did not verify (rc=${res}): ${out} ${err}")
    endif()
  endforeach()

elseif(CASE STREQUAL "check_utf8_list")
  # A UTF-8 list, as written on Linux, names a file outside the ANSI code
  # page: it verifies from a file and from standard input, alone (the
  # sequential path) and beside a second file (the batched path), and the
  # OK lines carry the name as written.
  set(name "Ω-é-文.bin")
  file(WRITE "${WORK}/${name}" "content of a UTF-8 name")
  file(WRITE "${WORK}/plain.bin" "content of a UTF-8 name")
  run_sum(ref res ARGS plain.bin)
  string(SUBSTRING "${ref}" 0 64 hex)
  if(NOT res EQUAL 0 OR NOT hex MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "hashing the ASCII name failed (rc=${res}): ${ref}")
  endif()
  file(WRITE "${WORK}/one" "${hex}  ${name}\n")
  file(WRITE "${WORK}/two" "${hex}  ${name}\n${hex}  plain.bin\n")
  run_sum(out1 res1 ERR_VAR err1 ARGS --check one)
  run_sum(out2 res2 ERR_VAR err2 ARGS --check - STDIN "${WORK}/one")
  run_sum(out3 res3 ERR_VAR err3 ARGS --check two)
  run_sum(out4 res4 ERR_VAR err4 ARGS --check - STDIN "${WORK}/two")
  run_sum(out5 res5 ERR_VAR err5 ARGS --threads 1 --check two)
  foreach(i 1 2 3 4 5)
    if(i LESS 3)
      set(expect "${name}: OK\n")
    else()
      set(expect "${name}: OK\nplain.bin: OK\n")
    endif()
    if(NOT res${i} EQUAL 0 OR NOT out${i} STREQUAL expect)
      message(FATAL_ERROR "UTF-8 list run ${i} failed (rc=${res${i}}): ${out${i}} ${err${i}}")
    endif()
  endforeach()

elseif(CASE STREQUAL "gen_utf8_names")
  # blake3ppgen opens a name outside the ANSI code page given to
  # --seed-file and to --output, on the pipelined and the sequential
  # file sink, and writes what it writes for an ASCII name.
  set(name "Ω-é-文")
  file(WRITE "${WORK}/${name}.seed" "seed of a UTF-8 name")
  file(WRITE "${WORK}/plain.seed" "seed of a UTF-8 name")
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed-file plain.seed
    --length 100003 --threads 1
    WORKING_DIRECTORY "${WORK}"
    OUTPUT_FILE "${WORK}/plain.out" RESULT_VARIABLE r0)
  foreach(threads 1 4)
    execute_process(COMMAND ${EMULATOR} "${GEN}" --seed-file "${name}.seed"
      --length 100003 --threads ${threads} --output "${name}.${threads}"
      WORKING_DIRECTORY "${WORK}"
      ERROR_VARIABLE err RESULT_VARIABLE res ENCODING UTF-8)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
      "${WORK}/plain.out" "${WORK}/${name}.${threads}" RESULT_VARIABLE same)
    if(NOT r0 EQUAL 0 OR NOT res EQUAL 0 OR NOT same EQUAL 0)
      message(FATAL_ERROR "--threads ${threads}: gen over UTF-8 names "
                          "failed or diverged (rc=${r0}, ${res}): ${err}")
    endif()
  endforeach()

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
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed smoke --length 64 --hex
    --threads ${SMOKE_THREADS}
    OUTPUT_VARIABLE a RESULT_VARIABLE r1)
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed smoke --length 64 --hex
    --threads ${SMOKE_THREADS}
    OUTPUT_VARIABLE b RESULT_VARIABLE r2)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT a STREQUAL b)
    message(FATAL_ERROR "gen is not deterministic")
  endif()
  # Cross-check the tools: gen of the empty seed at length 32 IS the
  # blake3 digest of empty input.
  execute_process(COMMAND ${EMULATOR} "${GEN}" --length 32 --hex
    --threads ${SMOKE_THREADS}
    OUTPUT_VARIABLE g RESULT_VARIABLE r3)
  if(NOT r3 EQUAL 0 OR NOT g MATCHES
     "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262")
    message(FATAL_ERROR "gen of empty seed != blake3 of empty input: ${g}")
  endif()

elseif(CASE STREQUAL "gen_seek")
  # O(1) seek: a slice at an offset equals that region of the full stream.
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed smoke --length 148 --hex
    --threads ${SMOKE_THREADS}
    OUTPUT_VARIABLE full RESULT_VARIABLE r1)
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed smoke --seek 100 --length 48 --hex
    --threads ${SMOKE_THREADS}
    OUTPUT_VARIABLE slice RESULT_VARIABLE r2)
  string(SUBSTRING "${full}" 200 96 expected)  # bytes 100..148 as hex
  string(SUBSTRING "${slice}" 0 96 got)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT expected STREQUAL got)
    message(FATAL_ERROR "seeked slice diverges from the stream")
  endif()

elseif(CASE STREQUAL "gen_threads")
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed t --length 20000000 --threads 1
    OUTPUT_FILE "${WORK}/one" RESULT_VARIABLE r1)
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed t --length 20000000 --threads 4
    OUTPUT_FILE "${WORK}/four" RESULT_VARIABLE r2)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
    "${WORK}/one" "${WORK}/four" RESULT_VARIABLE same)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT same EQUAL 0)
    message(FATAL_ERROR "threaded generation diverges from sequential")
  endif()

elseif(CASE STREQUAL "gen_short_buffer")
  # The stdout buffer holds what --length asks for, not one segment per
  # thread: 64 bytes on 1024 threads must not take 4 GiB, and on a 32-bit
  # target the uncapped product wrapped to an empty buffer that wrote
  # nothing for ever. On Linux the run gets an address space too small
  # for the old buffer, so that regression fails instead of allocating.
  # A sanitizer's shadow memory and an emulator (node reserves the wasm
  # heap and its guard regions up front) need more than that, so those
  # lanes run it without the limit.
  set(limit "")
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT SANITIZED AND NOT EMULATOR)
    find_program(PRLIMIT prlimit)
    if(PRLIMIT)
      set(limit "${PRLIMIT}" --as=3221225472 --)
    endif()
  endif()
  execute_process(
    COMMAND ${limit} ${EMULATOR} "${GEN}" --seed t --length 64 --threads 1024
    OUTPUT_FILE "${WORK}/short" ERROR_VARIABLE err RESULT_VARIABLE r)
  file(SIZE "${WORK}/short" n)
  if(NOT r EQUAL 0 OR NOT n EQUAL 64)
    message(FATAL_ERROR "--length 64 --threads 1024: exit ${r}, ${n} bytes: ${err}")
  endif()

elseif(CASE STREQUAL "sum_alloc_failure")
  # A batch whose arena cannot be allocated: four 1 GiB windows in an
  # address space of 2 GiB. The failure comes after the engine is built,
  # so it is the run's (exit 1, not the usage status), it is reported
  # rather than aborting the process, and the trace is still written.
  # Linux only, where prlimit sets the limit; a sanitizer or an emulator
  # needs more address space than that to start at all.
  if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR SANITIZED OR EMULATOR)
    message(STATUS "skipped: needs prlimit on a native, unsanitized Linux build")
    return()
  endif()
  find_program(PRLIMIT prlimit)
  if(NOT PRLIMIT)
    message(STATUS "skipped: no prlimit")
    return()
  endif()
  file(WRITE "${WORK}/a" "a\n")
  file(WRITE "${WORK}/b" "b\n")
  execute_process(
    COMMAND "${PRLIMIT}" --as=2147483648 -- "${SUM}" --threads 4 --window 1024
      --trace "${WORK}/trace.json" a b
    WORKING_DIRECTORY "${WORK}"
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE r)
  set(traced NO)
  if(EXISTS "${WORK}/trace.json")
    set(traced YES)
  endif()
  if(NOT r STREQUAL "1" OR NOT err MATCHES "bad_alloc" OR NOT traced)
    message(FATAL_ERROR "an arena that cannot be allocated: exit ${r}, "
      "trace written: ${traced}, stderr: ${err}")
  endif()

elseif(CASE STREQUAL "trace_write_failure")
  # A trace that cannot be written is reported and fails the run, also
  # when it is small enough that nothing reaches the file before the
  # stream is closed. /dev/full accepts the open and refuses every write.
  if(NOT EXISTS /dev/full)
    message(STATUS "skipped: no /dev/full")
    return()
  endif()
  file(WRITE "${WORK}/a" "a\n")
  execute_process(COMMAND ${EMULATOR} "${SUM}" --trace /dev/full a
    WORKING_DIRECTORY "${WORK}" OUTPUT_QUIET ERROR_VARIABLE err RESULT_VARIABLE r)
  if(r EQUAL 0 OR NOT err MATCHES "could not write /dev/full")
    message(FATAL_ERROR "blake3ppsum --trace /dev/full: exit ${r}: ${err}")
  endif()
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed t --length 64 --threads 4 --output a2
      --trace /dev/full
    WORKING_DIRECTORY "${WORK}" OUTPUT_QUIET ERROR_VARIABLE err RESULT_VARIABLE r)
  if(r EQUAL 0 OR NOT err MATCHES "/dev/full")
    message(FATAL_ERROR "blake3ppgen --trace /dev/full: exit ${r}: ${err}")
  endif()

elseif(CASE STREQUAL "gen_seed_file")
  # --seed-file streams through the library's file pipeline: a seed
  # spanning several windows must hash identically sequentially and
  # multi-core, and cross-checks against blake3ppsum's extended output of
  # the same file (gen's stream IS that XOF).
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed material --length 20000000
    --threads ${SMOKE_THREADS}
    OUTPUT_FILE "${WORK}/seed" RESULT_VARIABLE r0)
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed-file "${WORK}/seed" --length 4000000 --threads 1
    OUTPUT_FILE "${WORK}/one" RESULT_VARIABLE r1)
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed-file "${WORK}/seed" --length 4000000 --threads 4
    OUTPUT_FILE "${WORK}/four" RESULT_VARIABLE r2)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
    "${WORK}/one" "${WORK}/four" RESULT_VARIABLE same)
  if(NOT r0 EQUAL 0 OR NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT same EQUAL 0)
    message(FATAL_ERROR "seed-file ingestion diverges between 1 and 4 threads")
  endif()
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed-file "${WORK}/seed" --length 64 --hex --threads 4
    OUTPUT_VARIABLE g RESULT_VARIABLE r3)
  execute_process(COMMAND ${EMULATOR} "${SUM}" --length 64 "${WORK}/seed"
    OUTPUT_VARIABLE s RESULT_VARIABLE r4)
  string(SUBSTRING "${g}" 0 128 g)
  string(SUBSTRING "${s}" 0 128 s)
  if(NOT r3 EQUAL 0 OR NOT r4 EQUAL 0 OR NOT g STREQUAL s)
    message(FATAL_ERROR "gen --seed-file stream != blake3ppsum --length 64 of the seed")
  endif()

elseif(CASE STREQUAL "gen_output")
  # --output (direct async I/O when available) must be byte-identical to
  # the stdout stream. Odd length: exercises the unaligned-tail path and
  # the preallocate-then-trim size logic.
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed o --length 20000003
    --threads ${SMOKE_THREADS}
    OUTPUT_FILE "${WORK}/stdout_stream" RESULT_VARIABLE r1)
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed o --length 20000003
    --output "${WORK}/direct"
    --threads ${SMOKE_THREADS}
    RESULT_VARIABLE r2)
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed o --length 20000003
    --output "${WORK}/threaded" --threads 4 RESULT_VARIABLE r3)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
    "${WORK}/stdout_stream" "${WORK}/direct" RESULT_VARIABLE same1)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
    "${WORK}/stdout_stream" "${WORK}/threaded" RESULT_VARIABLE same2)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT r3 EQUAL 0
     OR NOT same1 EQUAL 0 OR NOT same2 EQUAL 0)
    message(FATAL_ERROR "--output file diverges from the stdout stream")
  endif()

elseif(CASE STREQUAL "sum_errors")
  # The failure paths: a missing input, a --check list with comments,
  # a malformed line and a missing file, a missing list, and the one
  # usage error (key and data both on stdin).
  run_sum(out res ARGS missing.bin)
  if(res EQUAL 0)
    message(FATAL_ERROR "a missing input did not fail")
  endif()
  file(WRITE "${WORK}/f" "hello blake3pp")
  run_sum(sums res ARGS f)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "hashing failed (rc=${res})")
  endif()
  string(REPEAT "0" 64 zeros)
  file(WRITE "${WORK}/list" "# a comment\n\n${sums}abc f\n${zeros}  nofile\n")
  run_sum(out res ARGS --check list)
  if(res EQUAL 0 OR NOT out MATCHES "f: OK" OR NOT out MATCHES "FAILED open or read")
    message(FATAL_ERROR "--check did not report the bad lines (rc=${res}): ${out}")
  endif()
  run_sum(out res ARGS --check nolist)
  if(res EQUAL 0)
    message(FATAL_ERROR "--check of a missing list did not fail")
  endif()
  run_sum(out res ARGS --keyed - - STDIN "${WORK}/f")
  if(res EQUAL 0)
    message(FATAL_ERROR "key and data both on stdin was accepted")
  endif()

elseif(CASE STREQUAL "key_forms")
  # A 32-byte raw key equals its 64-hex spelling; the key may come from
  # stdin; keyed and derived digests of a large file (the parallel path)
  # equal their single-threaded ones.
  file(WRITE "${WORK}/rawkey" "0123456789abcdef0123456789abcdef")
  file(WRITE "${WORK}/hexkey"
    "3031323334353637383961626364656630313233343536373839616263646566")
  file(WRITE "${WORK}/f" "keyed payload")
  run_sum(a r1 ARGS --keyed rawkey f)
  run_sum(b r2 ARGS --keyed hexkey f)
  run_sum(c r3 ARGS --keyed - f STDIN "${WORK}/hexkey")
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT r3 EQUAL 0
     OR NOT a STREQUAL b OR NOT a STREQUAL c)
    message(FATAL_ERROR "key forms disagree: raw=${a} hex=${b} stdin=${c}")
  endif()
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed k --length 20000000
    --threads ${SMOKE_THREADS}
    OUTPUT_FILE "${WORK}/big" RESULT_VARIABLE rg)
  run_sum(p1 s1 ARGS --keyed hexkey big)
  run_sum(p2 s2 ARGS --keyed hexkey --threads 1 big)
  run_sum(d1 s3 ARGS --derive-key ctx big)
  run_sum(d2 s4 ARGS --derive-key ctx --threads 1 big)
  if(NOT rg EQUAL 0 OR NOT s1 EQUAL 0 OR NOT s2 EQUAL 0 OR NOT s3 EQUAL 0
     OR NOT s4 EQUAL 0 OR NOT p1 STREQUAL p2 OR NOT d1 STREQUAL d2)
    message(FATAL_ERROR "keyed/derived parallel path diverges from sequential")
  endif()

elseif(CASE STREQUAL "gen_errors")
  # Size parsing (suffixes, a bad suffix, garbage), an infinite --seek, a
  # missing seed file and an unwritable --output all fail cleanly; 1K
  # means 1024.
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed s --length 1K --hex
    --threads ${SMOKE_THREADS}
    OUTPUT_VARIABLE a RESULT_VARIABLE r1)
  execute_process(COMMAND ${EMULATOR} "${GEN}" --seed s --length 1024 --hex
    --threads ${SMOKE_THREADS}
    OUTPUT_VARIABLE b RESULT_VARIABLE r2)
  if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT a STREQUAL b)
    message(FATAL_ERROR "--length 1K is not 1024 bytes")
  endif()
  foreach(bad "--length;12X" "--length;abc" "--seek;inf"
              "--seed-file;${WORK}/nope" "--output;${WORK}/no/such/dir/out")
    execute_process(COMMAND ${EMULATOR} "${GEN}" --seed s --length 16 ${bad}
    --threads ${SMOKE_THREADS}
    OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE r)
    if(r EQUAL 0)
      message(FATAL_ERROR "gen accepted '${bad}'")
    endif()
  endforeach()
  # A negative size, a size whose suffix multiplication overflows 64 bits
  # and a --window whose byte count overflows size_t fail before any
  # output. The timeout bounds a run that takes -1 as 2^64-1 bytes; a
  # timed-out run leaves a message in r, not an exit code.
  foreach(bad "--length;-1" "--length;16777216T" "--seek;-5"
              "--length;16;--output;${WORK}/window.bin;--window;17592186044416")
    execute_process(COMMAND ${EMULATOR} "${GEN}" --seed s ${bad}
      --threads ${SMOKE_THREADS}
      OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE r TIMEOUT 60)
    if(NOT r MATCHES "^[1-9][0-9]*$")
      message(FATAL_ERROR "gen did not reject '${bad}': ${r}")
    endif()
  endforeach()

elseif(CASE STREQUAL "engines")
  # The engine introspection, end to end. --version prints the list the
  # library derives from its own degradation ladder; the --no-async help
  # text is built from that list by b3tool::no_async_note(). The two must
  # agree, so a new engine cannot leave the help text behind.
  run_sum(ver res ARGS --version)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "--version failed (rc=${res}): ${ver}")
  endif()
  if(NOT ver MATCHES "io engines:([^\n]*)\\(auto -> ([a-z0-9_]+)\\)")
    message(FATAL_ERROR "--version prints no io engines line:\n${ver}")
  endif()
  set(engines_line "${CMAKE_MATCH_1}")
  set(auto_engine "${CMAKE_MATCH_2}")
  string(REPLACE "[no os support]" " " engines_plain "${engines_line}")
  string(REGEX MATCHALL "[a-z0-9_]+" engine_list "${engines_plain}")
  if(engine_list STREQUAL "")
    message(FATAL_ERROR "io engines line names nothing: ${engines_line}")
  endif()
  # What auto resolves to has to be one of them, and has to be one the
  # machine grants.
  if(NOT auto_engine IN_LIST engine_list)
    message(FATAL_ERROR "auto -> ${auto_engine}, not in ${engine_list}")
  endif()
  if(engines_line MATCHES "${auto_engine}\\[no os support\\]")
    message(FATAL_ERROR "auto -> ${auto_engine}, reported unsupported")
  endif()
  # The last engine is the synchronous rung everything degrades to, and
  # no kernel refuses it.
  list(GET engine_list -1 floor_engine)
  if(engines_line MATCHES "${floor_engine}\\[no os support\\]")
    message(FATAL_ERROR "the floor ${floor_engine} is reported unsupported")
  endif()

  # The async engines are the ones above the first synchronous rung
  # (src/io/io_engines.cpp static_asserts that nothing async sits below
  # one). These three spellings are the whole of the synchronous side;
  # tests/io.cpp holds the same three against is_async().
  set(sync_names pread readfile stdio)
  set(expect_async "")
  foreach(e IN LISTS engine_list)
    if(e IN_LIST sync_names)
      break()
    endif()
    list(APPEND expect_async "${e}")
  endforeach()

  run_sum(help res ARGS --help)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "--help failed (rc=${res}): ${help}")
  endif()
  if(NOT help MATCHES "\\(no ([a-z0-9_/-]+) queue\\)")
    message(FATAL_ERROR "--no-async help names no engines:\n${help}")
  endif()
  string(REPLACE "/" ";" note_names "${CMAKE_MATCH_1}")
  if(expect_async STREQUAL "")
    # No async engine in this build: the note says so in those words.
    if(NOT note_names STREQUAL "async")
      message(FATAL_ERROR "no async engine compiled, but --no-async says "
                          "'${note_names}'")
    endif()
  elseif(NOT note_names STREQUAL "${expect_async}")
    message(FATAL_ERROR "--no-async names '${note_names}', the build carries "
                        "'${expect_async}'")
  endif()

  # The generator says the same thing in the write spellings, which
  # differ for exactly one engine ("gcd-write").
  execute_process(COMMAND ${EMULATOR} "${GEN}" --help
    OUTPUT_VARIABLE gen_help ERROR_VARIABLE gen_err RESULT_VARIABLE res)
  if(NOT gen_help MATCHES "\\(no ([a-z0-9_/-]+) queue\\)")
    message(FATAL_ERROR "gen --no-async names no engines:\n${gen_help}")
  endif()
  string(REPLACE "/" ";" gen_names "${CMAKE_MATCH_1}")
  set(gen_read "")
  foreach(n IN LISTS gen_names)
    string(REGEX REPLACE "-write$" "" n "${n}")
    list(APPEND gen_read "${n}")
  endforeach()
  if(NOT expect_async STREQUAL "" AND NOT gen_read STREQUAL "${expect_async}")
    message(FATAL_ERROR "gen --no-async names '${gen_names}', the build "
                        "carries '${expect_async}'")
  endif()

else()
  message(FATAL_ERROR "unknown CASE '${CASE}'")
endif()
