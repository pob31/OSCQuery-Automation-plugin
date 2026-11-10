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

# The library's own tests, in a second executable so the library can leave with them in
# Phase 8. They share the suite's entry point (tests/TestMain.cpp: doctest's main, one JUCE
# initialiser, the --oqa-locale switch), its support headers and the mock device, and run
# twice under CTest like unit.*: once under C, once under French.

add_executable(oqa_oscquery_tests
    "${CMAKE_SOURCE_DIR}/tests/TestMain.cpp"
    OscValueTests.cpp
    OscCodecTests.cpp
    HostInfoTests.cpp
    TreeTests.cpp
    HttpGetTests.cpp
    DeviceTests.cpp
    MdnsTests.cpp
    AnswerPacerTests.cpp
    FinderTests.cpp
    TransportTests.cpp
    FixedEncoderTests.cpp
    RawUdpSenderTests.cpp
    RecentsTests.cpp
    LargeTreeTests.cpp
    ServerTests.cpp
    "${CMAKE_SOURCE_DIR}/tests/support/TreeGen.h")

target_include_directories(oqa_oscquery_tests PRIVATE "${CMAKE_SOURCE_DIR}/tests")
target_include_directories(oqa_oscquery_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/doctest")

target_compile_definitions(oqa_oscquery_tests PRIVATE
    OQA_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
    OQA_LOCALE_FR="${OQA_LOCALE_FR}")

target_link_libraries(oqa_oscquery_tests PRIVATE oqa::oscquery oqa_mock_device oqa_juce oqa::warnings)

add_test(NAME oscquery.C COMMAND oqa_oscquery_tests --oqa-locale=C)
add_test(NAME oscquery.fr_FR COMMAND oqa_oscquery_tests --oqa-locale=${OQA_LOCALE_FR})
