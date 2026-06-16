
#pragma once

#include "coro/Stream.h"
#include <asio.hpp>
#include <utility>

namespace coro {

template <typename Protocol>
class StreamSocket: public Stream<typename Protocol::socket> {
public:
	using BaseType = Stream<typename Protocol::socket>;
	using BaseType::_handle;

	StreamSocket(): BaseType(typename Protocol::socket(current_executor())) {}

	explicit StreamSocket(const typename Protocol::endpoint& endpoint)
		: BaseType(typename Protocol::socket(current_executor()))
	{
		_handle.connect(endpoint);
	}

	StreamSocket(typename Protocol::socket socket): BaseType(std::move(socket)) {}

	StreamSocket(StreamSocket&& other) = default;
	StreamSocket& operator=(StreamSocket&& other) = default;

	asio::awaitable<void> connect(const typename Protocol::endpoint& endpoint) {
		co_await _handle.async_connect(endpoint, asio::use_awaitable);
	}
};

using TcpSocket = StreamSocket<asio::ip::tcp>;

}
