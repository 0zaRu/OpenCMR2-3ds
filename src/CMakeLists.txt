set(GAME_DIR ${PROJECT_SOURCE_DIR}/game)

# Small standalone decoder, also usable by tests without SDL or game data.
add_library(opencmr2_bink STATIC video/bink_decoder.cpp)
target_include_directories(opencmr2_bink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The decompiled game. zlib is C, as in the original (ZlibZutil.cpp is C++).
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${GAME_DIR}/*.cpp)
file(GLOB ZLIB_C_SOURCES CONFIGURE_DEPENDS ${GAME_DIR}/Zlib*.cpp)
list(REMOVE_ITEM ZLIB_C_SOURCES ${GAME_DIR}/ZlibZutil.cpp)
set_source_files_properties(${ZLIB_C_SOURCES} PROPERTIES LANGUAGE C)

# Built as objects: the game and the platform layer call each other, so both
# go into one archive (opencmr2_platform) and link on every toolchain.
add_library(cmr2_game OBJECT ${GAME_SOURCES})
target_include_directories(cmr2_game PUBLIC ${GAME_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/port/compat)
target_compile_options(cmr2_game PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-x c>
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/port/types.h"
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/port/msvcrt.h"
    -fms-extensions)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC (devkitARM): the MSVC calling conventions Clang ignores, and member
    # functions declared static outside their class, which GCC rejects.
    target_compile_options(cmr2_game PRIVATE -w -D__fastcall= -D__forceinline=inline
        $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)
else()
    target_compile_options(cmr2_game PRIVATE -Wno-everything -ferror-limit=0)
endif()
target_link_libraries(cmr2_game PUBLIC SDL3::SDL3)

# These units used to call variadic sprintf through fixed-argument casts,
# which breaks the macOS ARM64 ABI. Reject incompatible function casts here
# even though other warnings in the decompiled game are disabled. Game.cpp
# still has legacy state callback casts, so it keeps the general warning policy.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(
        ${GAME_DIR}/InstallInfo.cpp ${GAME_DIR}/Graphics.cpp
        ${GAME_DIR}/Input.cpp ${GAME_DIR}/Texture.cpp
        TARGET_DIRECTORY cmr2_game
        PROPERTIES COMPILE_OPTIONS "-Wcast-function-type-strict;-Werror=cast-function-type-strict")
endif()

# The original builds these with /QIfist: their float-to-integer conversions
# (FTOI, game/FloatToInt.h) round to nearest like x87 FISTP instead of
# truncating. Same list as CMR2Decomp's scripts/build.py.
set(GAME_QIFIST_UNITS
    Race StageUI TimingUtils Frontend FrontendScreens Game GameInfo Graphics Sprite Car Sound
    CarPhysics HudDash CarEffects TrackCollision RallyData Mesh Sector StageTiming StageObjects
    SceneNode FixedPoint RallyTiming NetRace)
list(TRANSFORM GAME_QIFIST_UNITS PREPEND ${GAME_DIR}/)
list(TRANSFORM GAME_QIFIST_UNITS APPEND .cpp)
set_source_files_properties(${GAME_QIFIST_UNITS} TARGET_DIRECTORY cmr2_game
    PROPERTIES COMPILE_DEFINITIONS CMR2_QIFIST)

# Platform layer plus the game objects; tests link this too.
add_library(opencmr2_platform STATIC
    platform/sys.cpp
    platform/keys.cpp
    platform/msvcrt.cpp
    port/disk.cpp
    port/replay_disk.cpp
    $<TARGET_OBJECTS:cmr2_game>)
target_sources(opencmr2_platform PRIVATE diagnostics/observer.cpp diagnostics/timing.cpp)
target_include_directories(opencmr2_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${GAME_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/port/compat)
target_link_libraries(opencmr2_platform PUBLIC SDL3::SDL3)


if(ANDROID)
    # SDLActivity loads libmain.so and calls its SDL_main.
    add_library(opencmr2 SHARED platform/main.cpp)
    set_target_properties(opencmr2 PROPERTIES OUTPUT_NAME main)
elseif(NINTENDO_3DS)
    add_executable(opencmr2 platform/main.cpp)
    set_target_properties(opencmr2 PROPERTIES SUFFIX .elf)
else()
    add_executable(opencmr2 platform/main.cpp)
endif()
target_link_libraries(opencmr2 PRIVATE opencmr2_platform)
if(WIN32)
    target_sources(opencmr2 PRIVATE ${PROJECT_SOURCE_DIR}/res/opencmr2.rc)
    # SDL3.dll (and any other runtime DLL) next to the executable.
    add_custom_command(TARGET opencmr2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:opencmr2> $<TARGET_FILE_DIR:opencmr2>
        COMMAND_EXPAND_LISTS)
elseif(OPENCMR2_BUILT_SDL AND NOT ANDROID AND NOT NINTENDO_3DS)
    # The SDL3 built here goes next to the executable, which looks for it
    # there, so the release archives run without SDL3 installed.
    if(APPLE)
        set_property(TARGET opencmr2 APPEND PROPERTY BUILD_RPATH "@executable_path")
    else()
        set_property(TARGET opencmr2 APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    endif()
    add_custom_command(TARGET opencmr2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_SONAME_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:opencmr2>)
endif()

if(NOT NINTENDO_3DS)
# ---- shaders (GLSL -> SPIR-V, plus MSL for Metal; embedded) ----------------------
# SDL_GPU takes SPIR-V on Vulkan (Linux, Windows) and MSL on Metal (macOS).
# The MSL comes from the same SPIR-V through SPIRV-Cross; the SPIR-V bindings
# already follow SDL_GPU's resource order, so they become the Metal indices.
option(OPENCMR2_SHADERS_MSL "Also build Metal (MSL) shaders" ${APPLE})
# Recent glslang releases use glslang.exe; older packages keep glslangValidator.
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang REQUIRED)
if(OPENCMR2_SHADERS_MSL)
    find_program(SPIRV_CROSS_EXECUTABLE spirv-cross)
    if(SPIRV_CROSS_EXECUTABLE)
        set(SPIRV_CROSS ${SPIRV_CROSS_EXECUTABLE})
    else()
        message(STATUS "OpenCMR2: building spirv-cross from source")
        include(FetchContent)
        # The command-line tool needs the GLSL, HLSL, MSL, C++ and reflection back ends.
        foreach(option SPIRV_CROSS_ENABLE_TESTS SPIRV_CROSS_ENABLE_C_API SPIRV_CROSS_SHARED)
            set(${option} OFF CACHE BOOL "" FORCE)
        endforeach()
        set(SPIRV_CROSS_CLI ON CACHE BOOL "" FORCE)
        set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
        set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
        FetchContent_Declare(SPIRV-Cross
            GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
            GIT_TAG vulkan-sdk-1.4.363.0
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SPIRV-Cross)
        set(SPIRV_CROSS $<TARGET_FILE:spirv-cross>)
    endif()
endif()

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders)
set(SHADER_SOURCES fixed3d.vert fixedtl.vert fixed.frag clear.vert clear.frag)
set(SHADER_FILES)
foreach(shader IN LISTS SHADER_SOURCES)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.0 -o ${output} ${SHADER_DIR}/${shader}
        DEPENDS ${SHADER_DIR}/${shader} ${SHADER_DIR}/common.glsl
        COMMENT "Compiling shader ${shader}")
    list(APPEND SHADER_FILES ${output})
    if(OPENCMR2_SHADERS_MSL)
        set(msl ${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.msl)
        add_custom_command(
            OUTPUT ${msl}
            COMMAND ${SPIRV_CROSS} --msl --msl-version 20100 --msl-decoration-binding --output ${msl} ${output}
            DEPENDS ${output}
            COMMENT "Translating shader ${shader} to MSL")
        list(APPEND SHADER_FILES ${msl})
    endif()
endforeach()
set(SHADER_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders_spirv.h)
string(REPLACE ";" "|" SHADER_LIST "${SHADER_FILES}")
add_custom_command(
    OUTPUT ${SHADER_HEADER}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${SHADER_HEADER} -DINPUTS=${SHADER_LIST} -P ${PROJECT_SOURCE_DIR}/cmake/embed_spirv.cmake
    VERBATIM
    DEPENDS ${SHADER_FILES} ${PROJECT_SOURCE_DIR}/cmake/embed_spirv.cmake
    COMMENT "Embedding shaders")
endif()

# ---- window icon (embedded) ------------------------------------------------------
# res/opencmr2.png is the source; the derived icons are regenerated with:
#   magick res/opencmr2.png -define icon:auto-resize=256,128,96,64,48,40,32,24,20,16 res/opencmr2.ico
#   magick res/opencmr2.png -resize 128x128 -type TrueColor BMP3:res/opencmr2_icon.bmp
#   magick res/opencmr2.png -resize 256x256 -strip PNG24:res/opencmr2_256.png
# The BMP is the window icon; the PNG goes to the Linux icon theme with the
# applications menu entry.
set(ICON_BMP ${PROJECT_SOURCE_DIR}/res/opencmr2_icon.bmp)
set(ICON_PNG ${PROJECT_SOURCE_DIR}/res/opencmr2_256.png)
set(ICON_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/opencmr2_icon.h)
add_custom_command(
    OUTPUT ${ICON_HEADER}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${ICON_HEADER} "-DINPUTS=${ICON_BMP}|${ICON_PNG}" -P ${PROJECT_SOURCE_DIR}/cmake/embed_spirv.cmake
    VERBATIM
    DEPENDS ${ICON_BMP} ${ICON_PNG} ${PROJECT_SOURCE_DIR}/cmake/embed_spirv.cmake
    COMMENT "Embedding the window icon")

# Applications menu entry: on its own, as it includes the real <windows.h>
# that port/compat replaces for the game.
add_library(opencmr2_shortcut STATIC platform/menu_shortcut.cpp ${ICON_HEADER})
target_include_directories(opencmr2_shortcut PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(opencmr2_shortcut PUBLIC SDL3::SDL3)
if(WIN32)
    target_link_libraries(opencmr2_shortcut PRIVATE ole32 uuid shell32)
endif()
target_link_libraries(opencmr2_platform PUBLIC opencmr2_shortcut)

# ---- network (UDP with ENet, third_party/enet) -------------------------------------
set(ENET_DIR ${PROJECT_SOURCE_DIR}/third_party/enet)
add_library(enet STATIC
    ${ENET_DIR}/callbacks.c ${ENET_DIR}/compress.c ${ENET_DIR}/host.c ${ENET_DIR}/list.c
    ${ENET_DIR}/packet.c ${ENET_DIR}/peer.c ${ENET_DIR}/protocol.c ${ENET_DIR}/unix.c ${ENET_DIR}/win32.c)
target_include_directories(enet PUBLIC ${ENET_DIR}/include)
if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
elseif(NINTENDO_3DS)
    # libctru's sockets (soc:u): no sendmsg/recvmsg (unix.c gathers the
    # buffers itself there) and no msg_flags.
    target_compile_definitions(enet PRIVATE HAS_FCNTL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
        HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_SOCKLEN_T=1)
    target_link_libraries(enet PUBLIC ctru)
else()
    target_compile_definitions(enet PRIVATE HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
        HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
endif()

# Sessions over ENet, without SDL: tests run hosts and clients in one process.
add_library(opencmr2_net STATIC net/net_session.cpp)
target_include_directories(opencmr2_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(opencmr2_net PUBLIC enet Threads::Threads)
target_sources(opencmr2_platform PRIVATE net/net_api.cpp)
target_link_libraries(opencmr2_platform PRIVATE opencmr2_net)

# ---- platform subsystems ---------------------------------------------------------
target_sources(opencmr2_platform PRIVATE
    render/gfx_core.cpp
    ${ICON_HEADER}
    audio/audio_sdl.cpp
    input/input_sdl.cpp
    video/movie.cpp)
if(NOT NINTENDO_3DS)
    target_sources(opencmr2_platform PRIVATE platform/stagemap_none.cpp)
endif()
target_include_directories(opencmr2_platform PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(opencmr2_platform PRIVATE opencmr2_bink)

if(NINTENDO_3DS)
    # citro3d renderer; its vertex shader is assembled by picasso and embedded.
    set(PICA_SHADER ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/fixed.v.pica
                    ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/fixedtl.v.pica
                    ${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/fixedmin.v.pica)
    set(PICA_BINARY ${CMAKE_CURRENT_BINARY_DIR}/shaders/fixed.shbin)
    set(PICA_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders_pica.h)
    add_custom_command(
        OUTPUT ${PICA_BINARY}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${CTR_PICASSO_EXE} -o ${PICA_BINARY} ${PICA_SHADER}
        DEPENDS ${PICA_SHADER}
        COMMENT "Assembling the PICA shaders")
    add_custom_command(
        OUTPUT ${PICA_HEADER}
        COMMAND ${CMAKE_COMMAND} -DOUTPUT=${PICA_HEADER} -DINPUTS=${PICA_BINARY} -P ${PROJECT_SOURCE_DIR}/cmake/embed_spirv.cmake
        VERBATIM
        DEPENDS ${PICA_BINARY} ${PROJECT_SOURCE_DIR}/cmake/embed_spirv.cmake
        COMMENT "Embedding the PICA shader")
    target_sources(opencmr2_platform PRIVATE platform/ctr.cpp platform/ctr_map.cpp render/gfx_c3d.cpp ${PICA_HEADER})
    target_link_libraries(opencmr2_platform PUBLIC citro3d ctru m)

    # Homebrew Launcher package (.3dsx with its .smdh) and the installable CIA.
    set(CTR_RES ${PROJECT_SOURCE_DIR}/res/3ds)
    ctr_generate_smdh(${CMAKE_CURRENT_BINARY_DIR}/opencmr2.smdh
        NAME "OpenCMR2"
        DESCRIPTION "Colin McRae Rally 2.0 (needs the PC game files)"
        AUTHOR "OpenCMR2"
        ICON ${CTR_RES}/icon.png)
    ctr_create_3dsx(opencmr2 SMDH ${CMAKE_CURRENT_BINARY_DIR}/opencmr2.smdh)
    include(${PROJECT_SOURCE_DIR}/cmake/3ds_cia.cmake)
else()
    target_sources(opencmr2_platform PRIVATE render/gfx_gpu.cpp ${SHADER_HEADER})
endif()

# The renderer's test scene for the 3DS (tests/3ds/gfx_scene.cpp).
if(NINTENDO_3DS)
    add_executable(opencmr2_gfx_scene ${PROJECT_SOURCE_DIR}/tests/3ds/gfx_scene.cpp)
    set_target_properties(opencmr2_gfx_scene PROPERTIES SUFFIX .elf)
    target_link_libraries(opencmr2_gfx_scene PRIVATE opencmr2_platform)
    ctr_create_3dsx(opencmr2_gfx_scene SMDH ${CMAKE_CURRENT_BINARY_DIR}/opencmr2.smdh)
    add_executable(opencmr2_gpu_bisect ${PROJECT_SOURCE_DIR}/tests/3ds/gpu_bisect.cpp)
    set_target_properties(opencmr2_gpu_bisect PROPERTIES SUFFIX .elf)
    target_link_libraries(opencmr2_gpu_bisect PRIVATE opencmr2_platform)
    ctr_create_3dsx(opencmr2_gpu_bisect SMDH ${CMAKE_CURRENT_BINARY_DIR}/opencmr2.smdh)
    add_executable(opencmr2_gfx_stress ${PROJECT_SOURCE_DIR}/tests/3ds/gfx_scene.cpp)
    set_target_properties(opencmr2_gfx_stress PROPERTIES SUFFIX .elf)
    target_compile_definitions(opencmr2_gfx_stress PRIVATE GFX_STRESS)
    target_link_libraries(opencmr2_gfx_stress PRIVATE opencmr2_platform)
    ctr_create_3dsx(opencmr2_gfx_stress SMDH ${CMAKE_CURRENT_BINARY_DIR}/opencmr2.smdh)
endif()

# Host tests that run game code, as .3dsx files for an emulator or a console
# (tests/3ds/run_test.cpp): the game's arithmetic built by devkitARM's GCC.
if(NINTENDO_3DS)
    foreach(test math_tables car_scheduler)
        add_executable(opencmr2_test_${test} ${PROJECT_SOURCE_DIR}/tests/${test}.cpp
            ${PROJECT_SOURCE_DIR}/tests/3ds/run_test.cpp)
        set_target_properties(opencmr2_test_${test} PROPERTIES SUFFIX .elf)
        set_source_files_properties(${PROJECT_SOURCE_DIR}/tests/${test}.cpp TARGET_DIRECTORY opencmr2_test_${test}
            PROPERTIES COMPILE_DEFINITIONS main=test_main)
        target_link_libraries(opencmr2_test_${test} PRIVATE opencmr2_platform)
        ctr_create_3dsx(opencmr2_test_${test} NOSMDH)
    endforeach()
    target_compile_definitions(opencmr2_test_math_tables PRIVATE TEST_ARGS="sdmc:/math-tables.bin")
    target_compile_definitions(opencmr2_test_car_scheduler PRIVATE TEST_NO_ARGUMENTS)

    # The stage probe (needs the game files on the SD card): menus, loading
    # and a stage with a controlled clock (tests/3ds/stage_probe_main.cpp).
    add_executable(opencmr2_stage_probe ${PROJECT_SOURCE_DIR}/tests/stage_probe.cpp
        ${PROJECT_SOURCE_DIR}/tests/3ds/stage_probe_main.cpp)
    set_target_properties(opencmr2_stage_probe PROPERTIES SUFFIX .elf)
    set_source_files_properties(${PROJECT_SOURCE_DIR}/tests/stage_probe.cpp TARGET_DIRECTORY opencmr2_stage_probe
        PROPERTIES COMPILE_DEFINITIONS main=test_main)
    target_include_directories(opencmr2_stage_probe PRIVATE ${PROJECT_SOURCE_DIR}/game)
    target_compile_options(opencmr2_stage_probe PRIVATE -fms-extensions -fpermissive -w
        "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/port/types.h" -D__fastcall= -D__forceinline=inline)
    target_link_libraries(opencmr2_stage_probe PRIVATE opencmr2_platform)
    ctr_create_3dsx(opencmr2_stage_probe NOSMDH)
endif()
