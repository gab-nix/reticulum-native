cmake_minimum_required(VERSION 3.21)

project(reticulum-c VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(RETICULUM_BUILD_TESTS "Build reticulum-c tests" ${BUILD_TESTING})
option(RETICULUM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(RETICULUM_SANITIZERS "" CACHE STRING
    "Comma-separated sanitizers (for example: address,undefined)")

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

file(GLOB_RECURSE RETICULUM_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_library(reticulum ${RETICULUM_SOURCES})
add_library(reticulum::reticulum ALIAS reticulum)

target_compile_features(reticulum PUBLIC c_std_17)
set_target_properties(reticulum PROPERTIES
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

target_include_directories(reticulum
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(reticulum
    PUBLIC OpenSSL::Crypto
    PRIVATE Threads::Threads)

include(cmake/CompilerWarnings.cmake)
reticulum_set_project_warnings(reticulum)

if(RETICULUM_SANITIZERS)
    include(cmake/Sanitizers.cmake)
    reticulum_enable_sanitizers(reticulum "${RETICULUM_SANITIZERS}")
endif()

if(RETICULUM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS reticulum
    EXPORT reticulum-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT reticulum-targets
    FILE reticulum-targets.cmake
    NAMESPACE reticulum::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/reticulum)
configure_package_config_file(
    cmake/reticulum-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/reticulum-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/reticulum)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/reticulum-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/reticulum-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/reticulum-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/reticulum)

