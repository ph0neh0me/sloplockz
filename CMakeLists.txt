cmake_minimum_required(VERSION 3.20)
project(sloplockz VERSION 1.0.0 LANGUAGES C)

if(NOT WIN32)
    message(FATAL_ERROR "sloplockz supports Windows only")
endif()

add_executable(sloplockz src/sloplockz.c)
target_compile_definitions(sloplockz PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_features(sloplockz PRIVATE c_std_11)

if(MSVC)
    target_compile_options(sloplockz PRIVATE /W4 /permissive- /utf-8)
    target_link_options(sloplockz PRIVATE /SUBSYSTEM:CONSOLE)
endif()

install(TARGETS sloplockz RUNTIME DESTINATION bin)
