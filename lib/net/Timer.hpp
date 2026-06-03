
#pragma once

#include <chrono>
#include <thread>
#include <algorithm>

#include <net/CheckPoint.hpp>

namespace net {

/// Прерываемый сон: спит маленькими квантами, между ними вызывает check_point()
template <typename Duration>
void sleep_for(Duration duration) {
	const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(duration);
	constexpr auto quantum = std::chrono::milliseconds(20);
	while (true) {
		check_point();
		const auto now = Clock::now();
		if (now >= deadline) {
			break;
		}
		std::this_thread::sleep_for(std::min<Clock::duration>(deadline - now, quantum));
	}
}

/*!
	@brief Прерываемый таймер (аналог coro::Timer)

	Сохранён как класс, чтобы не менять места, где Timer хранится как член.
*/
class Timer {
public:
	template <typename Duration>
	void waitFor(Duration duration) {
		sleep_for(duration);
	}

	template <typename Timestamp>
	void waitUntil(Timestamp timestamp) {
		constexpr auto quantum = std::chrono::milliseconds(20);
		while (true) {
			check_point();
			const auto now = std::chrono::steady_clock::now();
			if (now >= timestamp) {
				break;
			}
			std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
				std::chrono::duration_cast<std::chrono::steady_clock::duration>(timestamp - now), quantum));
		}
	}
};

}
