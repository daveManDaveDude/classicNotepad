cmake_minimum_required(VERSION 3.20)

project(ClassicNotepad
    VERSION 0.1.0
    LANGUAGES CXX RC
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
    classic_notepad_large_transparent.ico
    src/app.cpp
    src/app.h
    src/main.cpp
    src/spell_check.cpp
    src/spell_check.h
    src/resource.h
    src/resources.rc
)

target_include_directories(ClassicNotepad PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
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
