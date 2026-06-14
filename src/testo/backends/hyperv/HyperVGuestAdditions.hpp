
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
	// Транспорт на нативных asio-корутинах (C++20).
	virtual asio::awaitable<void> send_raw(const uint8_t* data, size_t size) override;
	virtual asio::awaitable<void> recv_raw(uint8_t* data, size_t size) override;

	asio::awaitable<void> async_connect(const hyperv::VSocketEndpoint& endpoint);

	std::optional<hyperv::VSocketProtocol::socket> socket;
};
