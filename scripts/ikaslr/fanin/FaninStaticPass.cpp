// SPDX-License-Identifier: GPL-2.0
//
// Static fan-in census (thesis ch.5, E5-5). Measurement-only plugin.
//
//   clang -fpass-plugin=libFaninStatic.so ...   with FANIN_OUT=<dir>
//
// For every translation unit, after the optimisation pipeline, writes one TSV
// file into $FANIN_OUT containing
//
//   CS  <tu> <caller> <file:line> <fn-type> <slot|->   one indirect call site
//   AT  <sym> <name> <fn-type> <use>                   one address-taking use
//
// A "slot" names where a function pointer lives as  <struct-name>@<byte offset>,
// the offset taken relative to the innermost *named* struct that contains it.
// Call sites get it from the GEP the callee was loaded through; functions get
// it from the aggregate initializer (via debug info, since clang emits literal
// struct types for designated initializers) or the GEP they were stored
// through. <use> is a slot, or ESC for any other use that lets the address
// flow somewhere untracked.
//
// References that only exist for kernel metadata (EXPORT_SYMBOL's
// __ADDRESSABLE, NOKPROBE_SYMBOL, llvm.used, ...) are not address-taking,
// except __ADDRESSABLE of an __init function, which is how PREL32 initcalls are
// emitted and is therefore a real indirect-call target (do_one_initcall).
//
// Types are printed with LLVM's struct-name uniquing suffixes removed so the
// same C struct compares equal across TUs. LLVM function types are coarser
// than C prototypes (signedness, enums and typedefs are erased).

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <regex>
#include <set>
#include <string>
#include <unistd.h>

using namespace llvm;

namespace {

std::string norm(std::string S) {
  static const std::regex Suffix(R"(((?:struct|union)\.[A-Za-z0-9_]+?)(?:\.\d+)+\b)");
  return std::regex_replace(S, Suffix, "$1");
}

std::string typeStr(Type *T) {
  std::string S;
  raw_string_ostream OS(S);
  T->print(OS);
  return norm(OS.str());
}

std::string slot(StringRef StructName, uint64_t Off) {
  return norm(StructName.str()) + "@" + std::to_string(Off);
}

struct Census {
  Module &M;
  const DataLayout &DL;
  explicit Census(Module &M) : M(M), DL(M.getDataLayout()) {}

  // Walk a GEP's indices, remembering the innermost named struct and the byte
  // offset relative to it.
  std::string gepSlot(GEPOperator *G) {
    Type *T = G->getSourceElementType();
    std::string Name;
    uint64_t Rel = 0;
    auto It = G->idx_begin();
    if (It == G->idx_end())
      return "";
    // The first index steps over the pointer itself. A non-zero constant there
    // is pointer arithmetic past the struct (container_of and friends), so the
    // slot is not the one the source type suggests.
    if (auto *First = dyn_cast<ConstantInt>(*It); First && !First->isZero())
      return "";
    ++It;
    for (; It != G->idx_end(); ++It) {
      auto *CI = dyn_cast<ConstantInt>(*It);
      if (auto *ST = dyn_cast<StructType>(T)) {
        if (!CI)
          return "";
        if (ST->hasName()) {
          Name = ST->getName().str();
          Rel = 0;
        }
        Rel += DL.getStructLayout(ST)->getElementOffset(CI->getZExtValue());
        T = ST->getElementType(CI->getZExtValue());
      } else if (auto *AT = dyn_cast<ArrayType>(T)) {
        if (CI)
          Rel += CI->getZExtValue() * DL.getTypeAllocSize(AT->getElementType());
        T = AT->getElementType();
      } else {
        return "";
      }
    }
    return Name.empty() ? "" : slot(Name, Rel);
  }

  // With typed pointers a GEP to offset 0 is folded into a bitcast of the
  // struct pointer, so a cast-stripped struct pointer means offset 0 of the
  // innermost leading named struct.
  std::string ptrSlot(Value *P) {
    Value *S = P->stripPointerCasts();
    if (auto *G = dyn_cast<GEPOperator>(S))
      return gepSlot(G);
    if (S == P)
      return "";
    auto *PT = dyn_cast<PointerType>(S->getType());
    Type *T = PT ? PT->getPointerElementType() : nullptr;
    std::string Name;
    while (auto *ST = dyn_cast_or_null<StructType>(T)) {
      if (ST->hasName())
        Name = ST->getName().str();
      T = ST->getNumElements() ? ST->getElementType(0) : nullptr;
    }
    return Name.empty() ? "" : slot(Name, 0);
  }

  static const DIType *strip(const DIType *T, std::string *Typedef = nullptr) {
    while (auto *D = dyn_cast_or_null<DIDerivedType>(T)) {
      auto Tag = D->getTag();
      if (Tag == dwarf::DW_TAG_typedef && Typedef)
        *Typedef = D->getName().str();
      if (Tag != dwarf::DW_TAG_typedef && Tag != dwarf::DW_TAG_const_type &&
          Tag != dwarf::DW_TAG_volatile_type && Tag != dwarf::DW_TAG_member)
        break;
      T = D->getBaseType();
    }
    return T;
  }

  // Offset Off (bytes) inside an object of debug type T: which named struct
  // field holds the pointer there?
  std::string diSlot(const DIType *T, uint64_t Off) {
    std::string Name;
    for (int Depth = 0; Depth < 16; Depth++) {
      std::string Td;
      T = strip(T, &Td);
      auto *C = dyn_cast_or_null<DICompositeType>(T);
      if (!C)
        return "";
      if (C->getTag() == dwarf::DW_TAG_array_type) {
        const DIType *E = C->getBaseType();
        uint64_t Sz = strip(E) ? strip(E)->getSizeInBits() / 8 : 0;
        if (!Sz)
          return "";
        Off %= Sz;
        T = E;
        continue;
      }
      bool IsUnion = C->getTag() == dwarf::DW_TAG_union_type;
      if (C->getTag() != dwarf::DW_TAG_structure_type && !IsUnion)
        return "";
      std::string CN = C->getName().str();
      if (CN.empty())
        CN = Td.empty() ? "anon" : Td;
      Name = (IsUnion ? "union." : "struct.") + CN;
      const DIType *Next = nullptr;
      uint64_t NextOff = 0;
      for (auto *El : C->getElements()) {
        auto *Mem = dyn_cast<DIDerivedType>(El);
        if (!Mem || Mem->getTag() != dwarf::DW_TAG_member)
          continue;
        uint64_t MO = Mem->getOffsetInBits() / 8;
        const DIType *MT = strip(Mem->getBaseType());
        uint64_t MS = MT ? MT->getSizeInBits() / 8 : 0;
        if (Off < MO || Off >= MO + std::max<uint64_t>(MS, 1))
          continue;
        if (MT && MT->getTag() == dwarf::DW_TAG_pointer_type)
          return slot(Name, Off);
        if (!Next) {
          Next = Mem->getBaseType();
          NextOff = Off - MO;
        }
      }
      if (!Next)
        return "";
      T = Next;
      Off = NextOff;
    }
    return "";
  }

  // Same descent on LLVM types, for globals without debug info.
  std::string llvmSlot(Type *T, uint64_t Off) {
    std::string Name;
    uint64_t Rel = Off;
    while (true) {
      if (auto *ST = dyn_cast<StructType>(T)) {
        const StructLayout *SL = DL.getStructLayout(ST);
        if (Off >= SL->getSizeInBytes())
          return "";
        if (ST->hasName()) {
          Name = ST->getName().str();
          Rel = Off;
        }
        unsigned I = SL->getElementContainingOffset(Off);
        Off -= SL->getElementOffset(I);
        T = ST->getElementType(I);
      } else if (auto *AT = dyn_cast<ArrayType>(T)) {
        uint64_t Sz = DL.getTypeAllocSize(AT->getElementType());
        Off %= Sz;
        T = AT->getElementType();
      } else {
        return (T->isPointerTy() && !Name.empty()) ? slot(Name, Rel - Off + 0) : "";
      }
    }
  }

  static bool metadataOnly(GlobalVariable *G) {
    StringRef N = G->getName(), S = G->getSection();
    return N == "llvm.used" || N == "llvm.compiler.used" ||
           S.startswith(".discard") || S == "_kprobe_blacklist" ||
           S == "_error_injection_whitelist" || S.startswith("__ksymtab") ||
           S.startswith(".export_symbol") || S.startswith("___kcrctab");
  }

  void classify(Function &F, std::set<std::string> &Uses) {
    struct Item {
      User *U;
      Value *V;
      uint64_t Off;
      bool InAgg;
    };
    SmallVector<Item, 16> Work;
    for (User *U : F.users())
      Work.push_back({U, &F, 0, false});
    while (!Work.empty()) {
      Item It = Work.pop_back_val();
      User *U = It.U;
      Value *V = It.V;
      if (auto *CE = dyn_cast<ConstantExpr>(U)) {
        if (CE->isCast()) {
          for (User *UU : CE->users())
            Work.push_back({UU, CE, It.Off, It.InAgg});
        } else {
          Uses.insert("ESC");
        }
        continue;
      }
      if (auto *CS = dyn_cast<ConstantStruct>(U)) {
        const StructLayout *SL = DL.getStructLayout(CS->getType());
        for (unsigned I = 0; I < CS->getNumOperands(); I++)
          if (CS->getOperand(I) == V)
            for (User *UU : CS->users())
              Work.push_back({UU, CS, It.Off + SL->getElementOffset(I), true});
        continue;
      }
      if (auto *CA = dyn_cast<ConstantArray>(U)) {
        uint64_t Sz = DL.getTypeAllocSize(CA->getType()->getElementType());
        for (unsigned I = 0; I < CA->getNumOperands(); I++)
          if (CA->getOperand(I) == V)
            for (User *UU : CA->users())
              Work.push_back({UU, CA, It.Off + I * Sz, true});
        continue;
      }
      if (auto *G = dyn_cast<GlobalVariable>(U)) {
        if (metadataOnly(G)) {
          if (G->getSection() == ".discard.addressable" &&
              F.getSection().startswith(".init.text"))
            Uses.insert("ESC"); // PREL32 initcall
          continue;
        }
        std::string S;
        if (It.InAgg) {
          SmallVector<DIGlobalVariableExpression *, 1> GVs;
          G->getDebugInfo(GVs);
          if (!GVs.empty())
            S = diSlot(GVs[0]->getVariable()->getType(), It.Off);
          if (S.empty())
            S = llvmSlot(G->getValueType(), It.Off);
        }
        Uses.insert(S.empty() ? "ESC" : S);
        continue;
      }
      if (auto *CB = dyn_cast<CallBase>(U)) {
        if (is_contained(CB->args(), V) || CB->getCalledOperand() != V)
          Uses.insert("ESC");
        continue;
      }
      if (isa<ICmpInst>(U) || isa<GlobalAlias>(U))
        continue;
      if (auto *SI = dyn_cast<StoreInst>(U)) {
        if (SI->getValueOperand() == V) {
          std::string S = ptrSlot(SI->getPointerOperand());
          Uses.insert(S.empty() ? "ESC" : S);
        }
        continue;
      }
      Uses.insert("ESC");
    }
  }

  void run(raw_ostream &OS) {
    std::string TU = M.getSourceFileName();
    for (Function &F : M) {
      for (Instruction &I : instructions(F)) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB || CB->isInlineAsm() || CB->getCalledFunction())
          continue;
        Value *Callee = CB->getCalledOperand()->stripPointerCasts();
        if (isa<Function>(Callee) || isa<GlobalAlias>(Callee))
          continue;
        std::string S;
        if (auto *L = dyn_cast<LoadInst>(Callee))
          S = ptrSlot(L->getPointerOperand());
        std::string Loc = "-";
        if (const DebugLoc &D = CB->getDebugLoc())
          Loc = D->getFilename().str() + ":" + std::to_string(D.getLine());
        OS << "CS\t" << TU << "\t" << F.getName() << "\t" << Loc << "\t"
           << typeStr(CB->getFunctionType()) << "\t" << (S.empty() ? "-" : S)
           << "\n";
      }
    }
    for (Function &F : M) {
      if (F.isIntrinsic())
        continue;
      std::set<std::string> Uses;
      classify(F, Uses);
      std::string Sym = F.getName().str();
      if (F.hasLocalLinkage())
        Sym += "@" + TU;
      for (auto &U : Uses)
        OS << "AT\t" << Sym << "\t" << F.getName() << "\t"
           << typeStr(F.getFunctionType()) << "\t" << U << "\n";
    }
  }
};

struct FaninStaticPass : PassInfoMixin<FaninStaticPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    const char *Dir = getenv("FANIN_OUT");
    if (!Dir)
      return PreservedAnalyses::all();
    std::string Buf;
    raw_string_ostream OS(Buf);
    Census(M).run(OS);
    std::string Name = std::string(Dir) + "/" +
                       std::to_string(std::hash<std::string>{}(M.getSourceFileName())) +
                       "." + std::to_string(getpid()) + ".tsv";
    std::error_code EC;
    raw_fd_ostream Out(Name, EC);
    if (!EC)
      Out << OS.str();
    return PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "FaninStatic", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerOptimizerLastEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(FaninStaticPass());
                });
          }};
}
