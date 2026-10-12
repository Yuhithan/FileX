cmake_minimum_required(VERSION 3.16)
project(TExplorer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(Curses REQUIRED)

add_executable(FileX
    core/main.c
    tui/tui.c
)

target_include_directories(texplorer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    ${CURSES_INCLUDE_DIRS}
)
target_link_libraries(texplorer PRIVATE ${CURSES_LIBRARIES})