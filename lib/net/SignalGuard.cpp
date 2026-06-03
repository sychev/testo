
#include <net/SignalGuard.hpp>

namespace net {

SignalGuard::SignalGuard(std::initializer_list<int> signals, std::function<void(int)> handler)
	: _signals(_io)
	, _handler(std::move(handler))
{
	for (int signal: signals) {
		_signals.add(signal);
	}
	arm();
	_thread = std::thread([this] { _io.run(); });
}

SignalGuard::~SignalGuard() {
	_io.stop();
	if (_thread.joinable()) {
		_thread.join();
	}
}

void SignalGuard::arm() {
	_signals.async_wait([this](const std::error_code& error_code, int signal) {
		if (error_code) {
			return; // отменено при завершении работы
		}
		_handler(signal);
		arm();
	});
}

}
