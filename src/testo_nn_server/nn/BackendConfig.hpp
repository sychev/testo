
#pragma once

#include <string>
#include <vector>

namespace nn {

enum class TextBackend {
	Native,
	EasyOCR
};

struct BackendConfig {
	static BackendConfig& instance() {
		static BackendConfig config;
		return config;
	}

	BackendConfig(const BackendConfig&) = delete;
	BackendConfig& operator=(const BackendConfig&) = delete;

	TextBackend text_backend = TextBackend::Native;
	std::vector<std::string> easyocr_languages = {"en", "ru"};
	bool easyocr_gpu = false;

private:
	BackendConfig() = default;
};

}
