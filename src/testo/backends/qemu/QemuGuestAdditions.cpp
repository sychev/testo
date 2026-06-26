
#include "QemuGuestAdditions.hpp"
#include <testo_runtime/Runtime.hpp>

QemuGuestAdditions::QemuGuestAdditions(vir::Domain& domain): socket(g_io) {
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

	auto ec = await_io(socket, [&](auto h){ socket.async_connect(endpoint, h); });
	if (ec) {
		throw std::system_error(ec);
	}
}

void QemuGuestAdditions::send_raw(const uint8_t* data, size_t size) {
	auto ec = await_io(socket,
		[&](auto h){ asio::async_write(socket, asio::buffer(data, size), h); },
		deadline);
	if (ec == asio::error::operation_aborted) {
		throw std::runtime_error("Timeout was triggered");
	}
	if (ec) {
		throw std::system_error(ec);
	}
}

void QemuGuestAdditions::recv_raw(uint8_t* data, size_t size) {
	auto ec = await_io(socket,
		[&](auto h){ asio::async_read(socket, asio::buffer(data, size), h); },
		deadline);
	if (ec == asio::error::operation_aborted) {
		throw std::runtime_error("Timeout was triggered");
	}
	if (ec) {
		throw std::system_error(ec);
	}
}
