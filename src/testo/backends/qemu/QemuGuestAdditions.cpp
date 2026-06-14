
#include "QemuGuestAdditions.hpp"
#include <testo_guest_additions_protocol/coro_asio_bridge.hpp>

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

asio::awaitable<void> QemuGuestAdditions::send_raw(const uint8_t* data, size_t size) {
	size_t n = co_await asio::async_write(*socket, asio::buffer(data, size), asio::use_awaitable);
	if (n != size) {
		throw std::runtime_error(__PRETTY_FUNCTION__);
	}
}

asio::awaitable<void> QemuGuestAdditions::recv_raw(uint8_t* data, size_t size) {
	size_t n = co_await asio::async_read(*socket, asio::buffer(data, size), asio::use_awaitable);
	if (n != size) {
		throw std::runtime_error(__PRETTY_FUNCTION__);
	}
}
