#pragma once

// Printing a diagnosis. blake3pp::diagnose_read() and
// blake3pp::diagnose_write() in <blake3pp/trace.hpp> produce the reading;
// this is the half that formats, so only the tools include it. It returns
// strings and writes nothing, so each tool prints with its own println().

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <blake3pp/parallel_io.hpp>
#include <blake3pp/trace.hpp>

#include "tool_common.hpp"

namespace b3tool {

// A service time at the scale it reads at. Device figures span a drive's
// microseconds and a loaded queue's milliseconds in the same report.
[[nodiscard]] inline std::string service_label(
    std::chrono::duration<double, std::nano> t) {
  if (t >= std::chrono::milliseconds{1}) {
    return std::format(
        "{:.3f} ms", std::chrono::duration<double, std::milli>(t).count());
  }
  return std::format("{:.1f} us",
                     std::chrono::duration<double, std::micro>(t).count());
}

// The reading, or the two a run could not separate.
[[nodiscard]] inline std::string reading_label(const blake3pp::diagnosis& r) {
  if (r.label != blake3pp::bottleneck::undecided) {
    return std::string(blake3pp::to_string(r.label));
  }
  return std::format("{} or {}, this run does not separate them",
                     blake3pp::to_string(r.first),
                     blake3pp::to_string(r.second));
}

// The reason a run has no reading, or nothing when it has one: a run with
// no driver behind it, or one short enough that what it measured was its
// own ramp and drain. `noun` is the tool's word for a transfer,
// `no_driver` why this tool might have had none.
[[nodiscard]] inline std::optional<std::string> undiagnosable(
    const blake3pp::diagnosis& r, std::string_view tool,
    std::string_view noun, std::string_view no_driver) {
  if (!r.measured()) {
    return std::format("{}: diagnosis: {}", tool, no_driver);
  }
  if (!r.long_enough()) {
    return std::format(
        "{}: diagnosis: {} {} at a depth of {} is this run's own ramp and "
        "drain, not a measurement of either end",
        tool, r.transfers, noun, r.depth);
  }
  return std::nullopt;
}

// The lines about a stream run that the diagnosis cannot carry, in the
// order they print under it. Each states what the run did; a figure a
// platform cannot give is said to be unavailable rather than printed as
// zero.
[[nodiscard]] inline std::vector<std::string> stream_lines(
    const blake3pp::stream_report& s) {
  std::vector<std::string> out;
  if (s.early_hand_over) {
    out.push_back(std::format(
        "  stream         {} windows, {} ended early because the producer "
        "ran dry (early hand-over on)",
        s.windows, s.short_flushes));
  } else {
    out.push_back(std::format(
        "  stream         {} windows, each filled or ended by the end of "
        "the stream (early hand-over off)",
        s.windows));
  }
  if (s.stream_buffer_reported) {
    out.push_back(std::format("  pipe           {} buffer granted by the OS",
                              bytes_label(s.stream_buffer_bytes)));
  } else {
#if defined(__linux__)
    out.push_back(
        "  pipe           no buffer size: the input is not a pipe, or none "
        "was requested");
#else
    out.push_back(
        "  pipe           buffer size not reported on this platform");
#endif
  }
  if (s.watch.measured) {
    out.push_back(std::format(
        "  watch          {} waits on an empty stream: {} caught by a "
        "probe, {} slept; {} probes; budget {} us",
        s.watch.caught + s.watch.slept, s.watch.caught, s.watch.slept,
        s.watch.probes,
        std::chrono::duration_cast<std::chrono::microseconds>(s.watch.budget)
            .count()));
  } else {
    out.push_back("  watch          none on this platform: a read blocks");
  }
  if (s.read_ask_bound_bytes != 0) {
    out.push_back(std::format(
        "  read ask       bounded at {} by the end of the run",
        bytes_label(s.read_ask_bound_bytes)));
  }
  return out;
}

}  // namespace b3tool
