cmake_minimum_required(VERSION 3.25)

# The target client is a 32-bit process: build a Win32 DLL.
set(CMAKE_GENERATOR_PLATFORM Win32)

project(WraithEngine LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Hooking engine (vendored).
add_subdirectory(vendor/minhook)

file(GLOB_RECURSE WRAITH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# The Features/D3D12 subtree builds a SEPARATE d3d9.dll proxy (it carries the d3d9 exports + its own
# DllMain and routes the engine through D3D9On12 onto our D3D12 device), so keep it out of Wraith.dll.
file(GLOB_RECURSE D3D12_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/Features/D3D12/*.cpp")
list(REMOVE_ITEM WRAITH_SOURCES ${D3D12_SOURCES})

add_library(Wraith SHARED ${WRAITH_SOURCES})
set_target_properties(Wraith PROPERTIES OUTPUT_NAME "Wraith" PREFIX "")

target_include_directories(Wraith PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/minhook/include")

target_link_libraries(Wraith PRIVATE minhook)
target_compile_definitions(Wraith PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# d3d9.dll: our proxy. Forwards to the real d3d9, forces D3D9On12 onto our own D3D12 device, and hosts
# the D3D12-side rendering. Lives entirely under src/Features/D3D12.
add_library(d3d9 SHARED ${D3D12_SOURCES})
set_target_properties(d3d9 PROPERTIES OUTPUT_NAME "d3d9" PREFIX "")
target_include_directories(d3d9 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(d3d9 PRIVATE d3d12)
target_link_options(d3d9 PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/Features/D3D12/Proxy/Proxy.def")
target_compile_definitions(d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# Patcher: standalone tool that adds Wraith.dll to the target executable's import table.
add_executable(Patcher "${CMAKE_CURRENT_SOURCE_DIR}/Patcher/Patcher.cpp")
set_target_properties(Patcher PROPERTIES OUTPUT_NAME "WraithPatcher")

# Deploy to the client after build (set -DCLIENT_PATH=... at configure time).
set(CLIENT_PATH "" CACHE PATH "Path to the client directory")
if(NOT CLIENT_PATH STREQUAL "")
    add_custom_command(TARGET Wraith POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:Wraith>" "${CLIENT_PATH}/Wraith.dll"
        VERBATIM)
    add_custom_command(TARGET Patcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:Patcher>" "${CLIENT_PATH}/WraithPatcher.exe"
        VERBATIM)
    add_custom_command(TARGET d3d9 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:d3d9>" "${CLIENT_PATH}/d3d9.dll"
        VERBATIM)
endif()
