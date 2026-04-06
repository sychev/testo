
#include "TextTensor.hpp"
#include "TextDetector.hpp"
#include "TextRecognizer.hpp"
#include "TextColorPicker.hpp"
#include "BackendConfig.hpp"
#include "EasyOCRDetector.hpp"
#include <algorithm>
#include <locale>
#include <codecvt>

namespace nn {

static std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> u32conv;

// Character equivalence map based on TextRecognizer::symbols groups.
// Maps each character to a canonical representative so that
// visually similar Latin/Cyrillic characters match each other.
static char32_t normalize_char(char32_t ch) {
	static const std::vector<std::u32string>& groups = TextRecognizer::symbols;
	for (auto& group : groups) {
		if (group.empty()) continue;
		for (char32_t c : group) {
			if (c == ch) {
				return group[0];
			}
		}
	}
	return ch;
}

static std::u32string normalize_string(const std::u32string& s) {
	std::u32string result;
	result.reserve(s.size());
	for (char32_t ch : s) {
		if (ch != U' ') {
			result.push_back(normalize_char(ch));
		}
	}
	return result;
}

static bool contains_normalized(const std::string& haystack_utf8, const std::string& needle_utf8) {
	std::u32string haystack = normalize_string(u32conv.from_bytes(haystack_utf8));
	std::u32string needle = normalize_string(u32conv.from_bytes(needle_utf8));
	if (needle.empty()) return false;
	return haystack.find(needle) != std::u32string::npos;
}

TextTensor TextTensor::match_text(const stb::Image<stb::RGB>* image, const std::string& text) {
	TextTensor result;

	if (BackendConfig::instance().text_backend == TextBackend::EasyOCR) {
		for (auto& textline : objects) {
			if (contains_normalized(textline.text_recognizer_cache.recognized_text, text)) {
				result.objects.push_back(textline);
			}
		}
		return result;
	}

	for (auto& textline: objects) {
		for (auto& new_textline: TextRecognizer::instance().recognize(image, textline, text)) {
			result.objects.push_back(new_textline);
		}
	}
	return result;
}

TextTensor TextTensor::match_color(const stb::Image<stb::RGB>* image, const std::string& fg, const std::string& bg) {
	TextTensor result;
	for (auto& textline: objects) {
		if (TextColorPicker::instance().run(image, textline, fg, bg)) {
			result.objects.push_back(textline);
		}
	}
	return result;
}

static TextTensor find_text_easyocr(const stb::Image<stb::RGB>* image) {
	TextTensor result;

	std::vector<EasyOCRResult> detections = EasyOCRDetector::instance().detect(image);

	std::sort(detections.begin(), detections.end(), [](const EasyOCRResult& a, const EasyOCRResult& b) {
		return a.rect.left < b.rect.left;
	});

	// Group nearby detections into text lines (same logic as native backend)
	std::vector<bool> visited(detections.size(), false);
	for (size_t i = 0; i < detections.size(); ++i) {
		if (visited[i]) {
			continue;
		}
		visited[i] = true;
		size_t a = i;

		TextLine textline;
		textline.rect = detections[a].rect;
		std::string combined_text = detections[a].text;

		while (true) {
easyocr_textline_next:
			for (size_t j = a + 1; j < detections.size(); ++j) {
				if (visited[j]) {
					continue;
				}
				size_t b = j;

				if (detections[b].rect.left > (detections[a].rect.right + detections[a].rect.height())) {
					goto easyocr_textline_finish;
				}
				int32_t min_bottom = std::min(detections[a].rect.bottom, detections[b].rect.bottom);
				int32_t max_bottom = std::max(detections[a].rect.bottom, detections[b].rect.bottom);
				int32_t min_top = std::min(detections[a].rect.top, detections[b].rect.top);
				int32_t max_top = std::max(detections[a].rect.top, detections[b].rect.top);
				if ((min_bottom - max_top) >= ((max_bottom - min_top) / 2)) {
					visited[j] = true;
					textline.rect |= detections[b].rect;
					combined_text += " " + detections[b].text;
					a = b;
					goto easyocr_textline_next;
				}
			}
			goto easyocr_textline_finish;
		}
easyocr_textline_finish:
		textline.text_recognizer_cache.recognized_text = combined_text;
		result.objects.push_back(textline);
	}

	std::sort(result.objects.begin(), result.objects.end(), [](const TextLine& a, const TextLine& b) {
		return a.rect.top < b.rect.top;
	});

	return result;
}

TextTensor find_text(const stb::Image<stb::RGB>* image) {
	if (BackendConfig::instance().text_backend == TextBackend::EasyOCR) {
		return find_text_easyocr(image);
	}

	TextTensor result;

	std::vector<TextLine> words = TextDetector::instance().detect(image);

	std::sort(words.begin(), words.end(), [](const TextLine& a, const TextLine& b) {
		return a.rect.left < b.rect.left;
	});

	std::vector<bool> visited_words(words.size(), false);
	for (size_t i = 0; i < words.size(); ++i) {
		if (visited_words[i]) {
			continue;
		}
		visited_words[i] = true;
		size_t a = i;

		TextLine textline;
		textline.rect = words[a].rect;

		while (true) {
textline_next:
			for (size_t j = a + 1; j < words.size(); ++j) {
				if (visited_words[j]) {
					continue;
				}
				size_t b = j;

				if (words[b].rect.left > (words[a].rect.right + words[a].rect.height())) {
					goto textline_finish;
				}
				int32_t min_bottom = std::min(words[a].rect.bottom, words[b].rect.bottom);
				int32_t max_bottom = std::max(words[a].rect.bottom, words[b].rect.bottom);
				int32_t min_top = std::min(words[a].rect.top, words[b].rect.top);
				int32_t max_top = std::max(words[a].rect.top, words[b].rect.top);
				if ((min_bottom - max_top) >= ((max_bottom - min_top) / 2)) {
					visited_words[j] = true;
					textline.rect |= words[b].rect;
					a = b;
					goto textline_next;
				}
			}
			goto textline_finish;
		}
textline_finish:
		result.objects.push_back(textline);
	}

	std::sort(result.objects.begin(), result.objects.end(), [](const TextLine& a, const TextLine& b) {
		return a.rect.top < b.rect.top;
	});

	return result;
}

}
