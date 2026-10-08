cmake_minimum_required(VERSION 4.3)
project(steampi_cec)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCEC REQUIRED IMPORTED_TARGET libcec)

add_executable(steampi_cec main.cpp
        Logger.cpp
        Logger.h)

target_link_libraries(steampi_cec PRIVATE PkgConfig::LIBCEC)