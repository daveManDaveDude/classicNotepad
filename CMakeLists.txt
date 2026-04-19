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
    LANGUAGES CXX RC
)

configure_file(
    src/app_version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/app_version.h"
    @ONLY
)

add_library(ClassicNotepadCore STATIC
    src/document.cpp
    src/document.h
    src/encoding.cpp
    src/encoding.h
    src/line_endings.cpp
    src/line_endings.h
    src/spell_text_utils.cpp
    src/spell_text_utils.h
    src/win32_platform.h
)

target_compile_features(ClassicNotepadCore PUBLIC cxx_std_17)

target_include_directories(ClassicNotepadCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(ClassicNotepadCore PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(ClassicNotepadCore PRIVATE
        /W4
        /permissive-
        /utf-8
    )
endif()

add_executable(ClassicNotepad WIN32
    "${CLASSIC_NOTEPAD_APP_ICON}"
    src/app.cpp
    src/app.h
    src/main.cpp
    src/spell_check.cpp
    src/spell_check.h
    src/resource.h
    src/resources.rc
    src/win32_platform.h
)

target_include_directories(ClassicNotepad PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
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

set_target_properties(ClassicNotepad PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY
    VS_STARTUP_PROJECT ClassicNotepad
)
