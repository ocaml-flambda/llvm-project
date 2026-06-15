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


namespace {
struct DemangleCase {
    const char *Mangled;
    const char *Expected; // nullptr means the input must be rejected
};

void CheckDemangle(const DemangleCase &C) {
    char *Demangled = llvm::oxcamlDemangle(C.Mangled);
    if (C.Expected == nullptr)
        EXPECT_EQ(Demangled, nullptr) << "expected rejection of: " << C.Mangled;
    else
        EXPECT_STREQ(Demangled, C.Expected) << "input: " << C.Mangled;
    std::free(Demangled);
}
} // namespace

// Corpus from oxcaml/oxcaml PR #5100, the pure-OCaml `ocamlfilt` reference
// demangler (testsuite/tests/tool-ocamlfilt/structured.{sh,reference}), with the
// non-deterministic trailing compiler stamp dropped (oxcamlDemangle strips it).
TEST(OxCamlDemangle, StructuredCorpus) {
    static const DemangleCase Cases[] = {
        {"_CamlU3FooM3BarF3baz", "Foo.Bar.baz"},
        {"_CamlU6StdlibF3map", "Stdlib.map"},
        {"_CamlU3FooO5MyObj", "Foo.MyObj"},                 // class (O)
        {"_CamlU3FooIU3BarF3qux",                           // inline marker (I)
         "Foo.<specialization_of>.Bar.qux"},
        {"_CamlU3FooFu8A3e3e3d_", "Foo.>>="},               // escaped: bytes only
        {"_CamlU3FooFu7D2a_let", "Foo.let*"},               // escaped: one chunk
        {"_CamlU3FooFu14E27D27_funcsub", "Foo.func'sub'"},  // escaped: two chunks
        {"_CamlU6StdlibLu18G2e_stdlibml_334_0",
         "Stdlib.fn(stdlib.ml:334:0)"},
        {"_CamlU3FooS5_42_7", "Foo.mod(:42:7)"},            // empty filename
        {"_CamlU3FooP5_10_5", "Foo.partial(:10:5)"},        // partial, empty file
        {"_CamlU3FooM3BarM3BazF6my_fun", "Foo.Bar.Baz.my_fun"},
        {"_CamlU3FooFu5_0foo", "Foo.0foo"},                 // digit-leading name
        {"_CamlU4MainF11say_hello_0_5_code",                // stamp stripped
         "Main.say_hello"},
        {"_CamlU4MainM4TestF5foo_1_6_code", "Main.Test.foo"},
        {"_CamlU12Stdlib__ListF6map_15_113_code",           // __ pack separator
         "Stdlib__List.map"},
        {"_CamlU4MainSu15E2e_testml_5_15_300",              // span kept, stamp dropped
         "Main.mod(test.ml:5:15)"},
        {"_CamlU4MainF4mainLu15E2e_mainml_7_32F4fn_4_9_code",
         "Main.main.fn(main.ml:7:32).fn"},
        {"_CamlU3FooF3barLu14D2e_fooml_3_15Lu14D2e_fooml_4_22F4fn_7", // nested
         "Foo.bar.fn(foo.ml:3:15).fn(foo.ml:4:22).fn"},
        {"_CamlU3FooM3BarLu14D2e_fooml_5_10F3bazF4fn_1_2",
         "Foo.Bar.fn(foo.ml:5:10).baz.fn"},
        {"_CamlU8Functor2F8combinedLu14D2e_fooml_8_12F4fn_1_3_code",
         "Functor2.combined.fn(foo.ml:8:12).fn"},
        {"_CamlU3FooSu14D2e_fooml_2_10F4initF4fn_5_6",
         "Foo.mod(foo.ml:2:10).init.fn"},
        {"_CamlU3FooF3barPu14D2e_fooml_9_15", "Foo.bar.partial(foo.ml:9:15)"},
        {"_CamlU3FooM3BarIU3BazF3qux", "Foo.Bar.<specialization_of>.Baz.qux"},
        {"_CamlU3FooM3BarO5ShapeF4area", "Foo.Bar.Shape.area"},
    };
    for (const auto &C : Cases)
        CheckDemangle(C);
}

// Golden corpus for the legacy flat schemes from oxcaml/oxcaml PR #5100
// (testsuite/tests/tool-ocamlfilt/flat0.{sh,reference} and flat1.{sh,reference}).
// flat0 = OCaml <= 5.2 ("__" separators, "$xx" escapes); flat1 = OCaml >= 5.3,
// which adds the macOS flavour ("$" separators, "$$xx"/"$$$xx" escapes,
// auto-detected). The non-deterministic trailing compiler stamp is stripped.
TEST(OxCamlDemangle, FlatCorpus) {
    static const DemangleCase Cases[] = {
        // flat0 (and Linux flat1, which is identical)
        {"camlFoo", "Foo"},
        {"camlFoo__bar_0", "Foo.bar"},
        {"camlA__B__C__D__func_0", "A.B.C.D.func"},
        {"camlFoo__bar_baz_42", "Foo.bar_baz"},            // name '_' kept
        {"camlStdlib__array__map_154", "Stdlib.array.map"},
        {"camlBaz__Foo__Bar__init_0", "Baz.Foo.Bar.init"},
        {"camlFoo__", "Foo."},
        {"camlStdlib__bytes__$2b$2b_2205", "Stdlib.bytes.++"},
        {"camlFoo__$2b$2b$2b_1", "Foo.+++"},
        {"camlStdlib__anon_fn$5bstdlib$2eml$3a334$2c0$2d$2d54$5d_1453",
         "Stdlib.anon_fn[stdlib.ml:334,0--54]"},           // span kept
        {"_camlFoo__bar_0", "Foo.bar"},                    // macOS "_caml" prefix
        {"_camlStdlib__array__map_154", "Stdlib.array.map"},
        // flat1 Linux escapes
        {"camlFoo__$3e$3e$3d_12", "Foo.>>="},
        {"camlIndexing__.$25$28$29_2_14_code", "Indexing..%()"},
        // flat1 macOS flavour: "$" separator, "$$xx"/"$$$xx" escapes
        {"camlA$B$C$D$func_0", "A.B.C.D.func"},
        {"camlFoo$$$3e$$3e$$3d_12", "Foo.>>="},
        {"camlFoo$$$2b$$2b_5", "Foo.++"},
        {"camlIndexing$$$2e$$25$$28$$29_2_14_code", "Indexing..%()"},
        {"camlStdlib$anon_fn$$5bstdlib$$2eml$$3a334$$2c0$$2d$$2d54$$5d_1453",
         "Stdlib.anon_fn[stdlib.ml:334,0--54]"},
        {"camlList$add_42", "List.add"},
        {"camlBuffer$add_string_5_28_code", "Buffer.add_string"},
        {"_camlStdlib__List__map_15_113_code", "Stdlib.List.map"},
    };
    for (const auto &C : Cases)
        CheckDemangle(C);
}

// Inputs that must be rejected, from PR #5100 reject.{sh,reference} plus the
// empty-path case. Flat prefixes are only accepted when followed by an
// uppercase module letter, so the runtime symbol "caml_foo" and the
// double-underscore "__camlFoo" are rejected.
TEST(OxCamlDemangle, RejectCorpus) {
    static const DemangleCase Cases[] = {
        {"_Caml", nullptr},                       // valid prefix, empty path
        {"_ABCDEF", nullptr},                     // wrong prefix
        {"main", nullptr},
        {"_ZN4testE", nullptr},                   // Itanium C++ mangling
        {"caml_foo", nullptr},
        {"_CamlU3FooXXX", nullptr},               // unknown path-item tag
        {"_CamlU99Foo", nullptr},                 // length overruns input
        {"_CamlUFoo", nullptr},                   // missing length prefix
        {"_CamlU3FooFu6G1234_funcsub", nullptr},  // base26 position > raw length
        {"_CamlU3FooFu11G2g_funcsub", nullptr},   // 'g' is not a hex digit
        {"_CamlU3FooFu11G2A_funcsub", nullptr},   // 'A' where a hex digit is due
        {"__camlFoo__bar_0", nullptr},            // flat (lowercase) scheme
    };
    for (const auto &C : Cases)
        CheckDemangle(C);
}
