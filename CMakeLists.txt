cmake_minimum_required(VERSION 3.24)

project(R2T2-ws VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(R2T2_ENABLE_METAL "Enable the llama.cpp Metal backend" ${APPLE})
option(R2T2_ENABLE_CUDA "Enable the llama.cpp CUDA backend" OFF)
option(R2T2_ENABLE_VULKAN "Enable the llama.cpp Vulkan backend" OFF)
option(R2T2_ENABLE_CPU "Enable the llama.cpp CPU backend" ON)
option(R2T2_BUILD_TESTS "Build deterministic unit tests" ${BUILD_TESTING})
set(R2T2_LLAMA_CPP_SOURCE_DIR "" CACHE PATH "Existing pinned llama.cpp source tree")
set(R2T2_IXWEBSOCKET_SOURCE_DIR "" CACHE PATH "Existing IXWebSocket source tree")
set(R2T2_NLOHMANN_JSON_SOURCE_DIR "" CACHE PATH "Existing pinned nlohmann/json source tree")

if(NOT R2T2_ENABLE_CPU)
    message(FATAL_ERROR "The pinned llama.cpp build always includes CPU fallback; R2T2_ENABLE_CPU must remain ON")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_MTMD ON CACHE BOOL "" FORCE)
set(MTMD_VIDEO OFF CACHE BOOL "" FORCE)
set(GGML_METAL ${R2T2_ENABLE_METAL} CACHE BOOL "" FORCE)
set(GGML_METAL_EMBED_LIBRARY ${R2T2_ENABLE_METAL} CACHE BOOL "" FORCE)
set(GGML_CUDA ${R2T2_ENABLE_CUDA} CACHE BOOL "" FORCE)
set(GGML_VULKAN ${R2T2_ENABLE_VULKAN} CACHE BOOL "" FORCE)

if(R2T2_LLAMA_CPP_SOURCE_DIR)
    add_subdirectory("${R2T2_LLAMA_CPP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/llama-cpp-build" EXCLUDE_FROM_ALL)
    set(R2T2_LLAMA_SOURCE "${R2T2_LLAMA_CPP_SOURCE_DIR}")
else()
    FetchContent_Declare(
        llama_cpp
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG ad6c66839af3c5646fba8c6c2e2087a1e4e38948
        GIT_SHALLOW FALSE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(llama_cpp)
    set(R2T2_LLAMA_SOURCE "${llama_cpp_SOURCE_DIR}")
endif()

set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(USE_WS OFF CACHE BOOL "" FORCE)
set(USE_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_DEMO OFF CACHE BOOL "" FORCE)

if(R2T2_IXWEBSOCKET_SOURCE_DIR)
    add_subdirectory("${R2T2_IXWEBSOCKET_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/ixwebsocket-build" EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(
        ixwebsocket
        GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
        GIT_TAG 64fae7676bd8fe31f7cb4bcde7a6841892dad65e
        GIT_SHALLOW FALSE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(ixwebsocket)
endif()

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
if(R2T2_NLOHMANN_JSON_SOURCE_DIR)
    add_subdirectory(
        "${R2T2_NLOHMANN_JSON_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/_deps/nlohmann-json-build"
        EXCLUDE_FROM_ALL
    )
else()
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG 55f93686c01528224f448c19128836e7df245f72
        GIT_SHALLOW FALSE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mtmd PRIVATE -Wno-deprecated-enum-enum-conversion)
    target_compile_options(ixwebsocket PRIVATE
        -Wno-unused-const-variable
        -Wno-unused-parameter
        -Wno-unused-private-field
    )
endif()

add_library(r2t2_core
    src/config.cpp
    src/inference.cpp
    src/protocol.cpp
    src/session.cpp
)
target_include_directories(r2t2_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${R2T2_LLAMA_SOURCE}/include" "${R2T2_LLAMA_SOURCE}/tools/mtmd"
)
target_link_libraries(r2t2_core PUBLIC nlohmann_json::nlohmann_json PRIVATE llama mtmd)
target_compile_features(r2t2_core PUBLIC cxx_std_20)
target_compile_definitions(r2t2_core PUBLIC
    R2T2_VERSION="${PROJECT_VERSION}"
    R2T2_LLAMA_CPP_COMMIT="ad6c66839af3c5646fba8c6c2e2087a1e4e38948"
    R2T2_METAL_ENABLED=$<BOOL:${R2T2_ENABLE_METAL}>
    R2T2_CUDA_ENABLED=$<BOOL:${R2T2_ENABLE_CUDA}>
    R2T2_VULKAN_ENABLED=$<BOOL:${R2T2_ENABLE_VULKAN}>
)

add_library(r2t2_transport src/websocket_server.cpp)
target_link_libraries(r2t2_transport PUBLIC r2t2_core PRIVATE ixwebsocket::ixwebsocket)
target_compile_features(r2t2_transport PUBLIC cxx_std_20)

add_executable(r2t2-ws src/main.cpp)
target_link_libraries(r2t2-ws PRIVATE r2t2_transport ixwebsocket::ixwebsocket)
target_compile_features(r2t2-ws PRIVATE cxx_std_20)

if(APPLE)
    target_link_libraries(r2t2_core PUBLIC "-framework Security")
elseif(WIN32)
    target_link_libraries(r2t2_core PUBLIC bcrypt)
    target_link_libraries(r2t2-ws PRIVATE psapi)
endif()

if(MSVC)
    target_compile_options(r2t2_core PRIVATE /W4 /permissive-)
    target_compile_options(r2t2_transport PRIVATE /W4 /permissive-)
    target_compile_options(r2t2-ws PRIVATE /W4 /permissive-)
else()
    target_compile_options(r2t2_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(r2t2_transport PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(r2t2-ws PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS r2t2-ws RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(R2T2_BUILD_TESTS)
    add_executable(r2t2-unit-tests tests/unit_tests.cpp)
    target_link_libraries(r2t2-unit-tests PRIVATE r2t2_core)
    target_compile_features(r2t2-unit-tests PRIVATE cxx_std_20)
    add_test(NAME r2t2-unit-tests COMMAND r2t2-unit-tests)

    add_executable(r2t2-ws-tests tests/websocket_tests.cpp)
    target_link_libraries(r2t2-ws-tests PRIVATE r2t2_transport ixwebsocket::ixwebsocket)
    target_compile_features(r2t2-ws-tests PRIVATE cxx_std_20)
    add_test(NAME r2t2-ws-tests COMMAND r2t2-ws-tests)
    set_tests_properties(r2t2-ws-tests PROPERTIES SKIP_RETURN_CODE 77)
endif()
