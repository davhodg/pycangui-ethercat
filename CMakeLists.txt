# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
#
# The helper, two ways:
#   ecat_helper       over SOEM and a real network card -- what the plugin ships
#   ecat_helper_fake  over a scripted ring -- what the tests run against
# and the C unit tests. build/build.py runs this and copies the programs into
# the plugin's bin folder.

cmake_minimum_required(VERSION 3.28)
project(pycangui_ethercat LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

#: The SOEM release in external/SOEM, said in the helper's hello.
set(ECAT_SOEM_VERSION "2.0.1" CACHE STRING "SOEM release built against")
option(ECAT_WITH_SOEM "Build the real helper over SOEM" ON)

if(MSVC)
    set(ECAT_WARNINGS /W4 /WX /D_CRT_SECURE_NO_WARNINGS)
else()
    set(ECAT_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()

# The protocol, without the ring: shared by both helpers and the unit tests.
add_library(ecat_core STATIC helper/ecat_json.c helper/ecat_helper.c)
target_include_directories(ecat_core PUBLIC helper)
target_compile_options(ecat_core PRIVATE ${ECAT_WARNINGS})

set(ECAT_NET_LIBS)
if(WIN32)
    list(APPEND ECAT_NET_LIBS ws2_32)
else()
    find_package(Threads REQUIRED)
    list(APPEND ECAT_NET_LIBS Threads::Threads)
endif()

add_executable(ecat_helper_fake helper/ecat_main.c helper/ecat_port_fake.c)
target_link_libraries(ecat_helper_fake PRIVATE ecat_core ${ECAT_NET_LIBS})
target_compile_options(ecat_helper_fake PRIVATE ${ECAT_WARNINGS})

if(ECAT_WITH_SOEM)
    set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/SOEM EXCLUDE_FROM_ALL SYSTEM)
    add_executable(ecat_helper helper/ecat_main.c helper/ecat_port_soem.c)
    target_link_libraries(ecat_helper PRIVATE ecat_core soem ${ECAT_NET_LIBS})
    target_compile_options(ecat_helper PRIVATE ${ECAT_WARNINGS})
    target_compile_definitions(ecat_helper PRIVATE ECAT_SOEM_VERSION="${ECAT_SOEM_VERSION}")
    if(MSVC)
        # Npcap's DLLs load when first used, not at start-up: the helper starts
        # without Npcap and says so (ecat_port_check), rather than failing to run.
        target_link_options(ecat_helper PRIVATE /DELAYLOAD:wpcap.dll /DELAYLOAD:Packet.dll)
        target_link_libraries(ecat_helper PRIVATE delayimp)
    endif()
endif()

enable_testing()
add_executable(test_ecat tests_c/test_ecat.c helper/ecat_port_fake.c)
target_link_libraries(test_ecat PRIVATE ecat_core)
target_compile_options(test_ecat PRIVATE ${ECAT_WARNINGS})
add_test(NAME test_ecat COMMAND test_ecat)
