#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
#

zephyr_include_directories(include)

add_subdirectory(ext)
add_subdirectory(lib)
add_subdirectory(subsys)
add_subdirectory(drivers)

# Add the mandatory nrfxlib repository
set(               NRFXLIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../nrfxlib CACHE INTERNAL "")
assert_exists(     NRFXLIB_DIR)
add_subdirectory(${NRFXLIB_DIR} ${PROJECT_BINARY_DIR}/nrfxlib)
