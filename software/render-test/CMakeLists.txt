cmake_minimum_required(VERSION 3.16)
project(epub_render C)

set(CMAKE_C_STANDARD 11)

# --- Locate MuPDF (Homebrew or system) ---
find_path(MUPDF_INCLUDE_DIR mupdf/fitz.h
    HINTS /opt/homebrew/include /usr/local/include /usr/include
)
find_library(MUPDF_LIB mupdf
    HINTS /opt/homebrew/lib /usr/local/lib /usr/lib
)
find_library(MUPDF_THIRD_LIB mupdf-third
    HINTS /opt/homebrew/lib /usr/local/lib /usr/lib
)

if(NOT MUPDF_INCLUDE_DIR OR NOT MUPDF_LIB)
    message(FATAL_ERROR "MuPDF not found. Install with: brew install mupdf")
endif()

message(STATUS "MuPDF include: ${MUPDF_INCLUDE_DIR}")
message(STATUS "MuPDF lib:     ${MUPDF_LIB}")

# --- Build ---
add_executable(epub_render main.c)
target_include_directories(epub_render PRIVATE ${MUPDF_INCLUDE_DIR})

set(MUPDF_LIBS ${MUPDF_LIB})
if(MUPDF_THIRD_LIB)
    list(APPEND MUPDF_LIBS ${MUPDF_THIRD_LIB})
endif()

target_link_libraries(epub_render PRIVATE ${MUPDF_LIBS})

if(APPLE)
    target_link_libraries(epub_render PRIVATE
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework ImageIO"
    )
endif()

target_link_libraries(epub_render PRIVATE m z)
