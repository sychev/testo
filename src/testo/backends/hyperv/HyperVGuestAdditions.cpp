
#include "HyperVGuestAdditions.hpp"
#include <testo_guest_additions_protocol/coro_asio_bridge.hpp>

#define HYPERV_PORT 1234
DEFINE_GUID(service_id, HYPERV_PORT, 0xfacb, 0x11e6, 0xbd, 0x58, 0x64, 0x00, 0x6a, 0x79, 0x86, 0xd3);

GUID StringToGuid(const std::string& str)
{
	GUID guid;
	sscanf(str.c_str(),
	       "%8x-%4hx-%4hx-%2hhx%2hhx-%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
	       &guid.Data1, &guid.Data2, &guid.Data3,
	       &guid.Data4[0], &guid.Data4[1], &guid.Data4[2], &guid.Data4[3],
	       &guid.Data4[4], &guid.Data4[5], &guid.Data4[6], &guid.Data4[7] );
	return guid;
}

std::string GuidToString(GUID guid)
{
	char guid_cstr[39];
	snprintf(guid_cstr, sizeof(guid_cstr),
	         "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	         guid.Data1, guid.Data2, guid.Data3,
	         guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
	         guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);

	return std::string(guid_cstr);
}

HyperVGuestAdditions::HyperVGuestAdditions(hyperv::Machine& machine) {
	std::string guid_str = machine.guid();
	GUID vm_id = StringToGuid(guid_str);
	coro::await(async_connect(hyperv::VSocketEndpoint(service_id, vm_id)));
}

asio::awaitable<void> HyperVGuestAdditions::async_connect(const hyperv::VSocketEndpoint& endpoint) {
	socket.emplace(co_await asio::this_coro::executor);
	co_await socket->async_connect(endpoint, asio::use_awaitable);
}

asio::awaitable<void> HyperVGuestAdditions::send_raw(const uint8_t* data, size_t size) {
	size_t n = co_await asio::async_write(*socket, asio::buffer(data, size), asio::use_awaitable);
	if (n != size) {
		throw std::runtime_error(__PRETTY_FUNCTION__);
	}
}

asio::awaitable<void> HyperVGuestAdditions::recv_raw(uint8_t* data, size_t size) {
	size_t n = co_await asio::async_read(*socket, asio::buffer(data, size), asio::use_awaitable);
	if (n != size) {
		throw std::runtime_error(__PRETTY_FUNCTION__);
	}
}
