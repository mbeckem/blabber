set(MODULE_SOURCES
    module.cpp
    database.cpp
    storage.cpp

    common.hpp
    database.hpp
    fixed_string.hpp
    storage.hpp
)

pybind11_add_module(blabber_database ${MODULE_SOURCES})
target_compile_options(blabber_database PRIVATE -Wall -Wextra)

# Hide symbols
target_link_libraries(blabber_database PRIVATE -Wl,--exclude-libs,ALL)
target_link_libraries(blabber_database PRIVATE prequel)


