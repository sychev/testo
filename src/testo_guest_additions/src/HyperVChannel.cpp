
#include "HyperVChannel.hpp"
#include <stdexcept>
#include <spdlog/spdlog.h>

HyperVChannel::HyperVChannel(HyperVChannel&& other):
	socket(std::move(other.socket))
{

}

HyperVChannel& HyperVChannel::operator=(HyperVChannel&& other) {
	socket = std::move(other.socket);
	return *this;
}

size_t HyperVChannel::read(uint8_t* data, size_t size) {
	asio::io_context& io = static_cast<asio::io_context&>(socket.get_executor().context());
	std::error_code op_ec;
	size_t n = 0;
	bool done = false;
	socket.async_read_some(asio::buffer(data, size), [&](const std::error_code& ec, size_t bytes) {
		op_ec = ec;
		n = bytes;
		done = true;
	});
	while (!done) {
		io.run_one();
	}
	if (op_ec) {
		throw std::system_error(op_ec);
	}
	return n;
}

size_t HyperVChannel::write(uint8_t* data, size_t size) {
	asio::io_context& io = static_cast<asio::io_context&>(socket.get_executor().context());
	std::error_code op_ec;
	size_t n = 0;
	bool done = false;
	socket.async_write_some(asio::buffer(data, size), [&](const std::error_code& ec, size_t bytes) {
		op_ec = ec;
		n = bytes;
		done = true;
	});
	while (!done) {
		io.run_one();
	}
	if (op_ec) {
		throw std::system_error(op_ec);
	}
	return n;
}
