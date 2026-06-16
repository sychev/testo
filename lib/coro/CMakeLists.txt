cmake_minimum_required(VERSION 3.0)

# Header-only asio-based replacement for the former 3rd_party/coro fiber library.
add_library(coro INTERFACE)
target_include_directories(coro INTERFACE
	${CMAKE_SOURCE_DIR}/lib
	${CMAKE_SOURCE_DIR}/3rd_party
)
