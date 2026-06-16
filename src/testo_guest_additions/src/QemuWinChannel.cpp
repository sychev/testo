
#include "QemuWinChannel.hpp"
#include <winapi/Functions.hpp>
#include <stdexcept>
#include "QemuWinChannelExtra.hpp"

QemuWinChannel::QemuWinChannel(asio::io_context& io):
	stream(io)
{
	std::string device_path = GetVirtioDevicePath();
	HANDLE handle = CreateFile(winapi::utf8_to_utf16(device_path).c_str(),
		GENERIC_WRITE | GENERIC_READ,
		0,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,
		NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		throw std::runtime_error("CreateFile failed");
	}
	stream.assign(handle);
	info_buf.resize(sizeof(VIRTIO_PORT_INFO));
}

QemuWinChannel::~QemuWinChannel() {
}

QemuWinChannel& QemuWinChannel::operator=(QemuWinChannel&& other) {
	std::swap(stream, other.stream);
	std::swap(info_buf, other.info_buf);
	return *this;
}

size_t QemuWinChannel::read(uint8_t* data, size_t size) {
	PVIRTIO_PORT_INFO info = GetVirtioDeviceInformation(stream.native_handle(), info_buf);
	if (!info->HostConnected) {
		return 0;
	}
	asio::io_context& io = static_cast<asio::io_context&>(stream.get_executor().context());
	std::error_code op_ec;
	size_t n = 0;
	bool done = false;
	stream.async_read_some(asio::buffer(data, size), [&](const std::error_code& ec, size_t bytes) {
		op_ec = ec;
		n = bytes;
		done = true;
	});
	while (!done) {
		io.run_one();
	}
	if (op_ec) {
		throw std::system_error(op_ec);
	}
	return n;
}

size_t QemuWinChannel::write(uint8_t* data, size_t size) {
	asio::io_context& io = static_cast<asio::io_context&>(stream.get_executor().context());
	std::error_code op_ec;
	size_t n = 0;
	bool done = false;
	asio::async_write(stream, asio::buffer(data, size), [&](const std::error_code& ec, size_t bytes) {
		op_ec = ec;
		n = bytes;
		done = true;
	});
	while (!done) {
		io.run_one();
	}
	if (op_ec) {
		throw std::system_error(op_ec);
	}
	return n;
}

void QemuWinChannel::close() {
	CloseHandle(stream.native_handle());
}
