#pragma once

#include <functional>
#include <coro/CoroPool.h>

// ====================================================================
// Concurrency seam for the `parallel` block.
//
// This is the single place that knows HOW branches are scheduled. It
// exposes a tiny fan-out / fan-in primitive:
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
// It is built on coro::CoroPool. After the Coro->asio migration, CoroPool
// is itself implemented on asio::spawn + yield_context (see
// 3rd_party/coro/CoroPool.cpp), so this seam runs on asio coroutines while
// keeping the single-threaded cooperative model: branches truly overlap on
// their blocking waits, with no OS threads and no data races.
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
