//===------------------ OxCamlDemangleTest.cpp ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Demangle/Demangle.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <cstdlib>

TEST(OxCamlDemangle, Success) {
    char *Demangled = nullptr;

    // NamedFunction
    Demangled = llvm::oxcamlDemangle("_OM4MainF9say_hello_345_code");
    EXPECT_STREQ(Demangled, "Main.say_hello");
    std::free(Demangled);

    // Nested Modules with function
    Demangled = llvm::oxcamlDemangle("_OM4Main4TestF3foo_345");
    EXPECT_STREQ(Demangled, "Main.Test.foo");
    std::free(Demangled);

    // Nested Modules with function
    Demangled = llvm::oxcamlDemangle("_OM4Demo8PositiveF4make_2_code");
    EXPECT_STREQ(Demangled, "Demo.Positive.make");
    std::free(Demangled);

    Demangled = llvm::oxcamlDemangle(    "_OM12Stdlib__ListF3map_113_code");
    EXPECT_STREQ(Demangled, "Stdlib__List.map");
    std::free(Demangled);
}

TEST(OxCamlDemangle, PathItemPrefixes) {
    char *Demangled = nullptr;

    // Module prefix 'M'
    Demangled = llvm::oxcamlDemangle("_OM4Main_123");
    EXPECT_STREQ(Demangled, "Main");
    std::free(Demangled);

    // NamedFunction prefix 'F'
    Demangled = llvm::oxcamlDemangle("_OM4MainF3foo_456");
    EXPECT_STREQ(Demangled, "Main.foo");
    std::free(Demangled);

    // Multiple modules with function
    Demangled = llvm::oxcamlDemangle("_OM4MainM4TestF3bar_789");
    EXPECT_STREQ(Demangled, "Main.Test.bar");
    std::free(Demangled);

    // PartialFunction prefix 'P' (no identifier)
    Demangled = llvm::oxcamlDemangle("_OM4MainF3fooP_100");
    EXPECT_STREQ(Demangled, "Main.foo(partially_applied)");
    std::free(Demangled);

    // AnonymousFunction prefix 'L' (filename with unicode escaping for '.')
    // "main.ml_10_20": 'main' (4 chars) + '.' (encoded as 2e) + 'ml_10_20' (8 chars)
    // Coded: "E2e" (E=4 in base-26), Raw: "mainml_10_20" (12 chars), Total: 16
    Demangled = llvm::oxcamlDemangle("_OM4MainLu16E2e_mainml_10_20_200");
    EXPECT_STREQ(Demangled, "Main.fn(main.ml:10:20)");
    std::free(Demangled);

    // AnonymousModule prefix 'S' (filename with unicode escaping for '.')
    // "test.ml_5_15": 'test' (4 chars) + '.' (encoded as 2e) + 'ml_5_15' (7 chars)
    // Coded: "E2e" (E=4), Raw: "testml_5_15" (11 chars), Total: 15
    Demangled = llvm::oxcamlDemangle("_OM4MainSu15E2e_testml_5_15_300");
    EXPECT_STREQ(Demangled, "Main.fn(test.ml:5:15)");
    std::free(Demangled);
}

TEST(OxCamlDemangle, UnicodeEscaping) {
    // Test will be added when we have valid unicode-escaped identifier examples
    // Note: OCaml identifiers have restrictions on valid characters
    EXPECT_TRUE(true);
}

TEST(OxCamlDemangle, MixedEncoding) {
    char *Demangled = nullptr;

    // Mix of prefixed modules and nested modules
    Demangled = llvm::oxcamlDemangle("_OM4MainM8PositiveF4make_2");
    EXPECT_STREQ(Demangled, "Main.Positive.make");
    std::free(Demangled);

    // Actual symbols from Stdlib with nested module names using __
    Demangled = llvm::oxcamlDemangle("_OM12Stdlib__ListF3map_123");
    EXPECT_STREQ(Demangled, "Stdlib__List.map");
    std::free(Demangled);
}

TEST(OxCamlDemangle, Invalid) {
    char *Demangled = nullptr;

    // Invalid prefix.
    Demangled = llvm::oxcamlDemangle("_ABCDEF");
    EXPECT_EQ(Demangled, nullptr);

    // Correct prefix but still invalid.
    Demangled = llvm::oxcamlDemangle("_Ocaml");
    EXPECT_EQ(Demangled, nullptr);
}