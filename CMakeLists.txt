cmake_minimum_required(VERSION 3.20)

set(CLASSIC_NOTEPAD_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
set(CLASSIC_NOTEPAD_VERSION "1.0.0")
set(CLASSIC_NOTEPAD_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/classic_notepad_large_transparent.ico")

if(EXISTS "${CLASSIC_NOTEPAD_VERSION_FILE}")
    file(READ "${CLASSIC_NOTEPAD_VERSION_FILE}" CLASSIC_NOTEPAD_VERSION)
    string(STRIP "${CLASSIC_NOTEPAD_VERSION}" CLASSIC_NOTEPAD_VERSION)
endif()

if(NOT CLASSIC_NOTEPAD_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "VERSION must contain a semantic version like 1.0.0")
endif()

set(CLASSIC_NOTEPAD_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(CLASSIC_NOTEPAD_VERSION_MINOR "${CMAKE_MATCH_2}")
set(CLASSIC_NOTEPAD_VERSION_PATCH "${CMAKE_MATCH_3}")

project(ClassicNotepad
    VERSION "${CLASSIC_NOTEPAD_VERSION}"
    LANGUAGES CXX
)

configure_file(
    src/app_version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/app_version.h"
    @ONLY
)

set(CLASSIC_NOTEPAD_SHARED_CORE_SOURCES
    src/ansi_encoding.h
    src/document.cpp
    src/document.h
    src/encoding.cpp
    src/encoding.h
    src/file_io.h
    src/line_endings.cpp
    src/line_endings.h
    src/spell_text_utils.cpp
    src/spell_text_utils.h
    src/text_metadata.cpp
    src/text_metadata.h
)

set(CLASSIC_NOTEPAD_PORTABLE_PLATFORM_SOURCES
    src/platform/portable/ansi_encoding_portable.cpp
    src/platform/portable/file_io_std.cpp
)

set(CLASSIC_NOTEPAD_WINDOWS_PLATFORM_SOURCES
    src/platform/windows/ansi_encoding_win32.cpp
    src/platform/windows/file_io_win32.cpp
)

set(CLASSIC_NOTEPAD_CORE_PLATFORM_SOURCES
    ${CLASSIC_NOTEPAD_PORTABLE_PLATFORM_SOURCES}
)

if(WIN32)
    set(CLASSIC_NOTEPAD_CORE_PLATFORM_SOURCES
        ${CLASSIC_NOTEPAD_WINDOWS_PLATFORM_SOURCES}
    )
endif()

add_library(ClassicNotepadCore STATIC
    ${CLASSIC_NOTEPAD_SHARED_CORE_SOURCES}
    ${CLASSIC_NOTEPAD_CORE_PLATFORM_SOURCES}
)

target_compile_features(ClassicNotepadCore PUBLIC cxx_std_17)

target_include_directories(ClassicNotepadCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(WIN32)
    target_compile_definitions(ClassicNotepadCore PUBLIC
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

if(MSVC)
    target_compile_options(ClassicNotepadCore PRIVATE
        /W4
        /permissive-
        /utf-8
    )
endif()

if(WIN32)
    enable_language(RC)

    add_executable(ClassicNotepad WIN32
        "${CLASSIC_NOTEPAD_APP_ICON}"
        src/platform/windows/app.cpp
        src/platform/windows/app.h
        src/platform/windows/automation.cpp
        src/platform/windows/automation.h
        src/platform/windows/main.cpp
        src/platform/windows/spell_check.cpp
        src/platform/windows/spell_check.h
        src/platform/windows/resource.h
        src/platform/windows/resources.rc
        src/platform/windows/win32_platform.h
    )

    target_include_directories(ClassicNotepad PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )

    target_link_libraries(ClassicNotepad PRIVATE
        ClassicNotepadCore
        comctl32
        comdlg32
        shell32
        user32
        gdi32
        ole32
        advapi32
        dwmapi
    )

    if(MSVC)
        target_compile_options(ClassicNotepad PRIVATE
            /W4
            /permissive-
            /utf-8
        )
    endif()

    set_target_properties(ClassicNotepad PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY
        VS_STARTUP_PROJECT ClassicNotepad
    )
endif()

if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTK4 QUIET IMPORTED_TARGET gtk4)
    endif()

    if(GTK4_FOUND)
        add_executable(ClassicNotepadGtk
            src/platform/linux/gtk_actions.cpp
            src/platform/linux/gtk_actions.h
            src/platform/linux/gtk_app.cpp
            src/platform/linux/gtk_app.h
            src/platform/linux/gtk_automation.cpp
            src/platform/linux/gtk_automation.h
            src/platform/linux/gtk_dialogs.cpp
            src/platform/linux/gtk_dialogs.h
            src/platform/linux/gtk_main.cpp
        )

        target_compile_features(ClassicNotepadGtk PRIVATE cxx_std_17)

        target_link_libraries(ClassicNotepadGtk PRIVATE
            ClassicNotepadCore
            PkgConfig::GTK4
        )
    else()
        message(STATUS "GTK4 development package not found; skipping ClassicNotepadGtk target.")
    endif()
endif()

enable_testing()

add_executable(TextConversionTests
    tests/text_conversion_tests.cpp
)

target_link_libraries(TextConversionTests PRIVATE
    ClassicNotepadCore
)

if(MSVC)
    target_compile_options(TextConversionTests PRIVATE
        /W4
        /permissive-
        /utf-8
    )
endif()

add_test(NAME TextConversionTests COMMAND $<TARGET_FILE:TextConversionTests>)

if(WIN32)
    add_library(ClassicNotepadPortableCoreSmoke STATIC
        ${CLASSIC_NOTEPAD_SHARED_CORE_SOURCES}
        ${CLASSIC_NOTEPAD_PORTABLE_PLATFORM_SOURCES}
    )

    target_compile_features(ClassicNotepadPortableCoreSmoke PUBLIC cxx_std_17)

    target_include_directories(ClassicNotepadPortableCoreSmoke PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )

    if(MSVC)
        target_compile_options(ClassicNotepadPortableCoreSmoke PRIVATE
            /W4
            /permissive-
            /utf-8
        )
    endif()

    add_executable(TextConversionPortableSmokeTests
        tests/text_conversion_tests.cpp
    )

    target_link_libraries(TextConversionPortableSmokeTests PRIVATE
        ClassicNotepadPortableCoreSmoke
    )

    if(MSVC)
        target_compile_options(TextConversionPortableSmokeTests PRIVATE
            /W4
            /permissive-
            /utf-8
        )
    endif()

    add_test(NAME TextConversionPortableSmokeTests COMMAND $<TARGET_FILE:TextConversionPortableSmokeTests>)
endif()
