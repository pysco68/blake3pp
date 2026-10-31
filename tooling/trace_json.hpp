#pragma once

// The trace buffer's records as Chrome trace JSON, for the tools that
// record one (blake3pp_bench_file, blake3ppsum, blake3ppgen). Open the
// file in ui.perfetto.dev or chrome://tracing.

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include <blake3pp/trace.hpp>

namespace b3tool {

// The record kinds as Chrome trace JSON, one complete ("X") event per
// phase per window on the lane of the window object that carried it.
// Three processes, because a viewer titles a thread track "<name> <tid>"
// and the tid is the one number that can be made to mean something:
//
//   * process 0, the driver: one lane;
//   * process 1, the pipeline's window objects: tid = slot, so a lane
//     reads "window object 4";
//   * process 2, the agent records, the other view of the same windows --
//     which core did the work, and for how long: tid = cpu (99 where
//     unknown), each slice named by its kind (compress, fold, fill) with
//     the window and file in its args, and a flow from the window's
//     compute phase to it.
//
// The views collapse independently in the viewer. ts and dur are in
// microseconds, as the format wants; epoch_ns at the top level is the
// records' zero as absolute CLOCK_MONOTONIC nanoseconds, for joining with
// timestamps other tools take on the same clock.
inline bool write_chrome_trace(const std::string& path,
                               const blake3pp::trace_buffer& trace,
                               std::string_view process_name,
                               unsigned pool_threads = 0,
                               bool write_side = false) {
  std::ofstream out(path);
  if (!out.is_open()) {
    return false;
  }
  const auto us = [](std::int64_t ns) {
    return static_cast<double>(ns) / 1000.0;
  };
  out << std::format("{{\"epoch_ns\":{},\"displayTimeUnit\":\"ns\","
                     "\"pool_threads\":{},\"traceEvents\":[\n",
                     trace.epoch_ns(), pool_threads);
  out << std::format("{{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":0,"
                     "\"args\":{{\"name\":\"{} driver\"}}}},\n",
                     process_name);
  out << std::format("{{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,"
                     "\"args\":{{\"name\":\"{} pipeline, by window object\"}}}},\n",
                     process_name);
  out << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":0,\"tid\":0,"
         "\"args\":{\"name\":\"driver loop\"}}";
  // The driver's own track: one event over the whole run, carrying what
  // that thread spent its time on. Windows go on the track of the window
  // object that carried them, so several in flight read as parallel
  // lanes rather than as one track nesting into itself.
  const auto& d = trace.driver();
  const auto windows = trace.windows();
  if (d.iterations > 0 && !windows.empty()) {
    const std::int64_t begin = windows.front().t_wait_begin;
    const std::int64_t end = windows.back().t_released;
    out << std::format(
        ",\n{{\"name\":\"driver\",\"ph\":\"X\",\"pid\":0,\"tid\":0,"
        "\"ts\":{:.3f},\"dur\":{:.3f},\"args\":{{\"busy_ns\":{},"
        "\"parked_ns\":{},\"iterations\":{},\"polls\":{},"
        "\"blocking_polls\":{},\"wakes\":{},\"queue_runs\":{},"
        "\"admission_stalls\":{},\"max_pending\":{},"
        "\"transfers_landed\":{},\"transfer_bytes\":{},"
        "\"device_wait_ns\":{},\"starved_ns\":{},"
        "\"queue_depth_final\":{},"
        "\"queue_depth_peak\":{},\"agent_busy_ns\":{},"
        "\"device_wait_min_ns\":{},\"device_depth\":{},"
        "\"lane_wait_ns\":{},\"read_backend\":\"{}\"}}}}",
        us(begin), us(end - begin), d.busy_ns, d.parked_ns, d.iterations,
        d.polls, d.blocking_polls, d.wakes, d.queue_runs, d.admission_stalls,
        d.max_pending, d.transfers_landed, d.transfer_bytes,
        d.device_wait_ns, d.starved_ns, d.queue_depth_final,
        d.queue_depth_peak, d.agent_busy_ns, d.device_wait_min_ns,
        d.device_depth, d.lane_wait_ns, blake3pp::read_backend(d));
  }
  std::map<std::uint32_t, bool> slots_named;
  for (const auto& w : trace.windows()) {
    const std::uint32_t tid = w.slot;
    if (!slots_named[tid]) {
      slots_named[tid] = true;
      out << std::format(",\n{{\"name\":\"thread_name\",\"ph\":\"M\","
                         "\"pid\":1,\"tid\":{},\"args\":{{\"name\":"
                         "\"window object\"}}}}",
                         tid);
    }
    // The same five stamps mean different stages on the write side:
    // admitted, fill begun on an agent, fill done, write landed, released.
    const std::pair<const char*, std::pair<std::int64_t, std::int64_t>>
        phases[] = {{write_side ? "queue" : "wait", {w.t_wait_begin, w.t_ready}},
                    {write_side ? "fill" : "hash", {w.t_ready, w.t_joined}},
                    {write_side ? "write" : "absorb", {w.t_joined, w.t_absorbed}},
                    {"release", {w.t_absorbed, w.t_released}}};
    for (const auto& [name, span] : phases) {
      out << std::format(",\n{{\"name\":\"{}\",\"ph\":\"X\",\"pid\":1,"
                         "\"tid\":{},\"ts\":{:.3f},\"dur\":{:.3f},"
                         "\"args\":{{\"window\":{},\"file\":{},\"slot\":{},"
                         "\"bytes\":{},\"last\":{},\"parallel\":{}}}}}",
                         name, tid, us(span.first),
                         us(span.second - span.first), w.index, w.file,
                         w.slot, w.bytes,
                         (w.flags & blake3pp::window_record::flag_last) != 0
                             ? 1
                             : 0,
                         (w.flags & blake3pp::window_record::flag_parallel) != 0
                             ? 1
                             : 0);
    }
  }
  if (!trace.agents().empty()) {
    out << std::format(",\n{{\"name\":\"process_name\",\"ph\":\"M\","
                       "\"pid\":2,\"args\":{{\"name\":\"{} pool agents, "
                       "by cpu\"}}}}",
                       process_name);
  }
  // The agents' slices, named by their kind, and one flow per slice from
  // the window's compute phase on the pipeline side to it: the viewer
  // draws the arrows and highlights what a selected slice is linked to.
  // A flow is a start event bound to the slice enclosing it on one lane
  // and an end event ("bp":"e") bound the same way on another, so both
  // sit a hair inside their slices.
  std::map<std::uint32_t, bool> cpus_named;
  std::map<std::pair<std::uint32_t, std::uint64_t>, std::pair<std::uint32_t, std::int64_t>>
      compute_of;  // (file, window) -> (slot, t_ready)
  for (const auto& w : trace.windows()) {
    compute_of[{w.file, w.index}] = {w.slot, w.t_ready};
  }
  std::uint64_t flow = 0;
  for (const auto& a : trace.agents()) {
    const std::uint32_t tid = a.cpu == ~std::uint32_t{0} ? 99 : a.cpu;
    if (!cpus_named[tid]) {
      cpus_named[tid] = true;
      out << std::format(",\n{{\"name\":\"thread_name\",\"ph\":\"M\","
                         "\"pid\":2,\"tid\":{},\"args\":{{\"name\":\"{}\"}}}}",
                         tid, tid == 99 ? "cpu unknown, tid" : "cpu");
    }
    const char* const kind =
        write_side ? "fill" : (a.parts == 0 ? "fold" : "compress");
    out << std::format(",\n{{\"name\":\"{}\",\"ph\":\"X\",\"pid\":2,"
                       "\"tid\":{},\"ts\":{:.3f},\"dur\":{:.3f},"
                       "\"args\":{{\"window\":{},\"file\":{},\"parts\":{},"
                       "\"min_part_ns\":{},\"max_part_ns\":{}}}}}",
                       kind, tid, us(a.t_begin),
                       us(static_cast<std::int64_t>(a.busy_ns)), a.window,
                       a.file, a.parts, a.min_part_ns, a.max_part_ns);
    if (const auto it = compute_of.find({a.file, a.window});
        it != compute_of.end() && a.busy_ns > 0) {
      ++flow;
      const auto [slot, t_ready] = it->second;
      out << std::format(",\n{{\"name\":\"{}\",\"cat\":\"window\",\"ph\":\"s\","
                         "\"id\":{},\"pid\":1,\"tid\":{},\"ts\":{:.3f}}}",
                         kind, flow, slot, us(t_ready) + 0.001);
      out << std::format(",\n{{\"name\":\"{}\",\"cat\":\"window\",\"ph\":\"f\","
                         "\"bp\":\"e\",\"id\":{},\"pid\":2,\"tid\":{},"
                         "\"ts\":{:.3f}}}",
                         kind, flow, tid, us(a.t_begin) + 0.001);
    }
  }
  // A counter beside the agent slices, because the slices alone mislead.
  // On a run the device cannot feed, the pool is idle most of the wall
  // clock, so the per-cpu lanes are mostly empty space, and empty space
  // reads as "the trace lost something" rather than as "the pool was
  // starved". A counter is drawn as a filled curve pinned to zero, which
  // says the second thing. It is emitted at every change point, so it is
  // exact rather than sampled, and it costs two events per agent record.
  if (!trace.agents().empty()) {
    std::vector<std::pair<std::int64_t, int>> edges;
    edges.reserve(trace.agents().size() * 2);
    for (const auto& a : trace.agents()) {
      edges.emplace_back(a.t_begin, +1);
      edges.emplace_back(a.t_begin + static_cast<std::int64_t>(a.busy_ns), -1);
    }
    std::sort(edges.begin(), edges.end());
    int busy = 0;
    std::int64_t at = edges.empty() ? 0 : edges.front().first;
    for (std::size_t i = 0; i < edges.size();) {
      at = edges[i].first;
      while (i < edges.size() && edges[i].first == at) {
        busy += edges[i].second;
        ++i;
      }
      out << std::format(",\n{{\"name\":\"agents busy\",\"ph\":\"C\","
                         "\"pid\":2,\"tid\":0,\"ts\":{:.3f},"
                         "\"args\":{{\"busy\":{}}}}}",
                         us(at), busy);
    }
  }
  out << "\n]}\n";
  return out.good();
}

}  // namespace b3tool
