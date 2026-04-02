cmake_minimum_required(VERSION 3.16)
project(ShaderIDsGenerator)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(ShaderIDsGenerator src/main.cpp)

if(MSVC)
    target_compile_options(ShaderIDsGenerator PRIVATE /MP)
endif()
