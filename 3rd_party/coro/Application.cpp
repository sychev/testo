
#include "coro/Application.h"
#include "coro/detail/Engine.hpp"
#include <asio/spawn.hpp>

namespace coro {

Application::Application(std::function<void()> main): _main(std::move(main)) {}

Application::~Application() {}

void Application::run() {
	detail::current_io = &_io;
	struct ResetIo {
		~ResetIo() { detail::current_io = nullptr; }
	} reset_io;

	// Spawn the root coroutine. Its yield_context is installed as the ambient
	// one for the duration of the body (YieldScope); every coro primitive used
	// underneath picks it up via detail::yield(). The completion handler
	// rethrows anything that escaped the root, so it surfaces out of run().
	asio::spawn(_io,
		[this](asio::yield_context yield) {
			detail::YieldScope scope(yield);
			_main();
		},
		asio::bind_cancellation_slot(_cancel.slot(),
			[](std::exception_ptr error) {
				if (error) {
					std::rethrow_exception(error);
				}
			}));

	_io.run();
}

void Application::cancel() {
	_cancel.emit(asio::cancellation_type::all);
}

}
