# SPDX-FileCopyrightText: Copyright 2026 Eglantine Hub contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Eglantine Hub: a companion launcher shipped next to eglantine.exe. Its core (game file
# inspection, CPU-aware launching) has no Qt dependency; the window is plain Qt Widgets.

add_library(eglantine_hub_core STATIC
    hub_core.cpp
    hub_core.h
)
target_compile_features(eglantine_hub_core PUBLIC cxx_std_20)
target_include_directories(eglantine_hub_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if (WIN32)
    target_compile_definitions(eglantine_hub_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

set(HUB_SOURCES
    main.cpp
    hub_window.cpp
    hub_window.h
    hub.qrc
)
if (WIN32)
    list(APPEND HUB_SOURCES hub.rc)
endif()

add_executable(eglantine-hub ${HUB_SOURCES})
set_target_properties(eglantine-hub PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    WIN32_EXECUTABLE ON
)
target_link_libraries(eglantine-hub PRIVATE eglantine_hub_core Qt6::Widgets)

if (UNIX AND NOT APPLE)
    install(TARGETS eglantine-hub)
endif()
