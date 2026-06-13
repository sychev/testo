
#pragma once

// ============================================================================
// coro engine — asio::spawn + yield_context substrate
// ============================================================================
//
// This header replaces the old hand-rolled ucontext fiber engine (FiberLinux /
// FiberWindows / Coro.cpp). The public coro:: API (CheckPoint, Timer, Timeout,
// CoroPool, StreamSocket, ...) is kept intact and is now implemented on top of
// asio's own stackful coroutines (asio::spawn + asio::yield_context).
//
// THE BRIDGE (why this file exists)
// ---------------------------------
// The old code relied on Coro::current(): every blocking primitive could reach
// "the coroutine I am running in" implicitly. asio::spawn instead hands a
// yield_context to the coroutine body, and that token must be passed to each
// async operation. To keep the rest of the codebase unchanged (the interpreter
// and backends call Timer/CheckPoint/sockets without passing a yield), we keep
// an *ambient* yield_context in a thread_local and a single choke point —
// detail::await() — through which every suspension goes.
//
// INVARIANT
// ---------
// `current_yield` always points at the yield_context of the coroutine that is
// *currently running* on this thread. It is:
//   * set when a coroutine body starts          (YieldScope, in spawn())
//   * restored after every suspension completes  (detail::await())
// Between two suspension points exactly one coroutine runs (cooperative, single
// thread), so reads of current_yield are always correct. While a coroutine is
// suspended the value may belong to another coroutine — that is fine, because
// nobody reads it until some coroutine is running again and about to suspend,
// at which point await()/YieldScope have already put the right value back.
//
// This is exactly what Coro::current() did, re-expressed on asio::spawn.
//
// Requires: asio >= 1.30 (asio::cancel_after), C++20.
// ============================================================================

#include <cassert>
#include <chrono>
#include <optional>
#include <vector>
#include <system_error>

#include <asio.hpp>
#include <asio/spawn.hpp>

namespace coro {

using Clock = std::chrono::steady_clock;

// Cancellation marker. Deliberately NOT derived from std::exception so that a
// `catch (const std::exception&)` somewhere in the interpreter cannot swallow
// it: a cancelled coroutine must unwind all the way to the top. This preserves
// the exact contract of the old coro::CancelError.
struct CancelError {};

// Thrown when the nearest active coro::Timeout deadline elapses while a
// coroutine is suspended inside detail::await().
class TimeoutError: public std::runtime_error {
public:
	TimeoutError(): std::runtime_error("Timeout was triggered") {}
};

namespace detail {

// ---- ambient io_context / executor ----------------------------------------
// Set by Application::run() for the lifetime of the event loop.
inline thread_local asio::io_context* current_io = nullptr;

inline asio::io_context& io() {
	assert(current_io && "coro primitive used outside coro::Application::run()");
	return *current_io;
}

inline asio::any_io_executor executor() {
	return io().get_executor();
}

// ---- ambient yield_context (the bridge) ------------------------------------
inline thread_local asio::yield_context* current_yield = nullptr;

inline asio::yield_context& yield() {
	assert(current_yield && "coro primitive used outside of a coroutine");
	return *current_yield;
}

// Makes `y` the current yield while a coroutine body runs. Restored on exit so
// nested coroutines (a child finishing, a parent resuming) leave the parent's
// value in place.
struct YieldScope {
	asio::yield_context* prev;
	explicit YieldScope(asio::yield_context& y): prev(current_yield) { current_yield = &y; }
	~YieldScope() { current_yield = prev; }
	YieldScope(const YieldScope&) = delete;
	YieldScope& operator=(const YieldScope&) = delete;
};

// ---- deadlines (replacement for coro::Timeout) -----------------------------
// A thread-local stack of absolute deadlines. The nearest (smallest) active
// deadline bounds every await() below it. See Timeout.h.
inline thread_local std::vector<Clock::time_point> deadline_stack;

inline std::optional<Clock::time_point> current_deadline() {
	if (deadline_stack.empty()) {
		return std::nullopt;
	}
	return deadline_stack.back();
}

// ---- the single suspension choke point -------------------------------------
//
// `init` receives the (possibly deadline-bound) completion token and must start
// exactly one async operation with it, returning that operation's result.
//
// Responsibilities:
//   * pass the current coroutine's yield as the completion token,
//   * enforce the nearest active deadline via asio::cancel_after,
//   * restore current_yield after the operation completes,
//   * translate a deadline-triggered cancellation into coro::TimeoutError.
//
// On asio cancellation an async op completes with std::system_error carrying
// asio::error::operation_aborted. We translate that into:
//   * TimeoutError  — if the nearest deadline has elapsed (a coro::Timeout fired);
//   * CancelError   — otherwise (CoroPool::cancelAll / Application::cancel).
template <class Init>
decltype(auto) await(Init&& init) {
	asio::yield_context y = yield();

	// Restore the ambient yield after the operation returns/throws: while we
	// were suspended other coroutines ran and overwrote current_yield.
	asio::yield_context* saved = current_yield;
	struct Restore {
		asio::yield_context** slot;
		asio::yield_context* value;
		~Restore() { *slot = value; }
	} restore{&current_yield, saved};

	auto deadline = current_deadline();

	auto run = [&](auto token) -> decltype(auto) {
		try {
			return init(token);
		} catch (const std::system_error& error) {
			if (error.code() == asio::error::operation_aborted) {
				if (deadline && Clock::now() >= *deadline) {
					throw TimeoutError{};
				}
				throw CancelError{};
			}
			throw;
		}
	};

	if (!deadline) {
		return run(y);
	}

	auto remaining = *deadline - Clock::now();
	if (remaining <= Clock::duration::zero()) {
		throw TimeoutError{};
	}
	// asio::cancel_after auto-cancels the operation once the deadline passes
	// (asio >= 1.30). The resulting operation_aborted is mapped above.
	return run(asio::cancel_after(remaining, y));
}

// Suspend on an async operation WITHOUT deadline enforcement or cancellation
// translation, swallowing operation_aborted. Used by CoroPool's completion
// latch (a timer that is "fired" by cancelling it). Like await(), it restores
// the ambient yield after the operation completes.
template <class Init>
void suspend_quietly(Init&& init) {
	asio::yield_context* saved = current_yield;
	struct Restore {
		asio::yield_context** slot;
		asio::yield_context* value;
		~Restore() { *slot = value; }
	} restore{&current_yield, saved};

	asio::error_code ec;
	// yield[ec] reports the completion error code instead of throwing.
	init(yield()[ec]);
}

} // namespace detail
} // namespace coro
