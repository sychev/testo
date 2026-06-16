cmake_minimum_required(VERSION 3.15)

# Два бенчмарк-бинарника поверх библиотеки coro.
# coro линкует Boost::context и pthread публично, поэтому здесь их явно указывать не нужно.

add_executable(coro_benchmark_server server.cpp)
target_link_libraries(coro_benchmark_server coro)

add_executable(coro_benchmark_client client.cpp)
target_link_libraries(coro_benchmark_client coro)
