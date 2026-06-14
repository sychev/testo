
#include "QemuGuestAdditions.hpp"
#include "../../coro_asio_bridge.hpp"

QemuGuestAdditions::QemuGuestAdditions(vir::Domain& domain) {
	auto config = domain.dump_xml();

	auto devices = config.first_child().child("devices");

	std::string path;

	for (auto channel = devices.child("channel"); channel; channel = channel.next_sibling("channel")) {
		if (std::string(channel.child("target").attribute("name").value()) == "negotiator.0") {
			path = std::string(channel.child("source").attribute("path").value());
			break;
		}
	}

	if (!path.length()) {
		throw std::runtime_error("Can't find negotiator channel unix file");
	}

	endpoint = Endpoint(path);
	coro::await(async_connect());
}

asio::awaitable<void> QemuGuestAdditions::async_connect() {
	socket.emplace(co_await asio::this_coro::executor);
	co_await socket->async_connect(endpoint, asio::use_awaitable);
}

asio::awaitable<size_t> QemuGuestAdditions::async_send(const uint8_t* data, size_t size) {
	co_return co_await asio::async_write(*socket, asio::buffer(data, size), asio::use_awaitable);
}

asio::awaitable<size_t> QemuGuestAdditions::async_recv(uint8_t* data, size_t size) {
	co_return co_await asio::async_read(*socket, asio::buffer(data, size), asio::use_awaitable);
}

void QemuGuestAdditions::send_raw(const uint8_t* data, size_t size) {
	size_t n = coro::await(async_send(data, size));
	if (n != size) {
		throw std::runtime_error(__PRETTY_FUNCTION__);
	}
}

void QemuGuestAdditions::recv_raw(uint8_t* data, size_t size) {
	size_t n = coro::await(async_recv(data, size));
	if (n != size) {
		throw std::runtime_error(__PRETTY_FUNCTION__);
	}
}
