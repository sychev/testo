#pragma once

#include <functional>
#include <coro/CoroPool.h>

// ====================================================================
// Concurrency seam for the `parallel` block.
//
// This is the ONLY place that depends on the coroutine library (Coro).
// It exposes a tiny fan-out / fan-in primitive:
//
//     ParallelExecutor executor;
//     executor.spawn(branch1);
//     executor.spawn(branch2);
//     executor.join();   // waits for all branches; rethrows the first
//                        // failure and cancels the remaining branches
//                        // (fail-fast)
//
// The rest of the parallel-block machinery (grammar, semantic checks,
// ParallelBranchInterpreter) does not know how branches are scheduled —
// it only talks to this interface.
//
// To move off Coro while keeping asio, reimplement just this struct on
// top of asio::spawn + asio::experimental::make_parallel_group: have
// `spawn` collect one deferred operation per branch and `join` run
//
//     make_parallel_group(branches)
//         .async_wait(wait_for_one_error(), yield);
//
// Nothing else in the codebase needs to change.
// ====================================================================

struct ParallelExecutor {
	// Schedule a branch to run concurrently with the others.
	void spawn(std::function<void()> branch) {
		pool.exec(std::move(branch));
	}

	// Wait for every spawned branch to finish. If a branch threw, the
	// exception is rethrown here; the remaining branches are then cancelled
	// while the pool unwinds (see ~CoroPool).
	void join() {
		pool.waitAll();
	}

private:
	coro::CoroPool pool;
};
