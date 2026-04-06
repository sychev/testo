
#pragma once

#include "TextLine.hpp"
#include <stb/Image.hpp>
#include <vector>
#include <string>

namespace nn {

struct EasyOCRResult {
	Rect rect;
	std::string text;
};

struct EasyOCRDetector {
	static EasyOCRDetector& instance();

	EasyOCRDetector(const EasyOCRDetector&) = delete;
	EasyOCRDetector& operator=(const EasyOCRDetector&) = delete;
	~EasyOCRDetector();

	std::vector<EasyOCRResult> detect(const stb::Image<stb::RGB>* image);

private:
	EasyOCRDetector() = default;
	void ensure_process();
	void start_process();
	void stop_process();

	void write_bytes(const void* data, size_t size);
	void read_bytes(void* data, size_t size);

	int to_child_fd = -1;
	int from_child_fd = -1;
	pid_t child_pid = -1;
};

}
