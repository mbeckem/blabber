cmake_minimum_required(VERSION 3.0)

project(blabber)

# Set all symbols to "hidden" by default - this allows us to omit them in the final
# shared library that we will import from python. This reduces bloat in larger applications.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")

# Needed because we're about to produce a .so (or equivalent) for our python module.
set(CMAKE_POSITION_INDEPENDENT_CODE 1)

add_subdirectory(deps/fmt)
add_subdirectory(deps/prequel)

set(PYBIND11_CPP_STANDARD -std=c++17)
add_subdirectory(deps/pybind11)

add_subdirectory(src)
