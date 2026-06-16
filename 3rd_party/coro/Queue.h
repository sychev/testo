
#pragma once

#include "coro/AsioTask.h"
#include "coro/IoService.h"
#include <asio/experimental/concurrent_channel.hpp>
#include <limits>

namespace coro {

/*!
	@brief Очередь-канал между корутинами

	Реализована поверх concurrent_channel, поэтому потокобезопасна: producer и consumer могут
	жить в корутинах на разных потоках. pop() ожидает элемент (прерывается отменой корутины),
	push() кладёт элемент неблокирующе.
*/
template <typename T>
class Queue {
public:
	Queue(): _channel(IoService::current()->_impl, std::numeric_limits<std::size_t>::max()) {}

	/// Получить элемент (ждёт, если очередь пуста; может быть прерван отменой)
	T pop() {
		return awaitValue<T>([&](auto&& token) {
			return _channel.async_receive(std::forward<decltype(token)>(token));
		});
	}

	/// Положить элемент (неблокирующе, безопасно с любого потока)
	template <typename U>
	void push(U&& u) {
		_channel.try_send(std::error_code{}, std::forward<U>(u));
	}

private:
	asio::experimental::concurrent_channel<void(std::error_code, T)> _channel;
};

}
