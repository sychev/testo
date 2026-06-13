#pragma once

// ============================================================================
// ParallelExecutor — asio-only implementation (asio >= 1.36, C++20). Coro-free.
// ============================================================================
//
// Post-Coro version of the parallel block's fan-out / fan-in primitive. There
// is no `#include <coro/...>` here — it depends only on asio.
//
// --- Can it be isolated from Coro? -----------------------------------------
//
// The *source* is Coro-free, and with asio 1.36 + C++20 the toolchain is no
// longer an obstacle (make_parallel_group, cancellation, native coroutines and
// a Boost-free asio::spawn are all available).
//
// But it still cannot be dropped in as an isolated swap while the rest of the
// interpreter runs on Coro, for one reason that no asio version changes:
//
//     a branch can only suspend on the same coroutine substrate it runs on.
//
// A branch ultimately runs the action code (wait / sleep / exec / IO), which
// suspends at its waiting points. Today that suspension goes through Coro
// (coro::Timer, coro::AsioTask, coro::CheckPoint -> Coro::yield). If a branch is
// started as an asio coroutine, a Coro yield inside it has no Coro fiber to
// suspend; the two mechanisms do not interoperate. So this executor becomes
// usable only once the action layer suspends through asio (co_await / yield)
// instead of Coro. That migration is project-wide; it is not in this file.
//
// In short: with this toolchain the executor is ready, and the *only* remaining
// precondition is migrating the action substrate off Coro.
//
// --- Style note -------------------------------------------------------------
//
// This file uses native C++20 coroutines (awaitable / co_spawn). That implies
// the call chain (run_parallel_block, visit_command, the actions) becomes
// awaitable too — a viral but idiomatic change. For this codebase, which is
// written in a synchronous fiber style, the lower-effort alternative is the
// stackful flavour: asio::spawn + yield_context (Boost-free since asio 1.28).
// There the branch stays a `void(yield_context)` and join() takes a
// yield_context; the action code only threads `yield` into each async op
// instead of being rewritten as awaitables. The fan-out/fan-in below is
// identical in both flavours — only the suspension token differs.
//
// ============================================================================

#include <functional>
#include <vector>
#include <utility>
#include <exception>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/experimental/parallel_group.hpp>

struct ParallelExecutor {
	// A branch is a factory producing the coroutine to run. A factory (rather
	// than a ready awaitable) is used because an awaitable is single-shot and
	// must be handed straight to co_spawn — we want to create it at launch time.
	using Branch = std::function<asio::awaitable<void>()>;

	// Register a branch to run concurrently with the others. Nothing starts
	// here; join() launches them all together so the group can supervise them.
	void spawn(Branch branch) {
		branches.push_back(std::move(branch));
	}

	// Run every registered branch concurrently and wait for all of them.
	//
	// Fail-fast: wait_for_one_error() cancels the still-running branches as soon
	// as one finishes with an error (a non-null exception_ptr); we then rethrow
	// the first captured exception. This mirrors coro::CoroPool::waitAll(): one
	// failure propagates, the rest are abandoned. If all branches succeed,
	// nothing is thrown.
	//
	// `co_await executor.join();` from the parent coroutine — join() suspends
	// the parent until the whole group completes.
	asio::awaitable<void> join() {
		if (branches.empty()) {
			co_return;
		}

		auto executor = co_await asio::this_coro::executor;

		// One deferred child operation per branch (not started yet).
		std::vector<Operation> ops;
		ops.reserve(branches.size());
		for (auto& branch: branches) {
			ops.push_back(asio::co_spawn(executor, branch(), asio::deferred));
		}

		auto [completion_order, exceptions] =
			co_await asio::experimental::make_parallel_group(std::move(ops))
				.async_wait(
					asio::experimental::wait_for_one_error(),
					asio::use_awaitable);

		(void)completion_order;

		for (const std::exception_ptr& e: exceptions) {
			if (e) {
				std::rethrow_exception(e);
			}
		}
	}

private:
	// The element type co_spawn(..., deferred) yields for an awaitable<void>
	// branch; deduce it instead of spelling out an asio-internal type.
	using Operation = decltype(asio::co_spawn(
		std::declval<asio::any_io_executor&>(),
		std::declval<asio::awaitable<void>>(),
		asio::deferred));

	std::vector<Branch> branches;
};
