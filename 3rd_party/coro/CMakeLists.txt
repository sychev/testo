cmake_minimum_required(VERSION 3.15)

# The coroutine engine is now asio::spawn + yield_context (see detail/Engine.hpp).
# The old ucontext fibers (FiberLinux/FiberWindows/Coro.cpp) and the IoService
# run loop have been removed; only a handful of small .cpp files remain.
file(GLOB FILES *.cpp)

if (APPLE)
	add_definitions(-D_XOPEN_SOURCE)
endif()

add_library(coro STATIC ${FILES})

# Requires asio::spawn (stackful coroutines) and C++20.
target_compile_features(coro PUBLIC cxx_std_20)

# NOTE: the unit tests under tests/ exercised the old fiber engine and have not
# been ported yet, so the subdirectory is intentionally not added here.
