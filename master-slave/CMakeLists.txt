cmake_minimum_required(VERSION 3.16)
project(tabu_search CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

find_package(MPI REQUIRED)
find_package(OpenMP)

FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.3.2
)
FetchContent_MakeAvailable(CLI11)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG        v2.1.2
    GIT_SHALLOW    TRUE
)
set(MI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mimalloc)

# PGO options (set via -DPGO_GENERATE=ON or -DPGO_USE=ON)
option(PGO_GENERATE "Instrument binary for PGO profile collection" OFF)
option(PGO_USE      "Compile with PGO profile data"                OFF)
set(PGO_DATA_DIR "${CMAKE_BINARY_DIR}/pgo-data"
    CACHE PATH "Directory for PGO profile data files")

add_executable(tabu_search
    src/main.cpp
    src/parallel.cpp
    src/config.cpp
    src/logger.cpp
    src/clusterize.cpp
    src/routes.cpp
    src/neighborhoods.cpp
    src/solutions.cpp
)

target_include_directories(tabu_search PRIVATE src)
target_link_libraries(tabu_search PRIVATE CLI11::CLI11 nlohmann_json::nlohmann_json MPI::MPI_CXX mimalloc-static)

if(OpenMP_CXX_FOUND)
    target_link_libraries(tabu_search PRIVATE OpenMP::OpenMP_CXX)
endif()

if(MSVC)
    target_compile_options(tabu_search PRIVATE /W3 /O2)
else()
    target_compile_options(tabu_search PRIVATE
        -Wall -Wextra -O3 -march=native -flto -fno-rtti)
    target_link_options(tabu_search PRIVATE -flto)

    if(PGO_GENERATE)
        file(MAKE_DIRECTORY "${PGO_DATA_DIR}")
        target_compile_options(tabu_search PRIVATE -fprofile-generate=${PGO_DATA_DIR})
        target_link_options(tabu_search   PRIVATE -fprofile-generate=${PGO_DATA_DIR})
    elseif(PGO_USE)
        target_compile_options(tabu_search PRIVATE
            -fprofile-use=${PGO_DATA_DIR} -fprofile-correction)
        target_link_options(tabu_search   PRIVATE -fprofile-use=${PGO_DATA_DIR})
    endif()
endif()
