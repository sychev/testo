
#pragma once

#include "coro/Stream.h"

namespace coro {

template <typename Protocol>
class StreamSocket: public Stream<typename Protocol::socket> {
public:
	using BaseType = Stream<typename Protocol::socket>;
	using BaseType::BaseType;
	using BaseType::operator=;
	using BaseType::_handle;

	StreamSocket(): BaseType(typename Protocol::socket(detail::io())) {}

	StreamSocket(const typename Protocol::endpoint& endpoint)
		: BaseType(typename Protocol::socket(detail::io(), endpoint)) {}

	void connect(const typename Protocol::endpoint& endpoint) {
		detail::await([&](auto token) {
			return _handle.async_connect(endpoint, token);
		});
	}
};

using TcpSocket = StreamSocket<asio::ip::tcp>;

}
