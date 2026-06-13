
#pragma once

#include <functional>
#include <memory>

namespace coro {

/*!
	@brief Hierarchical management of child coroutines (drop-in for the old one).

	exec() spawns a child coroutine that runs concurrently right away;
	waitAll() suspends the parent until every child has finished; the destructor
	cancels whatever is still running and waits for it.

	Re-implemented on asio::spawn:
	  * each child is an asio stackful coroutine on the current executor;
	  * each child carries a cancellation_signal so it can be cancelled;
	  * completion is tracked with a counter; the parent waits on a steady_timer
	    "latch" that a finishing child cancels to wake the parent;
	  * fail-fast: the first child to throw stores its exception and cancels the
	    siblings, exactly like the old CoroPool.
*/
class CoroPool {
public:
	CoroPool();
	~CoroPool();

	CoroPool(const CoroPool&) = delete;
	CoroPool& operator=(const CoroPool&) = delete;

	/// Spawn a new child coroutine on the current executor (starts immediately).
	void exec(std::function<void()> routine);
	/// Suspend the parent until all children finish. Rethrows the first failure
	/// unless noThrow is set.
	void waitAll(bool noThrow = false);
	/// Cancel all still-running children.
	void cancelAll();

private:
	struct Impl;
	std::shared_ptr<Impl> _impl;
};

}
