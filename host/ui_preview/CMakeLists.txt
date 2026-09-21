cmake_minimum_required(VERSION 3.16)
project(meet_ui_preview LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MEET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(MEET_UI "${MEET_ROOT}/main/ui")

# Pin to same major.minor as main/idf_component.yml (lvgl/lvgl: ~9.5).
include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.5.0
    GIT_SHALLOW TRUE
)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
FetchContent_MakeAvailable(lvgl)

find_path(SDL2_INCLUDE_DIR SDL.h
    PATHS /opt/homebrew/include /usr/local/include /usr/include
    PATH_SUFFIXES SDL2)
find_library(SDL2_LIBRARY
    NAMES SDL2
    PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)

if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
    message(FATAL_ERROR "SDL2 not found. On macOS: brew install sdl2")
endif()

# LVGL expects #include <SDL2/SDL.h>, so pass the parent of SDL2/.
get_filename_component(SDL2_INCLUDE_PARENT "${SDL2_INCLUDE_DIR}" DIRECTORY)

add_executable(meet_ui_preview
    main.cc
    "${MEET_UI}/ui_view.cc"
    "${MEET_UI}/ui_face.cc"
    "${MEET_UI}/fonts/font_meet_cjk_16_4.c"
)

target_include_directories(meet_ui_preview PRIVATE
    "${MEET_UI}"
    "${SDL2_INCLUDE_PARENT}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(meet_ui_preview PRIVATE
    LV_CONF_INCLUDE_SIMPLE
)

target_link_libraries(meet_ui_preview PRIVATE
    lvgl
    "${SDL2_LIBRARY}"
)

# LVGL's SDL driver also needs the SDL include parent.
target_include_directories(lvgl PUBLIC "${SDL2_INCLUDE_PARENT}")
target_link_libraries(lvgl PUBLIC "${SDL2_LIBRARY}")
