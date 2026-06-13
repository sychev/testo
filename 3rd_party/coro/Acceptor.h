
#pragma once

#include "coro/detail/Engine.hpp"
#include "coro/CoroPool.h"

namespace coro {

/// Wrapper around asio's acceptor (drop-in for old coro::Acceptor).
template <typename Protocol>
class Acceptor {
public:
	Acceptor(const typename Protocol::endpoint& endpoint): _handle(detail::io()) {
		_handle.open(endpoint.protocol());
		asio::socket_base::reuse_address option(true);
		_handle.set_option(option);
		_handle.bind(endpoint);
		_handle.listen();
	}

	typename Protocol::socket accept() {
		typename Protocol::socket socket(detail::io());
		detail::await([&](auto token) {
			return _handle.async_accept(socket, token);
		});
		return socket;
	}

	/// Accepts connections in a loop, running each handler in its own coroutine.
	void run(std::function<void(typename Protocol::socket)> callback) {
		CoroPool pool;
		while (true) {
			auto socket = accept();
			pool.exec([&, socket = std::move(socket)]() mutable {
				callback(std::move(socket));
			});
		}
	}

	typename Protocol::acceptor& handle() { return _handle; }

protected:
	typename Protocol::acceptor _handle;
};

using TcpAcceptor = Acceptor<asio::ip::tcp>;

}
