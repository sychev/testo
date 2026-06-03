
#pragma once

#include <net/Stream.hpp>

namespace net {

/// Синхронный потоковый сокет (аналог coro::StreamSocket)
template <typename Protocol>
class Socket: public Stream<typename Protocol::socket> {
public:
	using BaseType = Stream<typename Protocol::socket>;
	using Endpoint = typename Protocol::endpoint;

	using BaseType::BaseType;

	void connect(const Endpoint& endpoint) {
		std::error_code error_code;
		detail::pump(this->io(), this->handle(), [&](auto on_done) {
			this->handle().async_connect(endpoint, [&, on_done](const std::error_code& ec) {
				error_code = ec;
				on_done();
			});
		});
		if (error_code) {
			throw std::system_error(error_code);
		}
	}
};

using TcpSocket = Socket<asio::ip::tcp>;

}
