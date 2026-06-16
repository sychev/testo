
#pragma once

#include <asio.hpp>
#include <functional>
#include <stdexcept>

namespace coro {

/*!
	@brief Wrapper вокруг общего asio::io_context

	В MT-модели io_context один на всё приложение и крутится на нескольких потоках
	(см. Application). current() возвращает этот единственный экземпляр; он выставляется до
	старта рабочих потоков и далее только читается, поэтому потокобезопасен без блокировок.
*/
class IoService {
public:
	static IoService* current();
	static void setCurrent(IoService* ioService);

	template <typename T>
	void post(T&& t) {
		asio::post(_impl, std::forward<T>(t));
	}

	template <typename T>
	void dispatch(T&& t) {
		asio::dispatch(_impl, std::forward<T>(t));
	}

	asio::io_context _impl;
};

}
