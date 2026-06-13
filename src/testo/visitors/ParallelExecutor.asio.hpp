#pragma once

// ============================================================================
// ParallelExecutor — asio-only target implementation (NOT wired in yet)
// ============================================================================
//
// This is the post-Coro version of the parallel block's fan-out / fan-in
// primitive. It depends ONLY on asio — there is no `#include <coro/...>` here.
//
// --- Can it be isolated from Coro? (read this) ------------------------------
//
// The *source* of this file is Coro-free, yes. But it CANNOT be dropped in as
// an isolated replacement while the rest of the interpreter still runs on Coro,
// for one fundamental reason:
//
//     a branch can only suspend on the same coroutine substrate it runs on.
//
// A "branch" here ultimately runs the existing action code (wait / sleep /
// exec / IO). That code suspends at its waiting points. Today it suspends
// through Coro (coro::Timer, coro::AsioTask, coro::CheckPoint -> Coro::yield).
// If we start a branch as an asio coroutine, a Coro yield inside it has no
// matching Coro fiber to suspend — the two fiber mechanisms do not interoperate.
//
// Therefore this executor becomes usable only AFTER the interpreter's blocking
// actions are migrated to suspend through asio (a yield_context, threaded down
// to every async op) instead of Coro. That is a project-wide change; it is not
// contained in this file. The seam keeps the *fan-out/fan-in* asio-only, but
// the *substrate the branches run on* is shared with the whole interpreter.
//
// --- Toolchain requirements -------------------------------------------------
//
//   * asio >= 1.28  — self-contained asio::spawn (no Boost.Coroutine dependency)
//   * asio >= 1.21  — asio::experimental::make_parallel_group
//   * (the project's C++17 is fine for the stackful style used below)
//
// The asio vendored in 3rd_party is 1.14, which predates both, and its
// asio::spawn still pulls in <boost/coroutine/all.hpp>. Bump asio before
// enabling this file. (On an asio without make_parallel_group, replace join()'s
// body with a manual latch: co_spawn/spawn each branch detached, count the
// completions, and wake a waiting operation when the count reaches zero —
// rethrowing the first captured exception. The public interface stays the same.)
//
// ============================================================================

#include <functional>
#include <vector>
#include <utility>
#include <exception>

#include <asio.hpp>
#include <asio/experimental/parallel_group.hpp>

struct ParallelExecutor {
	// A branch is synchronous-looking code that suspends through `yield`.
	// (In the Coro version the branch was a plain `void()` because Coro got the
	// current fiber implicitly; with asio the suspension handle is explicit.)
	using Branch = std::function<void(asio::yield_context)>;

	explicit ParallelExecutor(asio::any_io_executor executor):
		executor(std::move(executor)) {}

	// Register a branch to be run concurrently with the others.
	//
	// Nothing starts here: asio::spawn(..., asio::deferred) builds a *deferred*
	// stackful coroutine operation and hands it back without launching it.
	// join() launches them all together via make_parallel_group, so the group
	// can observe every branch and cancel the rest on the first failure.
	void spawn(Branch branch) {
		branches.push_back(asio::spawn(
			executor,
			std::move(branch),
			asio::deferred));
	}

	// Run all registered branches concurrently and wait for every one of them.
	//
	// Fail-fast: `wait_for_one_error()` makes the group emit a cancellation to
	// the still-running branches as soon as one branch finishes with an error
	// (a non-null exception_ptr). We then rethrow the first captured exception,
	// which mirrors the old coro::CoroPool::waitAll() behaviour (propagate one
	// failure, abandon the rest). When all branches succeed, nothing is thrown.
	//
	// `yield` is the *parent* coroutine's suspension handle: join() itself
	// suspends here until the group completes. This is why the call chain above
	// (run_parallel_block, visit_command, ...) must also be asio coroutines once
	// this implementation is active.
	void join(asio::yield_context yield) {
		if (branches.empty()) {
			return;
		}

		auto [completion_order, exceptions] =
			asio::experimental::make_parallel_group(std::move(branches))
				.async_wait(
					asio::experimental::wait_for_one_error(),
					yield);

		(void)completion_order;

		for (const std::exception_ptr& e: exceptions) {
			if (e) {
				std::rethrow_exception(e);
			}
		}
	}

private:
	asio::any_io_executor executor;

	// The element type is whatever asio::spawn(..., deferred) yields for our
	// branch signature; deduce it so we don't hard-code an asio-internal type.
	using DeferredBranch = decltype(asio::spawn(
		std::declval<asio::any_io_executor&>(),
		std::declval<Branch>(),
		asio::deferred));

	std::vector<DeferredBranch> branches;
};
