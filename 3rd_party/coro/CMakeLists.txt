cmake_minimum_required(VERSION 3.15)

file(GLOB FILES *.cpp)

if (APPLE)
	add_definitions(-D_XOPEN_SOURCE)
endif()

add_library(coro STATIC ${FILES})

# MT-модель coro построена на asio::spawn, который для stackful-корутин использует
# Boost.Context (boost/context/fiber.hpp). Линкуем его публично, чтобы потребители coro
# получали зависимость транзитивно.
find_package(Boost REQUIRED COMPONENTS context)
target_link_libraries(coro PUBLIC Boost::context)

if (UNIX)
	target_link_libraries(coro PUBLIC pthread)
endif()

add_subdirectory(tests)
