
#pragma once

#include "coro/detail/Engine.hpp"

namespace coro {

/// Wrapper around an asio datagram socket (drop-in for old coro::DatagramSocket).
template <typename Protocol>
class DatagramSocket {
public:
	DatagramSocket(Protocol protocol = Protocol()): _handle(detail::io(), protocol) {}

	DatagramSocket(const typename Protocol::endpoint& endpoint): _handle(detail::io(), endpoint.protocol()) {
		asio::socket_base::reuse_address option(true);
		_handle.set_option(option);
		_handle.bind(endpoint);
	}

	template <typename ...T>
	size_t receiveFrom(typename Protocol::endpoint& endpoint, T&&... t) {
		auto buffer = asio::buffer(std::forward<T>(t)...);
		return detail::await([&](auto token) {
			return _handle.async_receive_from(buffer, endpoint, token);
		});
	}

	template <typename ...T>
	size_t sendTo(const typename Protocol::endpoint& endpoint, T&&... t) {
		auto buffer = asio::buffer(std::forward<T>(t)...);
		return detail::await([&](auto token) {
			return _handle.async_send_to(buffer, endpoint, token);
		});
	}

	typename Protocol::socket& handle() { return _handle; }

protected:
	typename Protocol::socket _handle;
};

}
