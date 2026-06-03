
#include <net/Cancel.hpp>

namespace net {

std::stop_source& interrupt_source() {
	static std::stop_source source;
	return source;
}

void reset_interrupt() {
	interrupt_source() = std::stop_source();
}

}
