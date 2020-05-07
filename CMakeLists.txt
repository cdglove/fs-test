# Copyright 2020 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
cmake_minimum_required(VERSION 3.14)
project(fs-test CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(Boost REQUIRED timer filesystem)

include(FetchContent)
FetchContent_Declare(
    llfio
    GIT_REPOSITORY https://github.com/ned14/llfio.git
    GIT_TAG        e335c6c
)
FetchContent_GetProperties(llfio)
if(NOT llfio_POPULATED)
    FetchContent_Populate(llfio)
    add_subdirectory(${llfio_SOURCE_DIR} ${llfio_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

add_executable(test-llfio test-llfio.cpp)
target_link_libraries(test-llfio PUBLIC llfio_sl Boost::timer)
add_executable(test-boost_filesystem test-boost_filesystem.cpp)
target_link_libraries(test-boost_filesystem PUBLIC Boost::timer Boost::filesystem)
if(WIN32)
    add_executable(test-win32 test-win32.cpp)
    target_link_libraries(test-win32 PUBLIC Boost::timer)
endif()