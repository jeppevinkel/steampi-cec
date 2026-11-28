cmake_minimum_required(VERSION 3.31)
project(steampi_cec)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCEC REQUIRED IMPORTED_TARGET libcec)
pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)

add_executable(steampi_cec main.cpp
        Logger.cpp
        Logger.h
        CecEventQueue.cpp
        CecEventQueue.h
        FileDescriptor.h
        PhysicalAddress.h
        PowerStateMachine.cpp
        PowerStateMachine.h
        SteamControllerDecoder.cpp
        SteamControllerDecoder.h
        VirtualKeyboard.cpp
        VirtualKeyboard.h
        EventLoop.h
        Timers.h
        SteamControllerPuck.cpp
        SteamControllerPuck.h
        ControllerRouter.cpp
        ControllerRouter.h
        ControllerMode.h
        AppProfiles.h
        ControlProtocol.h
        ControlServer.cpp
        ControlServer.h
        AppLaunchClient.cpp
        AppLaunchClient.h)

target_compile_options(steampi_cec PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(steampi_cec PRIVATE PkgConfig::LIBCEC PkgConfig::LIBUDEV)