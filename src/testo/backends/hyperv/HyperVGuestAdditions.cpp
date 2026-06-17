
#include "HyperVGuestAdditions.hpp"
#include <testo_runtime/Runtime.hpp>

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

HyperVGuestAdditions::HyperVGuestAdditions(hyperv::Machine& machine): socket(g_io) {
	std::string guid_str = machine.guid();
	GUID vm_id = StringToGuid(guid_str);

	std::error_code op_ec;
	bool done = false;
	auto prev_cancel = g_cancel_current;
	g_cancel_current = [this]{ socket.cancel(); };
	socket.async_connect(hyperv::VSocketEndpoint(service_id, vm_id), [&](const std::error_code& ec) {
		op_ec = ec;
		done = true;
	});
	while (!done) {
		g_io.run_one();
	}
	g_cancel_current = prev_cancel;
	if (op_ec == asio::error::operation_aborted && g_interrupted) {
		throw Interruption();
	}
	if (op_ec) {
		throw std::system_error(op_ec);
	}
}

void HyperVGuestAdditions::send_raw(const uint8_t* data, size_t size) {
	std::error_code op_ec;
	int outstanding = 1;
	asio::steady_timer timer(g_io);
	bool timed = deadline != std::chrono::steady_clock::time_point::max();
	if (timed) {
		++outstanding;
		timer.expires_at(deadline);
		timer.async_wait([&](const std::error_code& ec) {
			--outstanding;
			if (!ec) {
				socket.cancel();
			}
		});
	}
	auto prev_cancel = g_cancel_current;
	g_cancel_current = [this]{ socket.cancel(); };
	asio::async_write(socket, asio::buffer(data, size), [&](const std::error_code& ec, size_t) {
		op_ec = ec;
		--outstanding;
		if (timed) {
			timer.cancel();
		}
	});
	while (outstanding) {
		g_io.run_one();
	}
	g_cancel_current = prev_cancel;
	if (op_ec == asio::error::operation_aborted && g_interrupted) {
		throw Interruption();
	}
	if (op_ec == asio::error::operation_aborted) {
		throw std::runtime_error("Timeout");
	}
	if (op_ec) {
		throw std::system_error(op_ec);
	}
}

void HyperVGuestAdditions::recv_raw(uint8_t* data, size_t size) {
	std::error_code op_ec;
	int outstanding = 1;
	asio::steady_timer timer(g_io);
	bool timed = deadline != std::chrono::steady_clock::time_point::max();
	if (timed) {
		++outstanding;
		timer.expires_at(deadline);
		timer.async_wait([&](const std::error_code& ec) {
			--outstanding;
			if (!ec) {
				socket.cancel();
			}
		});
	}
	auto prev_cancel = g_cancel_current;
	g_cancel_current = [this]{ socket.cancel(); };
	asio::async_read(socket, asio::buffer(data, size), [&](const std::error_code& ec, size_t) {
		op_ec = ec;
		--outstanding;
		if (timed) {
			timer.cancel();
		}
	});
	while (outstanding) {
		g_io.run_one();
	}
	g_cancel_current = prev_cancel;
	if (op_ec == asio::error::operation_aborted && g_interrupted) {
		throw Interruption();
	}
	if (op_ec == asio::error::operation_aborted) {
		throw std::runtime_error("Timeout");
	}
	if (op_ec) {
		throw std::system_error(op_ec);
	}
}
