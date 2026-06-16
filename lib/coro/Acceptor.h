
#pragma once

#include "coro/Runtime.h"
#include "coro/StreamSocket.h"
#include <asio.hpp>
#include <functional>
#include <utility>

namespace coro {

/// Обёртка вокруг asio acceptor-а.
template <typename Protocol>
class Acceptor {
public:
	explicit Acceptor(const typename Protocol::endpoint& endpoint): _handle(current_executor())
	{
		_handle.open(endpoint.protocol());
		asio::socket_base::reuse_address option(true);
		_handle.set_option(option);
		_handle.bind(endpoint);
		_handle.listen();
	}

	asio::awaitable<typename Protocol::socket> accept() {
		co_return co_await _handle.async_accept(asio::use_awaitable);
	}

	/*!
		@brief В цикле принимает подключения и запускает их обработчики
		       в отдельных корутинах.
	*/
	asio::awaitable<void> run(std::function<asio::awaitable<void>(typename Protocol::socket)> callback) {
		auto executor = co_await asio::this_coro::executor;
		for (;;) {
			auto socket = co_await accept();
			asio::co_spawn(executor, callback(std::move(socket)), asio::detached);
		}
	}

	typename Protocol::acceptor& handle() {
		return _handle;
	}

protected:
	typename Protocol::acceptor _handle;
};

using TcpAcceptor = Acceptor<asio::ip::tcp>;

}
