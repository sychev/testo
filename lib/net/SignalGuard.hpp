
#pragma once

#include <asio.hpp>
#include <functional>
#include <thread>
#include <initializer_list>

namespace net {

/*!
	@brief Обработчик сигналов (аналог coro::SignalSet в связке с корневой корутиной)

	Поднимает фоновый поток с собственным io_context и asio::signal_set.
	Каждый пойманный сигнал передаётся в handler (в фоновом потоке), после
	чего ожидание автоматически перевзводится. Обычно handler выставляет
	net::request_interrupt().
*/
class SignalGuard {
public:
	SignalGuard(std::initializer_list<int> signals, std::function<void(int)> handler);
	~SignalGuard();

	SignalGuard(const SignalGuard&) = delete;
	SignalGuard& operator=(const SignalGuard&) = delete;

private:
	void arm();

	asio::io_context _io;
	asio::signal_set _signals;
	std::function<void(int)> _handler;
	std::thread _thread;
};

}
