
#pragma once

#include <asio.hpp>
#include <optional>
#include <testo_guest_additions_protocol/GuestAdditions.hpp>
#include <hyperv/Machine.hpp>
#ifdef WIN32
#include <hyperv/AsioWin.hpp>
#else
#include <hyperv/AsioLinux.hpp>
#endif

struct HyperVGuestAdditions: GuestAdditions {
	HyperVGuestAdditions(hyperv::Machine& domain);

private:
	virtual void send_raw(const uint8_t* data, size_t size) override;
	virtual void recv_raw(uint8_t* data, size_t size) override;

	// Транспорт на нативных asio-корутинах (C++20), вызывается через мост.
	asio::awaitable<void> async_connect(const hyperv::VSocketEndpoint& endpoint);
	asio::awaitable<size_t> async_send(const uint8_t* data, size_t size);
	asio::awaitable<size_t> async_recv(uint8_t* data, size_t size);

	std::optional<hyperv::VSocketProtocol::socket> socket;
};
