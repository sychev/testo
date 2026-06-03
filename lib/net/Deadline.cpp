
#include <net/Deadline.hpp>
#include <vector>

namespace net {

namespace {
	// Стек действующих дедлайнов. Каждый элемент уже "схлопнут" с родителем,
	// поэтому back() — это всегда самый ранний из активных дедлайнов.
	thread_local std::vector<Clock::time_point> deadline_stack;
}

void Deadline::push(Clock::time_point deadline) {
	if (!deadline_stack.empty() && deadline_stack.back() < deadline) {
		deadline = deadline_stack.back();
	}
	deadline_stack.push_back(deadline);
}

Deadline::~Deadline() {
	if (!deadline_stack.empty()) {
		deadline_stack.pop_back();
	}
}

std::optional<Clock::time_point> Deadline::current() {
	if (deadline_stack.empty()) {
		return std::nullopt;
	}
	return deadline_stack.back();
}

bool Deadline::expired() {
	auto deadline = current();
	return deadline.has_value() && (Clock::now() >= *deadline);
}

}
