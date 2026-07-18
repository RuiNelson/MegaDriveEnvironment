cmake_minimum_required(VERSION 3.24)
project(MegaDriveEnvironment LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Shared by default: consumers only re-link when their own objects change.
# Implementation-only edits rebuild the dylib in place. Set OFF for a static .a.
option(MEGADRIVE_ENVIRONMENT_BUILD_SHARED
    "Build MegaDriveEnvironment as a shared library"
    ON
)

# External FetchContent deps (yaml-cpp, zlib, libpng) are shared libraries.
# PIC keeps objects safe if MEGADRIVE_ENVIRONMENT_BUILD_SHARED is turned off.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(SDL3 REQUIRED)

include(FetchContent)

FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        yaml-cpp-0.9.0
)

# OVERRIDE_FIND_PACKAGE: libpng's find_package(ZLIB) resolves to this build
# instead of the host system library.
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
    OVERRIDE_FIND_PACKAGE
)

FetchContent_Declare(
    libpng
    GIT_REPOSITORY https://github.com/pnggroup/libpng.git
    GIT_TAG        v1.6.47
)

set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Shared external libraries (dynamic link into MegaDriveEnvironment / consumers).
set(YAML_BUILD_SHARED_LIBS ON  CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(PNG_SHARED      ON  CACHE BOOL "" FORCE)
set(PNG_STATIC      OFF CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK   OFF CACHE BOOL "" FORCE)
set(PNG_TESTS       OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS       OFF CACHE BOOL "" FORCE)
set(PNG_EXECUTABLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(yaml-cpp zlib)

# zlib's CMake only creates the `zlib` target; libpng expects ZLIB::ZLIB.
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlib)
endif()
# Module-mode find_package(ZLIB) variables used by some consumers.
set(ZLIB_FOUND TRUE)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR}" CACHE PATH "" FORCE)
set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
set(ZLIB_LIBRARY zlib CACHE FILEPATH "" FORCE)
set(ZLIB_LIBRARIES zlib)

FetchContent_MakeAvailable(libpng)

file(GLOB_RECURSE MEGADRIVE_ENVIRONMENT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)

if(MEGADRIVE_ENVIRONMENT_BUILD_SHARED)
    add_library(MegaDriveEnvironment SHARED ${MEGADRIVE_ENVIRONMENT_SOURCES})
    set_target_properties(MegaDriveEnvironment PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        # Same folder as sibling consumer binaries under a unified build tree is
        # not guaranteed; RPATH on consumers points at this target's output dir.
        BUILD_WITH_INSTALL_RPATH OFF
    )
else()
    add_library(MegaDriveEnvironment STATIC ${MEGADRIVE_ENVIRONMENT_SOURCES})
endif()
add_library(MegaDriveEnvironment::MegaDriveEnvironment ALIAS MegaDriveEnvironment)

# Runtime lookup for shared FetchContent deps (yaml-cpp, libpng, zlib).
set_target_properties(MegaDriveEnvironment PROPERTIES
    BUILD_RPATH
        "$<TARGET_FILE_DIR:yaml-cpp>;$<TARGET_FILE_DIR:png_shared>;$<TARGET_FILE_DIR:zlib>"
    INSTALL_RPATH
        "$<TARGET_FILE_DIR:yaml-cpp>;$<TARGET_FILE_DIR:png_shared>;$<TARGET_FILE_DIR:zlib>"
)

set(MEGADRIVE_ENVIRONMENT_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/include/MegaDriveEnvironment"
)

target_include_directories(MegaDriveEnvironment PUBLIC
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/memory
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/graphics
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/controllers
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/sound
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/sound/mame_ymfm
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/z80
    ${MEGADRIVE_ENVIRONMENT_INCLUDE_DIR}/system/z80/suzukiplan
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

# SDL appears in public headers. Shared FetchContent deps stay private for a
# shared build so consumers do not re-link yaml/png/zlib on every dylib rebuild.
# Runtime still loads those dylibs as dependencies of MegaDriveEnvironment.
target_link_libraries(MegaDriveEnvironment PUBLIC SDL3::SDL3)
if(MEGADRIVE_ENVIRONMENT_BUILD_SHARED)
    target_link_libraries(MegaDriveEnvironment PRIVATE
        yaml-cpp::yaml-cpp
        png_shared
        zlib
    )
else()
    # Static archive still dynamically depends on the shared external libs.
    target_link_libraries(MegaDriveEnvironment PUBLIC
        yaml-cpp::yaml-cpp
        png_shared
        zlib
    )
endif()

include(CTest)
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
    add_executable(megadrive_environment_vdp_color_test tests/vdp_color_test.cpp)
    target_link_libraries(megadrive_environment_vdp_color_test PRIVATE MegaDriveEnvironment)
    add_test(
        NAME megadrive_environment_vdp_color_test
        COMMAND megadrive_environment_vdp_color_test
    )
endif()
