
#pragma once

#include "Channel.hpp"
#include <net/Stream.hpp>

struct QemuWinChannel: Channel {
	QemuWinChannel();
	~QemuWinChannel();

	QemuWinChannel(QemuWinChannel&& other);
	QemuWinChannel& operator=(QemuWinChannel&& other);

	size_t read(uint8_t* data, size_t size) override;
	size_t write(uint8_t* data, size_t size) override;

	void close();

	std::vector<uint8_t> info_buf;
	net::Stream<asio::windows::stream_handle> stream;
};
