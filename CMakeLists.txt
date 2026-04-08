cmake_minimum_required(VERSION 3.20)

project(libatlas VERSION 0.1.0 LANGUAGES CXX)

option(LIBATLAS_BUILD_TESTS "Build libatlas tests" ON)
option(LIBATLAS_BUILD_TOOLS "Build libatlas tools" ON)
option(LIBATLAS_BUILD_EXAMPLES "Build libatlas examples" ON)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

add_library(libatlas
  src/extraction.cpp
  src/identity.cpp
  src/image.cpp
  src/image_io.cpp
  src/packing.cpp
  src/similarity.cpp
  third_party/lodepng/lodepng.cpp
)

add_library(libatlas::libatlas ALIAS libatlas)

target_compile_features(libatlas PUBLIC cxx_std_17)

target_include_directories(libatlas
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/lodepng
)

if(MSVC)
  target_compile_options(libatlas PRIVATE /W4 /permissive-)
else()
  target_compile_options(libatlas PRIVATE -Wall -Wextra -Wpedantic)
endif()

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/libatlasConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  ${PROJECT_SOURCE_DIR}/libatlasConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/libatlasConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libatlas
)

install(TARGETS libatlas
  EXPORT libatlasTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/libatlas
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  EXPORT libatlasTargets
  FILE libatlasTargets.cmake
  NAMESPACE libatlas::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libatlas
)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/libatlasConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/libatlasConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libatlas
)

if(LIBATLAS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LIBATLAS_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(LIBATLAS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
