cmake_minimum_required(VERSION 3.20)
project(nerd-dictation-remote VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/nerd-dictation-api")
if(NOT EXISTS "${API_DIR}/include/nerd_dictation/backend_api.h")
    message(FATAL_ERROR "nerd-dictation-api submodule missing: git submodule update --init")
endif()

# The bridge the client runs on the server host, embedded as a C++ string.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge.py" BRIDGE_PY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/bridge.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/bridge.h" @ONLY)

find_package(Threads REQUIRED)

add_library(nerd-dictation-remote SHARED src/remote_backend.cpp)
target_include_directories(nerd-dictation-remote PRIVATE
    "${API_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(nerd-dictation-remote PRIVATE Threads::Threads)
target_compile_options(nerd-dictation-remote PRIVATE -Wall -Wextra)
set_target_properties(nerd-dictation-remote PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

include(GNUInstallDirs)
install(TARGETS nerd-dictation-remote LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
