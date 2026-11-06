cmake_minimum_required(VERSION 3.10)
project(gordian_linux_build CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(BLAS REQUIRED)
find_package(LAPACK REQUIRED)

add_executable(gordian_linux_build
  src/main.cpp
  src/Gordian.cpp
  src/EasyBMP.cpp
  src/EasyBMP_Geometry.cpp
)

# Armadillo 4.450 is vendored as headers under src/include.
target_include_directories(gordian_linux_build SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/src/include)
target_compile_options(gordian_linux_build PRIVATE -Wall -Wextra)
target_link_libraries(gordian_linux_build PRIVATE ${LAPACK_LIBRARIES} ${BLAS_LIBRARIES})
