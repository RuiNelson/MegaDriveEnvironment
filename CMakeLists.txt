cmake_minimum_required(VERSION 3.24)
project(MegaDriveEnvironment LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

find_package(SDL3 REQUIRED)

include(FetchContent)

FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        yaml-cpp-0.9.0
)

FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
)

FetchContent_Declare(
    libpng
    GIT_REPOSITORY https://github.com/pnggroup/libpng.git
    GIT_TAG        v1.6.47
)

set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PNG_SHARED      OFF CACHE BOOL "" FORCE)
set(PNG_STATIC      ON  CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK   OFF CACHE BOOL "" FORCE)
set(PNG_TESTS       OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS       OFF CACHE BOOL "" FORCE)
set(PNG_EXECUTABLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(yaml-cpp zlib)
FetchContent_MakeAvailable(libpng)

file(GLOB_RECURSE MEGADRIVE_ENVIRONMENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)

add_library(MegaDriveEnvironment STATIC ${MEGADRIVE_ENVIRONMENT_SOURCES})
add_library(MegaDriveEnvironment::MegaDriveEnvironment ALIAS MegaDriveEnvironment)

set(MEGADRIVE_ENVIRONMENT_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/include/MegaDriveEnvironment"
)

target_include_directories(MegaDriveEnvironment PUBLIC
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/cpu
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/memory
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/graphics
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/controllers
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/sound
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/sound/mame_ymfm
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/z80
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/z80/mame_z80
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/config
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/config/controls
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/runtime_tests
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/runtime_tests/controllers
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/runtime_tests/sound
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/runtime_tests/vdp_tests
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/util
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/util/font
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/util/image
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/art
)

target_link_libraries(MegaDriveEnvironment PUBLIC
    SDL3::SDL3
    yaml-cpp::yaml-cpp
    png_static
    zlibstatic
)
