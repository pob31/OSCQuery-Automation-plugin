# This file is part of the OSCQuery Automation Plugin —
# https://github.com/pob31/OSCQuery-Automation-plugin
#
# Copyright (C) 2026 Pierre-Olivier Boulant, Julien Bayle
#
# The OSCQuery Automation Plugin is free software: you can redistribute it
# and/or modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation, either version 3 of the License,
# or (at your option) any later version. It is distributed in the hope that it
# will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
# of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
# Public License (LICENSE, at the repository root) for more details.
#
# SPDX-License-Identifier: GPL-3.0-or-later

# The core's tests (devplan Phase 3), a third executable beside unit.* and oscquery.*,
# sharing the suite's entry point, support headers and the mock device, and run twice by
# CTest: once under C, once under French, because ranges and the state chunk are decimals.

add_executable(oqa_core_tests
    ../TestMain.cpp
    ../support/RtAllocCounter.cpp       # the counting operator new: test executables only
    ScalingTests.cpp
    SlotTests.cpp
    StateChunkTests.cpp
    RtCheckTests.cpp
    ChangeRingTests.cpp
    TimeTagTests.cpp
    RateLimiterTests.cpp
    RegistryRig.h
    RegistryTests.cpp
    ReceiveTests.cpp
    UdpReceiveTests.cpp
    GestureTests.cpp
    LoaderTests.cpp
    StaleTests.cpp
    RetestTests.cpp
    ReidentifyTests.cpp
    ImportTests.cpp
    FailureStatesTests.cpp
    SettingsTests.cpp
    StatusTests.cpp
    TimingTests.cpp
    ServerTests.cpp
    CommandTests.cpp
    PresentationTests.cpp
    MonitorModelTests.cpp
    PatternTests.cpp
    # The Monitor's rows, juce_core only (src/editor/README.md): compiled here, not through
    # oqa::editor, whose other sources want a GUI module.
    "${CMAKE_SOURCE_DIR}/src/editor/MonitorModel.cpp")

target_include_directories(oqa_core_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
target_include_directories(oqa_core_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/doctest")

target_compile_definitions(oqa_core_tests PRIVATE
    OQA_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
    OQA_TEST_STATE_DIR="${CMAKE_SOURCE_DIR}/tests/state"
    OQA_CORE_SHARED_PATH="$<TARGET_FILE:oqa_core_shared>"
    OQA_LOCALE_FR="${OQA_LOCALE_FR}")

# The loader's tests open the shared library this build makes.
add_dependencies(oqa_core_tests oqa_core_shared)

# oqa::oscquery named here although oqa::core carries it: GNU ld scans archives once, in
# order, and CMake appends a transitive archive after the direct ones, which put the
# library (which needs juce::SHA256) after oqa_juce (which has it). Named directly it
# comes before. The same order as lib/oscquery/tests.
target_link_libraries(oqa_core_tests PRIVATE oqa::core oqa::oscquery oqa_mock_device oqa_juce oqa::warnings)

add_test(NAME core.C COMMAND oqa_core_tests --oqa-locale=C)
add_test(NAME core.fr_FR COMMAND oqa_core_tests --oqa-locale=${OQA_LOCALE_FR})
