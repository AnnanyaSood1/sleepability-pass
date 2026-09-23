cmake_minimum_required(VERSION 3.20)
project(SleepabilityPass LANGUAGES CXX)

# Locate an LLVM install. Point cmake at it with:
#   -DLLVM_DIR=$(llvm-config --cmakedir)
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The pass must match LLVM's RTTI setting or it will fail to load.
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()

# Build the pass as a loadable module for opt's -load-pass-plugin.
add_library(Sleepability MODULE src/SleepabilityPass.cpp)

# On macOS, let undefined symbols resolve against the loading process (opt).
if(APPLE)
  set_target_properties(Sleepability PROPERTIES
    LINK_FLAGS "-undefined dynamic_lookup")
endif()
