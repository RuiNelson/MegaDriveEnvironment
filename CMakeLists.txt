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

# Consumers (e.g. StreetsOfRageRecompilation) can request shared FetchContent
# deps. Default is static archives embedded into MegaDriveEnvironment.
option(MEGADRIVE_ENVIRONMENT_SHARED_DEPS
    "Build and link yaml-cpp/zlib/libpng as shared libraries"
    OFF
)

# Required when embedding static FetchContent deps into a shared library;
# also needed for shared deps. Harmless for a pure static build.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(SDL3 REQUIRED)
find_package(Threads REQUIRED)

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
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK   OFF CACHE BOOL "" FORCE)
set(PNG_TESTS       OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS       OFF CACHE BOOL "" FORCE)
set(PNG_EXECUTABLES OFF CACHE BOOL "" FORCE)

if(MEGADRIVE_ENVIRONMENT_SHARED_DEPS)
    set(YAML_BUILD_SHARED_LIBS ON  CACHE BOOL "" FORCE)
    set(PNG_SHARED             ON  CACHE BOOL "" FORCE)
    set(PNG_STATIC             OFF CACHE BOOL "" FORCE)
    set(MEGADRIVE_ENVIRONMENT_PNG_TARGET png_shared)
    set(MEGADRIVE_ENVIRONMENT_ZLIB_TARGET zlib)
else()
    set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(PNG_SHARED             OFF CACHE BOOL "" FORCE)
    set(PNG_STATIC             ON  CACHE BOOL "" FORCE)
    set(MEGADRIVE_ENVIRONMENT_PNG_TARGET png_static)
    set(MEGADRIVE_ENVIRONMENT_ZLIB_TARGET zlibstatic)
endif()

FetchContent_MakeAvailable(yaml-cpp zlib)

# zlib's CMake only creates the `zlib` / `zlibstatic` targets; libpng expects
# ZLIB::ZLIB.
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS ${MEGADRIVE_ENVIRONMENT_ZLIB_TARGET})
endif()
set(ZLIB_FOUND TRUE)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR}" CACHE PATH "" FORCE)
set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
set(ZLIB_LIBRARY ${MEGADRIVE_ENVIRONMENT_ZLIB_TARGET} CACHE FILEPATH "" FORCE)
set(ZLIB_LIBRARIES ${MEGADRIVE_ENVIRONMENT_ZLIB_TARGET})

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

if(MEGADRIVE_ENVIRONMENT_SHARED_DEPS)
    # Runtime lookup for shared FetchContent deps (yaml-cpp, libpng, zlib).
    set_target_properties(MegaDriveEnvironment PROPERTIES
        BUILD_RPATH
            "$<TARGET_FILE_DIR:yaml-cpp>;$<TARGET_FILE_DIR:png_shared>;$<TARGET_FILE_DIR:zlib>"
        INSTALL_RPATH
            "$<TARGET_FILE_DIR:yaml-cpp>;$<TARGET_FILE_DIR:png_shared>;$<TARGET_FILE_DIR:zlib>"
    )
endif()

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

# SDL appears in public headers. Implementation-only deps stay private for a
# shared build so consumers do not re-link yaml/png/zlib on every dylib rebuild.
target_link_libraries(MegaDriveEnvironment PUBLIC SDL3::SDL3)
target_link_libraries(MegaDriveEnvironment PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(MegaDriveEnvironment PRIVATE ws2_32)
endif()
if(MEGADRIVE_ENVIRONMENT_BUILD_SHARED)
    target_link_libraries(MegaDriveEnvironment PRIVATE
        yaml-cpp::yaml-cpp
        ${MEGADRIVE_ENVIRONMENT_PNG_TARGET}
        ${MEGADRIVE_ENVIRONMENT_ZLIB_TARGET}
    )
else()
    target_link_libraries(MegaDriveEnvironment PUBLIC
        yaml-cpp::yaml-cpp
        ${MEGADRIVE_ENVIRONMENT_PNG_TARGET}
        ${MEGADRIVE_ENVIRONMENT_ZLIB_TARGET}
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

    add_executable(megadrive_environment_remote_access_foundations_test
        tests/remote_access_foundations_test.cpp
    )
    target_link_libraries(megadrive_environment_remote_access_foundations_test PRIVATE MegaDriveEnvironment)
    add_test(
        NAME megadrive_environment_remote_access_foundations_test
        COMMAND megadrive_environment_remote_access_foundations_test
    )

    add_executable(megadrive_environment_remote_access_protocol_test
        tests/remote_access_protocol_test.cpp
    )
    target_link_libraries(megadrive_environment_remote_access_protocol_test PRIVATE MegaDriveEnvironment)
    add_test(
        NAME megadrive_environment_remote_access_protocol_test
        COMMAND megadrive_environment_remote_access_protocol_test
    )

    add_executable(megadrive_environment_restart_test
        tests/restart_test.cpp
    )
    target_link_libraries(megadrive_environment_restart_test PRIVATE MegaDriveEnvironment)
    add_test(
        NAME megadrive_environment_restart_test
        COMMAND megadrive_environment_restart_test
    )

    find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME megadrive_environment_python_remote_client_test
            COMMAND ${Python3_EXECUTABLE} -m unittest discover
                -s ${CMAKE_CURRENT_SOURCE_DIR}/python/tests
                -v
        )
        set_tests_properties(megadrive_environment_python_remote_client_test PROPERTIES
            ENVIRONMENT
                "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python/src;PYTHONDONTWRITEBYTECODE=1"
        )
    endif()
endif()
