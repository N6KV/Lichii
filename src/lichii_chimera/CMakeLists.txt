# SPDX-FileCopyrightText: Copyright 2026 Lichii Chimera frontend contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# lichii-chimera: windowless Lichii driven by the Chimera frontend over stdin/stdout.
add_executable(lichii-chimera
    chimera_protocol.h
    main.cpp
    ../lichii_cmd/sdl_config.cpp
    ../lichii_cmd/sdl_config.h
)

target_link_libraries(lichii-chimera PRIVATE common core input_common frontend_common video_core)
target_link_libraries(lichii-chimera PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads SDL3::SDL3)
target_include_directories(lichii-chimera PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(UNIX AND NOT APPLE)
    install(TARGETS lichii-chimera)
endif()
