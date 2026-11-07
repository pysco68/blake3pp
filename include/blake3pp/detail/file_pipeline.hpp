#pragma once

// The file pipeline over a real device: the loop and its senders
// (driver_loop.hpp), the plan (pipeline_plan.hpp) and the scope
// (window_scope.hpp), with the definition-site checks that io_driver
// models what they need.
//
// Internal to the pipeline. Names an execution provider, so
// <blake3pp/io.hpp> must not reach it.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <blake3pp/detail/driver_loop.hpp>
#include <blake3pp/detail/ex_compat.hpp>
#include <blake3pp/detail/io_driver.hpp>
#include <blake3pp/detail/pipeline_plan.hpp>
#include <blake3pp/detail/window_scope.hpp>

namespace blake3pp::detail {

// Definition-site concept checks, as the I/O backends carry for theirs.
// io_driver is the model every provider has to host; an operation state
// only becomes a type once a receiver is named, which is what
// probe_receiver is for.
namespace pipeline_conformance {
using probe = ex_compat::probe_receiver;

static_assert(ex::sender<read_sender<io_driver>>);
static_assert(ex::operation_state<io_op_state<io_driver, probe, io_dir::read>>);
static_assert(ex::sender<write_sender<io_driver>>);
static_assert(ex::operation_state<io_op_state<io_driver, probe, io_dir::write>>);
// The declared adaptor over senders with no surprises of their own, so
// the asserts are about the adaptor and not about whatever it happens to
// wrap: the two value shapes the pipeline gates, and the valueless one
// the write side forwards.
using declared_cv_child = decltype(ex::just(std::array<std::uint32_t, 8>{}));
using declared_cv =
    declared_sender<declared_cv_child, std::array<std::uint32_t, 8>>;
static_assert(ex::sender<declared_cv>);
static_assert(ex::operation_state<
              declared_op<declared_cv_child, probe,
                          std::array<std::uint32_t, 8>>>);
using declared_n_child = decltype(ex::just(std::size_t{}));
static_assert(ex::sender<declared_sender<declared_n_child, std::size_t>>);
static_assert(
    ex::operation_state<declared_op<declared_n_child, probe, std::size_t>>);
using declared_void_child = decltype(ex::just());
static_assert(ex::sender<declared_sender<declared_void_child>>);
static_assert(ex::operation_state<declared_op<declared_void_child, probe>>);
static_assert(ex::sender<schedule_sender<io_driver>>);
static_assert(ex::operation_state<schedule_op<io_driver, probe>>);
static_assert(ex::scheduler<driver_scheduler<io_driver>>);
}  // namespace pipeline_conformance

}  // namespace blake3pp::detail
