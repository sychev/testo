
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

asio::awaitable<size_t> HyperVChannel::read(uint8_t* data, size_t size) {
	co_return co_await socket.readSome(data, size);
}

asio::awaitable<size_t> HyperVChannel::write(uint8_t* data, size_t size) {
	co_return co_await socket.writeSome(data, size);
}
