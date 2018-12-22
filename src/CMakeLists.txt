set(SOURCES
    main.cpp
    storage.cpp

    common.hpp
    storage.hpp
)

add_executable(blabber ${SOURCES})
target_link_libraries(blabber PRIVATE prequel)
