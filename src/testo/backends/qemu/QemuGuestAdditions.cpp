
#include "QemuGuestAdditions.hpp"
#include "../../Runtime.hpp"

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

	std::error_code op_ec;
	bool done = false;
	auto prev_cancel = g_cancel_current;
	g_cancel_current = [this]{ socket.cancel(); };
	socket.async_connect(endpoint, [&](const std::error_code& ec) {
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

void QemuGuestAdditions::send_raw(const uint8_t* data, size_t size) {
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

void QemuGuestAdditions::recv_raw(uint8_t* data, size_t size) {
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
