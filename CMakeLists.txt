cmake_minimum_required(VERSION 3.16)
project(SlimyJourney CXX)

find_package(Threads REQUIRED)

# Ninja / MinGW Make : avoid empty CMAKE_BUILD_TYPE (raylib then warns "not set").
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# raylib (client + map editor)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
set(SUPPORT_MODULE_RAUDIO OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(raylib)

# enet — UDP networking (client + server)
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG v1.3.18
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(enet)

set(SHARED_SRC
    src/physics/Body.cpp
    src/physics/SoftBody.cpp
    src/physics/Collision.cpp
    src/physics/World.cpp
    src/physics/Broadphase.cpp
    src/game/Slime.cpp
    src/game/Scene.cpp
    src/game/MapIO.cpp
    src/net/NetServer.cpp
    src/net/Room.cpp
)

set(CLIENT_SRC
    src/main.cpp
    src/render/Renderer.cpp
    src/net/NetClient.cpp
    ${SHARED_SRC}
)
add_executable(slimyjourney ${CLIENT_SRC})
target_include_directories(slimyjourney PRIVATE src ${enet_SOURCE_DIR}/include)
target_link_libraries(slimyjourney PRIVATE raylib enet)

set(MAPEDIT_SRC
    src/tools/map_editor.cpp
    src/game/MapIO.cpp
    src/game/Scene.cpp
    src/render/Renderer.cpp
    src/physics/Body.cpp
    src/physics/SoftBody.cpp
    src/physics/Collision.cpp
    src/physics/World.cpp
    src/physics/Broadphase.cpp
    src/game/Slime.cpp
)
add_executable(slimyjourney_mapedit ${MAPEDIT_SRC})
target_include_directories(slimyjourney_mapedit PRIVATE src)
target_link_libraries(slimyjourney_mapedit PRIVATE raylib)

set(SERVER_SRC
    src/server_main.cpp
    ${SHARED_SRC}
)
add_executable(slimyjourney_server ${SERVER_SRC})
target_include_directories(slimyjourney_server PRIVATE src ${enet_SOURCE_DIR}/include)
target_link_libraries(slimyjourney_server PRIVATE enet Threads::Threads)

if(MSVC)
    target_compile_options(slimyjourney        PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_options(slimyjourney_server PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_options(slimyjourney_mapedit PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(slimyjourney        PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(slimyjourney_server PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(slimyjourney_mapedit PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(slimyjourney        PRIVATE winmm ws2_32 winhttp)
    target_link_libraries(slimyjourney_server PRIVATE winmm ws2_32 winhttp)
    target_link_libraries(slimyjourney_mapedit PRIVATE winmm ws2_32)
    target_compile_definitions(slimyjourney        PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_definitions(slimyjourney_server PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_definitions(slimyjourney_mapedit PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    set_target_properties(slimyjourney PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(slimyjourney PRIVATE
            /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    else()
        target_link_options(slimyjourney PRIVATE -mwindows)
    endif()
endif()
