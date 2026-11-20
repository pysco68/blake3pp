#pragma once

// The trace buffer's records as Chrome trace JSON, for the tools that
// record one (blake3pp_bench_file, blake3ppsum, blake3ppgen). Open the
// file in ui.perfetto.dev or chrome://tracing.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <set>
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
//     which core did the work, and for how long: tid = cpu, and the lane
//     after the highest cpu for a record whose cpu is unknown, each slice
//     named by its kind (compress, fold, fill) with the window and file in
//     its args, and a flow from the window's compute phase to it.
//
// The views collapse independently in the viewer. ts and dur are in
// microseconds, as the format wants; epoch_ns at the top level is the
// records' zero as absolute CLOCK_MONOTONIC nanoseconds, for joining with
// timestamps other tools take on the same clock. A write run names its
// backend in `write_backend`, which driver_stats does not carry.
inline bool write_chrome_trace(const std::string& path,
                               const blake3pp::trace_buffer& trace,
                               std::string_view process_name,
                               unsigned pool_threads = 0,
                               bool write_side = false,
                               std::string_view write_backend = {}) {
  std::ofstream out(path);
  if (!out.is_open()) {
    return false;
  }
  // Where the records' instants and spans become numbers, and the one
  // place: an instant as microseconds since the buffer's epoch, the
  // format's ts; a span as microseconds for dur, and as integer
  // nanoseconds where an args field says _ns.
  using std::chrono::nanoseconds;
  const blake3pp::trace_clock::time_point epoch = trace.epoch();
  const auto us_at = [epoch](blake3pp::trace_clock::time_point t) {
    return std::chrono::duration<double, std::micro>(t - epoch).count();
  };
  const auto us = [](nanoseconds d) {
    return std::chrono::duration<double, std::micro>(d).count();
  };
  const auto ns = [](nanoseconds d) { return d.count(); };
  out << std::format("{{\"epoch_ns\":{},\"displayTimeUnit\":\"ns\","
                     "\"pool_threads\":{},\"traceEvents\":[\n",
                     ns(epoch.time_since_epoch()), pool_threads);
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
  // lanes rather than as one track nesting into itself. A batch over a
  // shared context records no loop counters, which then read zero, but its
  // files still name the backend that read them.
  const auto& d = trace.driver();
  const auto windows = trace.windows();
  if ((d.iterations > 0 || !blake3pp::read_backend(d).empty() ||
       !write_backend.empty()) &&
      !windows.empty()) {
    // Over every window, not the first and last record: windows finish
    // out of order, writes especially.
    blake3pp::trace_clock::time_point begin = windows.front().t_wait_begin;
    blake3pp::trace_clock::time_point end = windows.front().t_released;
    for (const auto& w : windows) {
      begin = std::min(begin, w.t_wait_begin);
      end = std::max(end, w.t_released);
    }
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
        "\"lane_wait_ns\":{},\"read_backend\":\"{}\"{}}}}}",
        us_at(begin), us(end - begin), ns(d.busy), ns(d.parked),
        d.iterations, d.polls, d.blocking_polls, d.wakes, d.queue_runs,
        d.admission_stalls, d.max_pending, d.transfers_landed,
        d.transfer_bytes, ns(d.device_wait), ns(d.starved),
        d.queue_depth_final, d.queue_depth_peak, ns(d.agent_busy),
        ns(d.device_wait_min), d.device_depth, ns(d.lane_wait),
        blake3pp::read_backend(d),
        write_side ? std::format(",\"write_backend\":\"{}\"", write_backend)
                   : std::string{});
  }
  std::set<std::uint32_t> slots_named;
  for (const auto& w : trace.windows()) {
    const std::uint32_t tid = w.slot;
    if (slots_named.insert(tid).second) {
      out << std::format(",\n{{\"name\":\"thread_name\",\"ph\":\"M\","
                         "\"pid\":1,\"tid\":{},\"args\":{{\"name\":"
                         "\"window object\"}}}}",
                         tid);
    }
    // The same five stamps mean different stages on the write side:
    // admitted, fill begun on an agent, fill done, write landed, released.
    const std::pair<const char*, std::pair<blake3pp::trace_clock::time_point,
                                           blake3pp::trace_clock::time_point>>
        phases[] = {{write_side ? "queue" : "wait", {w.t_wait_begin, w.t_ready}},
                    {write_side ? "fill" : "hash", {w.t_ready, w.t_joined}},
                    {write_side ? "write" : "absorb", {w.t_joined, w.t_absorbed}},
                    {"release", {w.t_absorbed, w.t_released}}};
    for (const auto& [name, span] : phases) {
      out << std::format(",\n{{\"name\":\"{}\",\"ph\":\"X\",\"pid\":1,"
                         "\"tid\":{},\"ts\":{:.3f},\"dur\":{:.3f},"
                         "\"args\":{{\"window\":{},\"file\":{},\"slot\":{},"
                         "\"bytes\":{},\"last\":{},\"parallel\":{}}}}}",
                         name, tid, us_at(span.first),
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
  std::set<std::uint32_t> cpus_named;
  std::map<std::pair<std::uint32_t, std::uint64_t>,
           std::pair<std::uint32_t, blake3pp::trace_clock::time_point>>
      compute_of;  // (file, window) -> (slot, t_ready)
  for (const auto& w : trace.windows()) {
    compute_of[{w.file, w.index}] = {w.slot, w.t_ready};
  }
  std::uint64_t flow = 0;
  // A record whose cpu the platform could not say goes on the lane after
  // the highest cpu seen, which no real cpu can share.
  std::uint32_t unknown_tid = 0;
  for (const auto& a : trace.agents()) {
    if (a.cpu != ~std::uint32_t{0}) {
      unknown_tid = std::max(unknown_tid, a.cpu + 1);
    }
  }
  for (const auto& a : trace.agents()) {
    const bool known = a.cpu != ~std::uint32_t{0};
    const std::uint32_t tid = known ? a.cpu : unknown_tid;
    if (cpus_named.insert(tid).second) {
      out << std::format(",\n{{\"name\":\"thread_name\",\"ph\":\"M\","
                         "\"pid\":2,\"tid\":{},\"args\":{{\"name\":\"{}\"}}}}",
                         tid, known ? "cpu" : "cpu unknown");
    }
    const char* const kind =
        write_side ? "fill" : (a.parts == 0 ? "fold" : "compress");
    out << std::format(",\n{{\"name\":\"{}\",\"ph\":\"X\",\"pid\":2,"
                       "\"tid\":{},\"ts\":{:.3f},\"dur\":{:.3f},"
                       "\"args\":{{\"window\":{},\"file\":{},\"parts\":{},"
                       "\"min_part_ns\":{},\"max_part_ns\":{}}}}}",
                       kind, tid, us_at(a.t_begin), us(a.busy), a.window,
                       a.file, a.parts, ns(a.min_part), ns(a.max_part));
    if (const auto it = compute_of.find({a.file, a.window});
        it != compute_of.end() && a.busy > nanoseconds::zero()) {
      ++flow;
      const auto [slot, t_ready] = it->second;
      out << std::format(",\n{{\"name\":\"{}\",\"cat\":\"window\",\"ph\":\"s\","
                         "\"id\":{},\"pid\":1,\"tid\":{},\"ts\":{:.3f}}}",
                         kind, flow, slot, us_at(t_ready) + 0.001);
      out << std::format(",\n{{\"name\":\"{}\",\"cat\":\"window\",\"ph\":\"f\","
                         "\"bp\":\"e\",\"id\":{},\"pid\":2,\"tid\":{},"
                         "\"ts\":{:.3f}}}",
                         kind, flow, tid, us_at(a.t_begin) + 0.001);
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
    std::vector<std::pair<blake3pp::trace_clock::time_point, int>> edges;
    edges.reserve(trace.agents().size() * 2);
    for (const auto& a : trace.agents()) {
      edges.emplace_back(a.t_begin, +1);
      edges.emplace_back(a.t_begin + a.busy, -1);
    }
    std::sort(edges.begin(), edges.end());
    int busy = 0;
    for (std::size_t i = 0; i < edges.size();) {
      const blake3pp::trace_clock::time_point at = edges[i].first;
      while (i < edges.size() && edges[i].first == at) {
        busy += edges[i].second;
        ++i;
      }
      out << std::format(",\n{{\"name\":\"agents busy\",\"ph\":\"C\","
                         "\"pid\":2,\"tid\":0,\"ts\":{:.3f},"
                         "\"args\":{{\"busy\":{}}}}}",
                         us_at(at), busy);
    }
  }
  out << "\n]}\n";
  // A trace smaller than the stream's buffer reaches the file only here,
  // so the close is part of the answer.
  out.close();
  return !out.fail();
}

}  // namespace b3tool
