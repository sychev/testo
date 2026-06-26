cmake_minimum_required(VERSION 3.0)

file(GLOB_RECURSE SRCS *.cpp)
add_library(guestfs++ STATIC ${SRCS})

# guestfs.cpp ссылается на хостовый глобал g_io (в testo_core), а бинарь testo
# линкуется как PIE. Без PIC это даёт текст-релокацию против g_io (DT_TEXTREL),
# что на hardened-ОС (AstraLinux) может приводить к падению. PIC убирает её.
set_target_properties(guestfs++ PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(guestfs++ guestfs)