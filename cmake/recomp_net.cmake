# recomp_net.cmake — optional recomp-net delay-sync for gbarecomp hosts.
#
# recomp-net lives as a git submodule at lib/recomp-net (track branch main).
# Games that want multiplayer / link-cable netplay link the STATIC target
# `recomp_net` and the thin GBA facade `gbarecomp_netplay`, then implement the
# host loop in lib/recomp-net/docs/host_integration.md:
#   pump → try_admit → step frame (SIO via FrameLinkPartner) → advance
#
# Usage from a game CMakeLists (after include(runtime.cmake) / gbarecomp):
#
#   gbarecomp_enable_recomp_net(EmeraldRecomp)
#
# Or configure the core with:
#   cmake -DGBARECOMP_ENABLE_NET=ON [-DGBARECOMP_NET_ICE=ON] ...
#
# ICE / WAN (libjuice):
#   cmake -DGBARECOMP_NET_ICE=ON ...
# TURN testing (relay-only; both peers):
#   cmake -DGBARECOMP_NET_FORCE_TURN=ON -DGBARECOMP_NET_ICE=ON ...

get_filename_component(GBARECOMP_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(GBARECOMP_ROOT_DIR "${GBARECOMP_CMAKE_DIR}/.." ABSOLUTE)

if(NOT GBARECOMP_RECOMP_NET_ROOT)
    set(GBARECOMP_RECOMP_NET_ROOT "${GBARECOMP_ROOT_DIR}/lib/recomp-net")
endif()

option(GBARECOMP_ENABLE_NET
    "Build and expose recomp-net (delay-sync) + gba_netplay for game targets" OFF)
option(GBARECOMP_NET_ICE
    "Enable ICE/libjuice transport in recomp-net (may FetchContent libjuice)" OFF)
option(GBARECOMP_NET_FORCE_TURN
    "Testing: require TURN and only use typ relay ICE candidates" OFF)

function(_gbarecomp_add_recomp_net)
    if(TARGET recomp_net)
        return()
    endif()

    if(NOT EXISTS "${GBARECOMP_RECOMP_NET_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "recomp-net submodule missing at ${GBARECOMP_RECOMP_NET_ROOT}.\n"
            "Run: git submodule update --init lib/recomp-net")
    endif()

    set(RNET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RNET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(GBARECOMP_NET_ICE OR GBARECOMP_NET_FORCE_TURN)
        set(RNET_ENABLE_ICE ON CACHE BOOL "" FORCE)
        set(RNET_ICE_BUNDLE_STATIC ON CACHE BOOL "" FORCE)
        if(GBARECOMP_NET_FORCE_TURN)
            set(RNET_ICE_FORCE_TURN ON CACHE BOOL "" FORCE)
        endif()
    else()
        set(RNET_ENABLE_ICE OFF CACHE BOOL "" FORCE)
    endif()

    add_subdirectory(
        "${GBARECOMP_RECOMP_NET_ROOT}"
        "${CMAKE_BINARY_DIR}/recomp-net-build"
        EXCLUDE_FROM_ALL)

    if(NOT TARGET recomp_net)
        message(FATAL_ERROR "recomp-net CMake did not create target recomp_net")
    endif()
endfunction()

function(_gbarecomp_add_netplay_lib)
    if(TARGET gbarecomp_netplay)
        return()
    endif()
    _gbarecomp_add_recomp_net()

    if(NOT TARGET gbarecomp_gba)
        message(FATAL_ERROR
            "gbarecomp_netplay requires target gbarecomp_gba (include after "
            "the gbarecomp_gba library is defined)")
    endif()

    add_library(gbarecomp_netplay STATIC
        "${GBARECOMP_ROOT_DIR}/src/netplay/gba_netplay.cpp")
    target_include_directories(gbarecomp_netplay PUBLIC
        "${GBARECOMP_ROOT_DIR}/src/netplay"
        "${GBARECOMP_ROOT_DIR}/src/gba")
    target_link_libraries(gbarecomp_netplay PUBLIC recomp_net gbarecomp_gba)
    target_compile_definitions(gbarecomp_netplay PUBLIC GBARECOMP_NET=1)
    set_property(TARGET gbarecomp_netplay PROPERTY POSITION_INDEPENDENT_CODE ON)
endfunction()

# Link recomp-net + gba_netplay into a game / test executable.
function(gbarecomp_enable_recomp_net target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "gbarecomp_enable_recomp_net: '${target}' is not a CMake target. "
            "Call this after add_executable(${target} ...).")
    endif()
    _gbarecomp_add_netplay_lib()
    target_link_libraries(${target} PRIVATE gbarecomp_netplay)
    target_compile_definitions(${target} PRIVATE GBARECOMP_NET=1)
    if(GBARECOMP_NET_FORCE_TURN)
        if(NOT GBARECOMP_NET_ICE AND NOT RNET_ENABLE_ICE)
            message(FATAL_ERROR
                "GBARECOMP_NET_FORCE_TURN=ON requires ICE "
                "(-DGBARECOMP_NET_ICE=ON)")
        endif()
        target_compile_definitions(${target} PRIVATE GBARECOMP_NET_FORCE_TURN=1)
    endif()
    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()
endfunction()

# When GBARECOMP_ENABLE_NET=ON at configure time, build the facade library
# so game targets / tests can link it without a separate helper call.
if(GBARECOMP_ENABLE_NET)
    # Deferred: gbarecomp_gba must exist. Callers include this file after the
    # library is defined, or invoke gbarecomp_enable_recomp_net() later.
    if(TARGET gbarecomp_gba)
        _gbarecomp_add_netplay_lib()
        message(STATUS
            "GBARECOMP_ENABLE_NET: recomp-net + gba_netplay "
            "(ICE=${GBARECOMP_NET_ICE} FORCE_TURN=${GBARECOMP_NET_FORCE_TURN})")
    endif()
endif()
