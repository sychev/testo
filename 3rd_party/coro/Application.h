
#pragma once

#include <functional>
#include <asio.hpp>

/*!
	@brief Cooperative-IO library: synchronous-looking code on top of asio.

	Same public surface as before, but the engine is now asio::spawn +
	yield_context (see coro/detail/Engine.hpp) instead of ucontext fibers.
*/
namespace coro {

/*!
	@brief Entry point of a coroutine-based application.

	@code
	int main() {
		coro::Application([&] {
			// coroutine primitives (Timer, sockets, CoroPool, ...) usable here
		}).run();
	}
	@endcode
*/
class Application {
public:
	Application(std::function<void()> main);
	~Application();

	/// Runs the event loop in the current thread until the root coroutine and
	/// all of its children finish. Rethrows an exception escaping the root.
	void run();

	/// Requests cancellation of the root coroutine and returns immediately.
	void cancel();

private:
	std::function<void()> _main;
	asio::io_context _io;
	asio::cancellation_signal _cancel;
};

}
