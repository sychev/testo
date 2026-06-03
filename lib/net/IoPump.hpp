
#pragma once

#include <asio.hpp>
#include <chrono>

#include <net/Cancel.hpp>
#include <net/Deadline.hpp>

namespace net::detail {

constexpr auto io_quantum = std::chrono::milliseconds(20);

/*!
	@brief Синхронно дожидается завершения асинхронной операции asio

	Заменяет связку coro::AsioTask + фибер. Крутит io_context маленькими
	квантами; между ними проверяет запрос отмены и действующий дедлайн.
	Если сработало — отменяет операцию на handle, дожидается её завершения
	(чтобы корректно отработали захваченные по ссылке колбэки) и бросает
	Interruption / TimeoutError.

	@param io       io_context, которому принадлежит handle
	@param handle   объект asio с методом cancel() (socket / acceptor / timer / ...)
	@param initiate функция, запускающая async-операцию; ей передаётся
	                продолжение on_done, которое нужно вызвать из колбэка asio
*/
template <typename Handle, typename Initiate>
void pump(asio::io_context& io, Handle& handle, Initiate&& initiate) {
	io.restart();

	bool done = false;
	initiate([&done] { done = true; });

	while (!done) {
		io.run_one_for(io_quantum);
		if (done) {
			break;
		}
		if (interrupt_requested()) {
			try { handle.cancel(); } catch (...) {}
			io.run();
			throw Interruption{};
		}
		if (Deadline::expired()) {
			try { handle.cancel(); } catch (...) {}
			io.run();
			throw TimeoutError{};
		}
	}
}

}
