cmake_minimum_required(VERSION 3.0)

if (WIN32)
	add_definitions(-D__PRETTY_FUNCTION__=__FUNCSIG__)
endif()

file(GLOB_RECURSE SRCS *.cpp)
add_library(net STATIC ${SRCS})

if (UNIX)
	target_link_libraries(net pthread)
endif()
