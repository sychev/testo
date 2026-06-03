
#pragma once

#include <net/Socket.hpp>

namespace net {

/*!
	@brief Синхронный приёмник входящих соединений (аналог coro::Acceptor)

	accept() блокирует (но прерываем по Ctrl-C/дедлайну) и возвращает Socket
	с собственным io_context — его можно обслуживать в отдельном потоке.
*/
template <typename Protocol>
class Acceptor {
public:
	using Endpoint = typename Protocol::endpoint;

	Acceptor(const Endpoint& endpoint)
		: _io(std::make_unique<asio::io_context>())
		, _handle(*_io)
	{
		_handle.open(endpoint.protocol());
		asio::socket_base::reuse_address option(true);
		_handle.set_option(option);
		_handle.bind(endpoint);
		_handle.listen();
	}

	Acceptor(Acceptor&&) = default;
	Acceptor& operator=(Acceptor&&) = default;

	Socket<Protocol> accept() {
		Socket<Protocol> peer;
		std::error_code error_code;
		detail::pump(*_io, _handle, [&](auto on_done) {
			_handle.async_accept(peer.handle(), [&, on_done](const std::error_code& ec) {
				error_code = ec;
				on_done();
			});
		});
		if (error_code) {
			throw std::system_error(error_code);
		}
		return peer;
	}

	typename Protocol::acceptor& handle() {
		return _handle;
	}

private:
	std::unique_ptr<asio::io_context> _io;
	typename Protocol::acceptor _handle;
};

using TcpAcceptor = Acceptor<asio::ip::tcp>;

}
