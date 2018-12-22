set(SOURCES
    main.cpp

    common.hpp
    database.hpp

    database.cpp
)

add_executable(blabber ${SOURCES})
target_link_libraries(blabber PRIVATE prequel)
