cmake_minimum_required(VERSION 3.20)

project(FakaApache LANGUAGES CXX)

add_executable(fakaapache webserver.cpp)
target_compile_features(fakaapache PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(fakaapache PRIVATE /W4 /permissive-)
else()
    target_compile_options(fakaapache PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_compile_options(fakaapache PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-pthread>)
    target_link_options(fakaapache PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-pthread>)
endif()

if(WIN32)
    target_link_libraries(fakaapache PRIVATE ws2_32)
endif()
