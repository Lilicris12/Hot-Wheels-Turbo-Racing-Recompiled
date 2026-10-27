# Builds N64Recomp and RSPRecomp for the host machine and runs them on the user's ROM to
# produce RecompiledFuncs/ and rsp/aspMain.cpp. This runs at configure time so the generated
# file list is known when targets are defined, and so cross-builds (iOS, Android) still run
# the recompilers natively on the build machine.
#
# Inputs:
#   HWTR_ROM             path to the Hot Wheels Turbo Racing (USA) ROM
#   HWTR_REGENERATE      force regeneration even if outputs exist
#   N64RECOMP_EXECUTABLE / RSPRECOMP_EXECUTABLE   optional prebuilt tools

set(HWTR_FUNCS_DIR "${CMAKE_SOURCE_DIR}/RecompiledFuncs")
set(HWTR_RSP_DIR "${CMAKE_SOURCE_DIR}/rsp")
set(HWTR_ROM_EXPECTED_SHA1 "94913a07e49005ffd43dfdfa16ff5862bdb93748")

set(_hwtr_outputs_present FALSE)
if (EXISTS "${HWTR_FUNCS_DIR}/funcs.h" AND EXISTS "${HWTR_FUNCS_DIR}/recomp_overlays.inl" AND EXISTS "${HWTR_RSP_DIR}/aspMain.cpp")
    set(_hwtr_outputs_present TRUE)
endif()

# Regenerate when the inputs are newer than the last generation.
set(_hwtr_stamp "${HWTR_FUNCS_DIR}/.generated")
foreach(_input hwtr.us.toml aspMain.us.toml symbols/hwtr.syms.toml)
    if (_hwtr_outputs_present AND EXISTS "${_hwtr_stamp}" AND "${CMAKE_SOURCE_DIR}/${_input}" IS_NEWER_THAN "${_hwtr_stamp}")
        message(STATUS "${_input} changed, regenerating recompiled code")
        set(_hwtr_outputs_present FALSE)
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${_input}")
endforeach()

if (_hwtr_outputs_present AND NOT HWTR_REGENERATE)
    message(STATUS "Using existing recompiled code in ${HWTR_FUNCS_DIR}")
    return()
endif()

if (NOT EXISTS "${HWTR_ROM}")
    message(FATAL_ERROR
        "Recompiled code has not been generated yet and no ROM was found at:\n"
        "  ${HWTR_ROM}\n"
        "Copy your Hot Wheels Turbo Racing (USA) ROM there (big-endian .z64), or pass "
        "-DHWTR_ROM=/path/to/rom.z64. See BUILDING.md.")
endif()

file(SHA1 "${HWTR_ROM}" _rom_sha1)
if (NOT _rom_sha1 STREQUAL HWTR_ROM_EXPECTED_SHA1)
    message(FATAL_ERROR
        "${HWTR_ROM} has SHA-1 ${_rom_sha1}, expected ${HWTR_ROM_EXPECTED_SHA1}.\n"
        "Only the USA release in big-endian (.z64) byte order is supported. "
        "tools/rom_info.py can convert .n64/.v64 dumps.")
endif()

# The recompiler configs refer to the ROM by this name, relative to the source dir.
set(_rom_link "${CMAKE_SOURCE_DIR}/hwtr.us.z64")
get_filename_component(_rom_real "${HWTR_ROM}" REALPATH)
get_filename_component(_rom_link_real "${_rom_link}" REALPATH)
if (NOT _rom_real STREQUAL _rom_link_real)
    file(COPY_FILE "${HWTR_ROM}" "${_rom_link}")
endif()

if (NOT N64RECOMP_EXECUTABLE OR NOT RSPRECOMP_EXECUTABLE)
    set(_tools_src "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp")
    set(_tools_bin "${CMAKE_BINARY_DIR}/host-tools")
    if (NOT EXISTS "${_tools_src}/CMakeLists.txt")
        message(FATAL_ERROR "Submodules are missing. Run: git submodule update --init --recursive")
    endif()

    message(STATUS "Building N64Recomp and RSPRecomp for the host")
    # Deliberately no toolchain file: these must run on the build machine.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_tools_src}" -B "${_tools_bin}"
                -DCMAKE_BUILD_TYPE=Release
                "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${_tools_bin}/bin"
                "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${_tools_bin}/bin"
        RESULT_VARIABLE _res)
    if (NOT _res EQUAL 0)
        message(FATAL_ERROR "Configuring N64Recomp failed")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${_tools_bin}" --config Release --target N64Recomp RSPRecomp --parallel
        RESULT_VARIABLE _res)
    if (NOT _res EQUAL 0)
        message(FATAL_ERROR "Building N64Recomp failed")
    endif()

    set(N64RECOMP_EXECUTABLE "${_tools_bin}/bin/N64Recomp${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    set(RSPRECOMP_EXECUTABLE "${_tools_bin}/bin/RSPRecomp${CMAKE_HOST_EXECUTABLE_SUFFIX}")
endif()

file(REMOVE_RECURSE "${HWTR_FUNCS_DIR}")
file(MAKE_DIRECTORY "${HWTR_FUNCS_DIR}" "${HWTR_RSP_DIR}")

message(STATUS "Recompiling the game code")
execute_process(
    COMMAND "${N64RECOMP_EXECUTABLE}" hwtr.us.toml
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _res)
if (NOT _res EQUAL 0)
    message(FATAL_ERROR "N64Recomp failed (see output above)")
endif()

message(STATUS "Recompiling the audio microcode")
execute_process(
    COMMAND "${RSPRECOMP_EXECUTABLE}" aspMain.us.toml
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _res)
if (NOT _res EQUAL 0)
    message(FATAL_ERROR "RSPRecomp failed (see output above)")
endif()

file(TOUCH "${_hwtr_stamp}")
