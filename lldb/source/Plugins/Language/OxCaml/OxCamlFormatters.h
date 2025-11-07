//===-- OxCamlFormatters.h ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_OxCamlFormatters_h_
#define liblldb_OxCamlFormatters_h_

#include "lldb/DataFormatters/TypeSummary.h"
#include "lldb/Utility/Stream.h"
#include "lldb/lldb-forward.h"

namespace lldb_private {
namespace formatters {
namespace oxcaml {

bool OxCamlValue_SummaryProvider(ValueObject &valobj, Stream &stream,
                                 const TypeSummaryOptions &options);

} // namespace oxcaml
} // namespace formatters
} // namespace lldb_private

#endif // liblldb_OxCamlFormatters_h_
