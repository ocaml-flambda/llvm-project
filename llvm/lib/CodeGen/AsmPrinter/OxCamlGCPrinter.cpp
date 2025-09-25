//===- OxCamlGCPrinter.cpp - OxCaml frametable emitter --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements printing the assembly code for an OxCaml frametable.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/GCMetadata.h"
#include "llvm/CodeGen/GCMetadataPrinter.h"
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/IR/BuiltinGCs.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace llvm;

namespace {

class OxCamlGCMetadataPrinter : public GCMetadataPrinter {
public:
  void beginAssembly(Module &M, GCModuleInfo &Info, AsmPrinter &AP) override;
  void finishAssembly(Module &M, GCModuleInfo &Info, AsmPrinter &AP) override;
  bool emitStackMaps(Module &M, StackMaps &SM, AsmPrinter &AP) override;
};

} // end anonymous namespace

static GCMetadataPrinterRegistry::Add<OxCamlGCMetadataPrinter>
    Y("oxcaml", "OxCaml frametable printer");

void llvm::linkOxCamlGCPrinter() {}

static std::string camlGlobalSymName(const Module &M, const char *Id) {
  if (Metadata *ModuleMD = M.getModuleFlag(StringRef("oxcaml_module"))) {
    if (MDString *Str = dyn_cast<MDString>(ModuleMD)) {
      StringRef ModuleName = Str->getString();
      
      std::string SymName;
      SymName += "caml";
      SymName += ModuleName;
      SymName += "__";
      SymName += Id;
    
      return SymName;
    }
  }

  report_fatal_error("[OxCamlGCPrinter] module name not provided");
}

static void emitCamlGlobal(const Module &M, MCStreamer &OS, const char *Id) {
  std::string SymName = camlGlobalSymName(M, Id);

  SmallString<128> TmpStr;
  Mangler::getNameWithPrefix(TmpStr, SymName, M.getDataLayout());

  MCSymbol *Sym = OS.getContext().getOrCreateSymbol(TmpStr);

  OS.emitSymbolAttribute(Sym, MCSA_Global);
  OS.emitLabel(Sym);
}

void OxCamlGCMetadataPrinter::beginAssembly(Module &M, GCModuleInfo &Info,
                                           AsmPrinter &AP) {
  AP.OutStreamer->switchSection(AP.getObjFileLowering().getTextSection());
  emitCamlGlobal(M, *(AP.OutStreamer), "code_begin");

  AP.OutStreamer->switchSection(AP.getObjFileLowering().getDataSection());
  emitCamlGlobal(M, *(AP.OutStreamer), "data_begin");
}


void OxCamlGCMetadataPrinter::finishAssembly(Module &M, GCModuleInfo &Info,
                                         AsmPrinter &AP) {
  AP.OutStreamer->switchSection(AP.getObjFileLowering().getTextSection());
  emitCamlGlobal(M, *(AP.OutStreamer), "code_end");

  AP.OutStreamer->switchSection(AP.getObjFileLowering().getDataSection());
  emitCamlGlobal(M, *(AP.OutStreamer), "data_end");
}

/// Map LLVM DWARF register numbers to OxCaml register map.
/// * See llvm/lib/Target/X86/X86RegisterInfo.td for DWARF register numbers.
/// * See backend/amd64/proc.ml for the OxCaml register map.

// TODO: This is target-specific and should probably live in a
// target-specific location.

// Directly taken from [Reg_class.gpr_dwarf_reg_numbers]:
// https://github.com/oxcaml/oxcaml/blob/main/backend/amd64/reg_class.ml#L26
// Note that R14 and R15 are added for completeness
static constexpr std::array<unsigned, 16> GPR_OxCamlToDwarf =
  { 0, 3, 5, 4, 1, 2, 8, 9, 12, 13, 10, 11, 6, 14, 15 };

static constexpr auto GPR_DwarfToOxCaml = []() {
  std::array<unsigned, 16> result{};
  for (size_t ocaml_idx = 0; ocaml_idx < GPR_OxCamlToDwarf.size(); ++ocaml_idx) {
    unsigned dwarf_reg = GPR_OxCamlToDwarf[ocaml_idx];
    if (dwarf_reg < result.size()) {
      result[dwarf_reg] = ocaml_idx;
    }
  }
  return result;
}();

static const unsigned XMMBeginOxCaml = 100;
static const unsigned XMMBeginDwarf = 17;
static const unsigned XMMEndDwarf = 32;

static unsigned mapLLVMDwarfRegToOxCamlIndex(unsigned DwarfRegNum) {
  if (DwarfRegNum < GPR_DwarfToOxCaml.size()) {
    return GPR_DwarfToOxCaml[DwarfRegNum];
  } else if (XMMBeginDwarf <= DwarfRegNum && DwarfRegNum <= XMMEndDwarf) {
    return DwarfRegNum - XMMBeginDwarf + XMMBeginOxCaml;
  } else {
    report_fatal_error("[OxCamlGCPrinter] unrecognised DWARF register: "
      + Twine(DwarfRegNum));
  }
}

// note that although `StackMaps` keeps `ID` as a 64-bit integer, anything
// above 32 bits gets truncated, so we can't use them.

static uint64_t stackOffsetOfID(uint64_t ID) {
  return ID & ((1ull << 16) - 1) & ~(1ull);
}

static uint64_t allocSizeOfID(uint64_t ID) {
  return ID >> 16;
}

static bool IDHasAlloc(uint64_t ID) {
  return ID & 1ull;
}

// Every 8-bit entry emitted in the frametable is offset by 2 (since that is the
// min allocation size). So, every slot can represent allocations of size [2, 257]
static uint8_t encodeAllocSize(uint64_t AllocSize) {
  return AllocSize - 2;
}

static const int AllocMask = 2;
static const int FrameSizeReservedMask = 3; // Debug + Alloc

bool OxCamlGCMetadataPrinter::emitStackMaps(Module &M, StackMaps &SM, AsmPrinter &AP) {
  MCStreamer &OS = *AP.OutStreamer;
  unsigned PtrSize = M.getDataLayout().getPointerSize(); // Can only be 8 for now
  
  OS.switchSection(AP.getObjFileLowering().getDataSection());
  
  emitCamlGlobal(M, OS, "frametable");

  // Number of records
  OS.emitInt64(SM.getCSInfos().size());

  for (const auto &CSI : SM.getCSInfos()) {
    // From runtime/frame_descriptors.h:
    // https://github.com/oxcaml/oxcaml/blob/main/runtime/caml/frame_descriptors.h#L63
    //
    // typedef struct {
    //   int32_t retaddr_rel; /* offset of return address from &retaddr_rel */
    //   uint16_t frame_data; /* frame size and various flags */
    //   uint16_t num_live;
    //   uint16_t live_ofs[num_live];
    // } frame_descr;

    // retaddr_rel
    MCSymbol *Here = OS.getContext().createTempSymbol();
    OS.emitLabel(Here);
    const MCExpr *RelativeAddr = MCBinaryExpr::createSub(
        MCSymbolRefExpr::create(CSI.CSLabel, OS.getContext()),
        MCSymbolRefExpr::create(Here, OS.getContext()),
        OS.getContext());
    OS.emitValue(RelativeAddr, 4);

    // frame_data
    uint64_t FrameSize = CSI.CSFunctionInfo.StaticStackSize;
    FrameSize += PtrSize; // Return address

    // The LLVM IR emitted from OxCaml will always set the statepoint ID for
    // calls to be wrapped in a statepoint. Also, dote that DefaultStatepointID
    // (= 0xABCDEF00 as of now) does not clash with the encoding we use since
    // anything that sets the upper 16 bits will also set the bottom bit.
    if (CSI.ID != StatepointDirectives::DefaultStatepointID) {
      // Stack offset from OxCaml
      FrameSize += stackOffsetOfID(CSI.ID);

      if (FrameSize & FrameSizeReservedMask) {
        report_fatal_error("[OxCamlGCPrinter] frame size has bottom bits set: "
          + Twine(FrameSize));
      }
      
      // Alloc bit
      if (IDHasAlloc(CSI.ID)) {
        FrameSize |= AllocMask;
      }
    }

    if (FrameSize >= 1 << 16)
      report_fatal_error("[OxCamlGCPrinter] frame size requires long frames:"
        + Twine(FrameSize));
    OS.emitInt16(FrameSize);

    // num_live
    uint64_t LiveCount = 0;
    for (const auto &Loc : CSI.Locations) {
      if (Loc.Type == StackMaps::Location::Register ||
          Loc.Type == StackMaps::Location::Direct ||
          Loc.Type == StackMaps::Location::Indirect) {
        LiveCount++;
      }
    }
    LiveCount += CSI.LiveOuts.size();

    if (LiveCount >= 1 << 16) {
      // Very rude!
      report_fatal_error("[OxCamlGCPrinter] live count requires long frames:"
        + Twine(LiveCount));
    }
    OS.emitInt16(LiveCount);

    // live_ofs
    for (const auto &Loc : CSI.Locations) {
      if (Loc.Type == StackMaps::Location::Register) {
        // Register indices are tagged (2n+1) and follow the OxCaml register
        // map (see `mapLLVMDwarfRegToOxCamlIndex`)
        unsigned DwarfRegNum = Loc.Reg;
        unsigned OxCamlIndex = mapLLVMDwarfRegToOxCamlIndex(DwarfRegNum);
        uint16_t EncodedReg = (OxCamlIndex << 1) + 1;
        OS.emitInt16(EncodedReg);
      } else if (Loc.Type == StackMaps::Location::Direct ||
                 Loc.Type == StackMaps::Location::Indirect) {
        // For stack locations (Direct/Indirect): emit offset directly
        int64_t Offset = Loc.Offset;

        // BP-relative addressing -> SP
        if (Offset < 0) {
          int64_t TempFrameSize =
            FrameSize - PtrSize /* return address */ - PtrSize /* pushed BP */;
          Offset += TempFrameSize;
        }
        
        if (Offset < -(1 << 15) || Offset >= (1 << 15)) {
          // Very rude!
          report_fatal_error("[OxCamlGCPrinter] stack offset too large: "
            + Twine(Offset));
        }
        OS.emitInt16(static_cast<uint16_t>(Offset));
      } else {
        // TODO: Do we need anything else here?
      }
    }

    for (const auto &LO : CSI.LiveOuts) {
      unsigned OxCamlIndex = mapLLVMDwarfRegToOxCamlIndex(LO.DwarfRegNum);
      uint16_t EncodedReg = (OxCamlIndex << 1) + 1;
      OS.emitInt16(EncodedReg);
    }

    if (IDHasAlloc(CSI.ID)) {
      int AllocSize = allocSizeOfID(CSI.ID);

      if (AllocSize < 2) {
        report_fatal_error("[OxCamlGCPrinter] alloc size must at least be two!");
      }

      // Allocations can theoretically go up to 255 * 257 = 65535 words,
      // but in practice comballoc never gives us allocations that exceed 255,
      // so this handling isn't necessarily needed, but it's here just in case.

      int MaxAllocSize = 257;

      if (AllocSize % MaxAllocSize == 0) {
        size_t NumAlloc = AllocSize / MaxAllocSize;
        
        OS.emitInt8(NumAlloc);
        for (size_t i = 0; i < NumAlloc; ++i) {
          OS.emitInt8(encodeAllocSize(MaxAllocSize));
        }
      } else if (AllocSize % MaxAllocSize == 1) {
        // This is special since we cannot have allocations of size 1...
        
        // Guaranteed to be nonnegative
        size_t NumMaxAlloc = AllocSize / MaxAllocSize - 1;
        
        OS.emitInt8(NumMaxAlloc + 2);
        for (size_t i = 0; i < NumMaxAlloc; ++i) {
          OS.emitInt8(encodeAllocSize(MaxAllocSize));
        }
        
        OS.emitInt8(encodeAllocSize(MaxAllocSize - 1));
        OS.emitInt8(encodeAllocSize(2));
      } else {
        size_t NumMaxAlloc = AllocSize / MaxAllocSize;
        
        OS.emitInt8(NumMaxAlloc + 1);
        for (size_t i = 0; i < NumMaxAlloc; ++i) {
          OS.emitInt8(encodeAllocSize(MaxAllocSize));
        }
        
        OS.emitInt8(encodeAllocSize(AllocSize % MaxAllocSize));
      }
    }

    OS.emitValueToAlignment(Align(PtrSize));
  }

  OS.addBlankLine();
  return true;
}
