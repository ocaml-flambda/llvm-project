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

/* How to reproduce the symbols tested in the present file

cat > main.ml << EOF
let say_hello () = ()

module Test = struct
  let foo () = ()
end

let main () = ignore (List.map2 (fun x -> let f y = y * x * 2 in f) [] [])

let _ = ignore (List.map say_hello [])
EOF

ocamlopt -o main main.ml
nm -pa main | awk '/_Caml.*(Main|[0-9]map_[0-9])/ { print $3 }'
*/


TEST(OxCamlDemangle, Success) {
    char *Demangled = nullptr;

    // NamedFunction
    Demangled = llvm::oxcamlDemangle("_CamlU4MainF11say_hello_0_5_code");
    EXPECT_STREQ(Demangled, "Main.say_hello");
    std::free(Demangled);

    // Nested Modules with function
    Demangled = llvm::oxcamlDemangle("_CamlU4MainM4TestF5foo_1_6_code");
    EXPECT_STREQ(Demangled, "Main.Test.foo");
    std::free(Demangled);

    // DROP: Duplicate case, see below
    // Demangled = llvm::oxcamlDemangle("_CamlU12Stdlib__ListF6map_15_113_code");
    // EXPECT_STREQ(Demangled, "Stdlib__List.map");
    // std::free(Demangled);
}

TEST(OxCamlDemangle, PathItemPrefixes) {
    char *Demangled = nullptr;

    // FIXME The point of this case is unclear to me
    // // Module prefix 'M'
    // Demangled = llvm::oxcamlDemangle("_CamlU4Main_123");
    // EXPECT_STREQ(Demangled, "Main");
    // std::free(Demangled);

    // DROP: Duplicate case
    // // NamedFunction prefix 'F'
    // Demangled = llvm::oxcamlDemangle("_CamlU4MainF3foo_456");
    // EXPECT_STREQ(Demangled, "Main.foo");
    // std::free(Demangled);

    // DROP: Duplicate case
    // // Multiple modules with function
    // Demangled = llvm::oxcamlDemangle("_CamlU4MainM4TestF3bar_789");
    // EXPECT_STREQ(Demangled, "Main.Test.bar");
    // std::free(Demangled);

    // FIXME: The current name-mangling scheme drops partial functions from the
    // suffix to replace them with a (fallback) name, so this case doesn't
    // appear in actual cases
    // PartialFunction prefix 'P' (no identifier)
    // Demangled = llvm::oxcamlDemangle("_CamlU4MainF3fooP_100");
    // EXPECT_STREQ(Demangled, "Main.foo(partially_applied)");
    // std::free(Demangled);

    // AnonymousFunction prefix 'L' (filename with escaping for '.')
    // "main.ml_7_32": 'main' (4 chars) + '.' (encoded as 2e) + 'ml_7_32' (7 chars)
    // Coded: "E2e" (E=4 in base-26), Raw: "mainml_7_32" (11 chars), Total: 15
    Demangled = llvm::oxcamlDemangle("_CamlU4MainF4mainLu15E2e_mainml_7_32F4fn_4_9_code");
    EXPECT_STREQ(Demangled, "Main.main.fn(main.ml:7:32).fn");
    std::free(Demangled);

    // FIXME: Didn't manage to create a simple reproducer
    // AnonymousModule prefix 'S' (filename with unicode escaping for '.')
    // "test.ml_5_15": 'test' (4 chars) + '.' (encoded as 2e) + 'ml_5_15' (7 chars)
    // Coded: "E2e" (E=4), Raw: "testml_5_15" (11 chars), Total: 15
    Demangled = llvm::oxcamlDemangle("_CamlU4MainSu15E2e_testml_5_15_300");
    EXPECT_STREQ(Demangled, "Main.mod(test.ml:5:15)");
    std::free(Demangled);
}

TEST(OxCamlDemangle, Escaping) {
    // Test will be added when we have valid escaped identifier examples
    // Note: OCaml identifiers have restrictions on valid characters
    EXPECT_TRUE(true);
}

TEST(OxCamlDemangle, MixedEncoding) {
    char *Demangled = nullptr;

    // DROP: Duplicate case
    // // Mix of prefixed modules and nested modules
    // Demangled = llvm::oxcamlDemangle("_CamlU4MainM8PositiveF4make_2");
    // EXPECT_STREQ(Demangled, "Main.Positive.make");
    // std::free(Demangled);

    // Actual symbols from Stdlib with nested module names using __
    Demangled = llvm::oxcamlDemangle("_CamlU12Stdlib__ListF6map_15_113_code");
    EXPECT_STREQ(Demangled, "Stdlib__List.map");
    std::free(Demangled);
}

TEST(OxCamlDemangle, Invalid) {
    char *Demangled = nullptr;

    // Invalid prefix.
    Demangled = llvm::oxcamlDemangle("_ABCDEF");
    EXPECT_EQ(Demangled, nullptr);

    // Correct prefix but still invalid.
    Demangled = llvm::oxcamlDemangle("_Caml");
    EXPECT_EQ(Demangled, nullptr);
}
