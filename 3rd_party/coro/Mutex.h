
#pragma once

#include "coro/AsioTask.h"
#include "coro/IoService.h"
#include <asio/experimental/concurrent_channel.hpp>
#include <mutex>

namespace coro {

/*!
	@brief Монопольный доступ корутины к ресурсу. Используйте вместе с std::lock_guard

	Реализован как канал ёмкостью 1, в котором лежит единственный "токен владения". lock()
	забирает токен (или ждёт его), unlock() возвращает. concurrent_channel потокобезопасен,
	поэтому в MT-модели мьютексом можно пользоваться из корутин на разных потоках.
*/
class Mutex {
public:
	Mutex(): _channel(IoService::current()->_impl, 1) {
		_channel.try_send(std::error_code{});   // изначально свободен
	}

	/// Захват мьютекса (ждёт освобождения; может быть прерван отменой корутины)
	void lock() {
		awaitOp([&](auto&& token) {
			_channel.async_receive(std::forward<decltype(token)>(token));
		});
	}

	/// Освобождение мьютекса (неблокирующе, безопасно с любого потока)
	void unlock() {
		_channel.try_send(std::error_code{});
	}

private:
	asio::experimental::concurrent_channel<void(std::error_code)> _channel;
};

}
