
#pragma once

#include <asio.hpp>
#include <optional>
#include <testo_guest_additions_protocol/GuestAdditions.hpp>
#include <qemu/Domain.hpp>

struct QemuGuestAdditions: GuestAdditions {
	QemuGuestAdditions(vir::Domain& domain);

private:
	virtual void send_raw(const uint8_t* data, size_t size) override;
	virtual void recv_raw(uint8_t* data, size_t size) override;

	using Endpoint = asio::local::stream_protocol::endpoint;

	// Транспорт на нативных asio-корутинах (C++20), вызывается через мост.
	asio::awaitable<void> async_connect();
	asio::awaitable<size_t> async_send(const uint8_t* data, size_t size);
	asio::awaitable<size_t> async_recv(uint8_t* data, size_t size);

	std::optional<asio::local::stream_protocol::socket> socket;
	Endpoint endpoint;
};
