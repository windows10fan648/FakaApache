cmake_minimum_required(VERSION 3.20)

project(FakaApache LANGUAGES CXX)

find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)

add_executable(fakaapache webserver.cpp)
target_compile_features(fakaapache PRIVATE cxx_std_17)
target_compile_definitions(fakaapache PRIVATE FAKAAPACHE_WITH_OPENSSL)

if(MSVC)
    target_compile_options(fakaapache PRIVATE /W4 /permissive-)
else()
    target_compile_options(fakaapache PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_compile_options(fakaapache PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-pthread>)
    target_link_options(fakaapache PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-pthread>)
endif()

if(WIN32)
    target_link_libraries(fakaapache PRIVATE ws2_32 OpenSSL::SSL OpenSSL::Crypto)
else()
    target_link_libraries(fakaapache PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
