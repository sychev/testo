
#include "EasyOCRDetector.hpp"
#include "BackendConfig.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <stb_image_write.h>

#include <stdexcept>
#include <cstring>

#ifdef __linux__
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#endif

namespace nn {

static const char EASYOCR_SCRIPT[] = R"PYTHON(
import sys
import struct
import json
import easyocr
import numpy as np
from PIL import Image
import io

def main():
    langs = json.loads(sys.argv[1])
    use_gpu = sys.argv[2] == "1"

    reader = easyocr.Reader(langs, gpu=use_gpu, verbose=False)

    sys.stderr.write("EasyOCR initialized\n")
    sys.stderr.flush()

    while True:
        header = sys.stdin.buffer.read(4)
        if len(header) < 4:
            break

        size = struct.unpack('<I', header)[0]
        png_data = sys.stdin.buffer.read(size)
        if len(png_data) < size:
            break

        img = Image.open(io.BytesIO(png_data))
        img_np = np.array(img)

        results = reader.readtext(img_np)

        output = []
        for (bbox, text, confidence) in results:
            xs = [p[0] for p in bbox]
            ys = [p[1] for p in bbox]
            output.append({
                "left": int(min(xs)),
                "top": int(min(ys)),
                "right": int(max(xs)),
                "bottom": int(max(ys)),
                "text": text
            })

        json_bytes = json.dumps(output).encode('utf-8')
        sys.stdout.buffer.write(struct.pack('<I', len(json_bytes)))
        sys.stdout.buffer.write(json_bytes)
        sys.stdout.buffer.flush()

if __name__ == "__main__":
    main()
)PYTHON";

EasyOCRDetector& EasyOCRDetector::instance() {
	static EasyOCRDetector instance;
	return instance;
}

EasyOCRDetector::~EasyOCRDetector() {
	stop_process();
}

void EasyOCRDetector::stop_process() {
#ifdef __linux__
	if (child_pid > 0) {
		kill(child_pid, SIGTERM);
		waitpid(child_pid, nullptr, 0);
		child_pid = -1;
	}
	if (to_child_fd >= 0) {
		close(to_child_fd);
		to_child_fd = -1;
	}
	if (from_child_fd >= 0) {
		close(from_child_fd);
		from_child_fd = -1;
	}
#endif
}

void EasyOCRDetector::start_process() {
#ifdef __linux__
	int pipe_to_child[2];
	int pipe_from_child[2];

	if (pipe(pipe_to_child) != 0) {
		throw std::runtime_error("EasyOCRDetector: failed to create pipe_to_child");
	}
	if (pipe(pipe_from_child) != 0) {
		close(pipe_to_child[0]);
		close(pipe_to_child[1]);
		throw std::runtime_error("EasyOCRDetector: failed to create pipe_from_child");
	}

	auto& config = BackendConfig::instance();
	nlohmann::json langs_json = config.easyocr_languages;
	std::string langs_str = langs_json.dump();
	std::string gpu_str = config.easyocr_gpu ? "1" : "0";

	pid_t pid = fork();
	if (pid < 0) {
		close(pipe_to_child[0]);
		close(pipe_to_child[1]);
		close(pipe_from_child[0]);
		close(pipe_from_child[1]);
		throw std::runtime_error("EasyOCRDetector: fork failed");
	}

	if (pid == 0) {
		// Child process
		close(pipe_to_child[1]);
		close(pipe_from_child[0]);

		dup2(pipe_to_child[0], STDIN_FILENO);
		dup2(pipe_from_child[1], STDOUT_FILENO);

		close(pipe_to_child[0]);
		close(pipe_from_child[1]);

		execlp("python3", "python3", "-c", EASYOCR_SCRIPT,
			langs_str.c_str(), gpu_str.c_str(), nullptr);

		// If exec fails
		_exit(1);
	}

	// Parent process
	close(pipe_to_child[0]);
	close(pipe_from_child[1]);

	to_child_fd = pipe_to_child[1];
	from_child_fd = pipe_from_child[0];
	child_pid = pid;

	spdlog::info("EasyOCR process started (pid={})", child_pid);
#else
	throw std::runtime_error("EasyOCRDetector is only supported on Linux");
#endif
}

void EasyOCRDetector::ensure_process() {
	if (child_pid <= 0) {
		start_process();
	}
}

void EasyOCRDetector::write_bytes(const void* data, size_t size) {
#ifdef __linux__
	const uint8_t* ptr = static_cast<const uint8_t*>(data);
	size_t remaining = size;
	while (remaining > 0) {
		ssize_t written = write(to_child_fd, ptr, remaining);
		if (written <= 0) {
			stop_process();
			throw std::runtime_error("EasyOCRDetector: write failed");
		}
		ptr += written;
		remaining -= written;
	}
#endif
}

void EasyOCRDetector::read_bytes(void* data, size_t size) {
#ifdef __linux__
	uint8_t* ptr = static_cast<uint8_t*>(data);
	size_t remaining = size;
	while (remaining > 0) {
		ssize_t bytes_read = read(from_child_fd, ptr, remaining);
		if (bytes_read <= 0) {
			stop_process();
			throw std::runtime_error("EasyOCRDetector: read failed");
		}
		ptr += bytes_read;
		remaining -= bytes_read;
	}
#endif
}

std::vector<EasyOCRResult> EasyOCRDetector::detect(const stb::Image<stb::RGB>* image) {
	if (!image->data) {
		return {};
	}

	ensure_process();

	// Encode image as PNG
	std::vector<uint8_t> png_data = image->write_png_mem();

	// Send: [4 bytes size][PNG data]
	uint32_t png_size = static_cast<uint32_t>(png_data.size());
	write_bytes(&png_size, sizeof(png_size));
	write_bytes(png_data.data(), png_data.size());

	// Receive: [4 bytes size][JSON data]
	uint32_t json_size = 0;
	read_bytes(&json_size, sizeof(json_size));

	std::vector<char> json_buf(json_size);
	read_bytes(json_buf.data(), json_size);

	std::string json_str(json_buf.begin(), json_buf.end());
	nlohmann::json results = nlohmann::json::parse(json_str);

	std::vector<EasyOCRResult> output;
	for (auto& item : results) {
		EasyOCRResult result;
		result.rect.left = item.at("left").get<int32_t>();
		result.rect.top = item.at("top").get<int32_t>();
		result.rect.right = item.at("right").get<int32_t>();
		result.rect.bottom = item.at("bottom").get<int32_t>();
		result.text = item.at("text").get<std::string>();
		output.push_back(result);
	}

	return output;
}

}
