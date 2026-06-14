
#pragma once

#include <asio.hpp>
#include <optional>
#include <testo_guest_additions_protocol/GuestAdditions.hpp>
#include <qemu/Domain.hpp>

struct QemuGuestAdditions: GuestAdditions {
	QemuGuestAdditions(vir::Domain& domain);

private:
	// Транспорт на нативных asio-корутинах (C++20).
	virtual asio::awaitable<void> send_raw(const uint8_t* data, size_t size) override;
	virtual asio::awaitable<void> recv_raw(uint8_t* data, size_t size) override;

	using Endpoint = asio::local::stream_protocol::endpoint;

	asio::awaitable<void> async_connect();

	std::optional<asio::local::stream_protocol::socket> socket;
	Endpoint endpoint;
};
