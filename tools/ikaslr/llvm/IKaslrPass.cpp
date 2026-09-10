//===- IKaslrPass.cpp - I-KASLR trampoline generation ---------------------===//
//
// I-KASLR 编译期支持（论文 §3.5.1）。LLVM 新 Pass Manager 插件。
//
// 对每个被选中的函数 F 做三件事：
//
//   1. 把 F 改名为 F_body 并放进 .rand.text.F —— 函数体，可迁移；
//   2. 新建一个同名同签名的 F 放进 .tramp.text.F —— fixed_in 跳板，地址固定；
//   3. 发出 F 的 target 槽与跳板表项。
//
// 关键在于**改名**：把原函数改名、再用原名新建跳板，所有既有调用点自然就落到
// 跳板上，无需遍历和改写调用者。这正是论文 §3.4.2 所要的"调用者不必修改"。
//
// 跳板体等价于：
//     ikaslr_enter();
//     r = (*__ikaslr_target_F)(args...);   // 经 target 间接转移
//     ikaslr_leave();
//     return r;
//
// 选择哪些函数：由 -ikaslr-funcs-file=<路径> 指定的名单文件（每行一个函数名），
// 或环境变量 IKASLR_FUNCS 指向同样格式的文件。内核构建用名单文件比源码注解方便，
// 因为不需要改动被随机化子系统的源码（论文的设计目标之二）。
//
// 另有 -ikaslr-verify：只报告跨区域调用而不做改写，对应 §6.4 的编译期检查。
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/Constants.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <fstream>
#include <set>
#include <string>

using namespace llvm;

static cl::opt<std::string> FuncsFile(
    "ikaslr-funcs-file", cl::init(""),
    cl::desc("File listing function names to randomize, one per line"));

static cl::opt<bool> VerifyOnly(
    "ikaslr-verify", cl::init(false),
    cl::desc("Only report cross-region calls; do not transform"));

static cl::opt<bool> Verbose(
    "ikaslr-verbose", cl::init(false),
    cl::desc("Print each transformed function"));

namespace {

/// 读入随机化函数名单。文件路径来自选项或 IKASLR_FUNCS 环境变量。
/// '#' 起始的行与空行忽略。
static std::set<std::string> loadFuncList() {
  std::set<std::string> Names;
  std::string Path = FuncsFile;
  if (Path.empty())
    if (const char *Env = getenv("IKASLR_FUNCS"))
      Path = Env;
  if (Path.empty())
    return Names;

  std::ifstream In(Path);
  if (!In) {
    errs() << "ikaslr: cannot open function list '" << Path << "'\n";
    return Names;
  }
  std::string Line;
  while (std::getline(In, Line)) {
    // 去空白
    size_t B = Line.find_first_not_of(" \t\r\n");
    if (B == std::string::npos)
      continue;
    size_t E = Line.find_last_not_of(" \t\r\n");
    Line = Line.substr(B, E - B + 1);
    if (Line.empty() || Line[0] == '#')
      continue;
    Names.insert(Line);
  }
  return Names;
}

/// 早期标记 pass：给选中的函数打上 noinline。
///
/// **必须在内联器之前跑。** 否则调用点会被内联，内联出来的副本不经过跳板，
/// 既绕过了地址间接（随机化对它无效），也绕过了 enter/leave 计数（活跃集合
/// 不再准确）——不变式"所有跨区域转移必经跳板"随之失效。
/// 实测：不加这一步时，-O2 会把被随机化函数整个内联进调用者并常量折叠。
struct IKaslrMarkPass : PassInfoMixin<IKaslrMarkPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    std::set<std::string> Sel = loadFuncList();
    if (Sel.empty())
      return PreservedAnalyses::all();
    bool Changed = false;
    for (Function &F : M) {
      if (F.isDeclaration() || !Sel.count(F.getName().str()))
        continue;
      F.addFnAttr(Attribute::NoInline);
      F.removeFnAttr(Attribute::AlwaysInline);
      Changed = true;
    }
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

struct IKaslrPass : PassInfoMixin<IKaslrPass> {
  bool IsAArch64 = false;   /* 由 run() 按模块 triple 设定 */


  /// 发出该函数的 target 槽、表项结构与表内指针。
  /// 表里存的是**指针**而非结构体本身：x86-64 会把 >=32 字节的对象按 32 字节
  /// 对齐，而表项结构 40 字节，直接排布会在表项间留下空洞，按 sizeof 索引就会
  /// 落进填充区（内核侧踩过这个坑，见 03-randomization.md）。
  void emitTableEntry(Module &M, Function *Tramp, Function *Body,
                      GlobalVariable *Target, StringRef Name) {
    LLVMContext &Ctx = M.getContext();
    Type *I8Ptr = Type::getInt8PtrTy(Ctx);
    Type *I8PtrPtr = I8Ptr->getPointerTo();
    Type *I64 = Type::getInt64Ty(Ctx);

    // struct ikaslr_tramp { void *tramp; void *body; void **target;
    //                       const char *name; size_t size; }
    StructType *EntTy = StructType::getTypeByName(Ctx, "struct.ikaslr_tramp");
    if (!EntTy)
      EntTy = StructType::create(Ctx, {I8Ptr, I8Ptr, I8PtrPtr, I8Ptr, I64},
                                 "struct.ikaslr_tramp");

    Constant *NameStr = ConstantDataArray::getString(Ctx, Name, true);
    auto *NameGV = new GlobalVariable(M, NameStr->getType(), true,
                                      GlobalValue::PrivateLinkage, NameStr,
                                      ".ikaslr.name." + Name);
    NameGV->setAlignment(Align(1));

    Constant *Init = ConstantStruct::get(
        cast<StructType>(EntTy),
        {ConstantExpr::getBitCast(Tramp, I8Ptr),
         ConstantExpr::getBitCast(Body, I8Ptr),
         ConstantExpr::getBitCast(Target, I8PtrPtr),
         ConstantExpr::getBitCast(NameGV, I8Ptr),
         ConstantInt::get(I64, 0)});   // size 由内核在初始化时填

    auto *Ent = new GlobalVariable(M, EntTy, false,
                                   GlobalValue::InternalLinkage, Init,
                                   "__ikaslr_ent_" + Name);
    Ent->setAlignment(Align(8));

    auto *Ptr = new GlobalVariable(M, EntTy->getPointerTo(), false,
                                   GlobalValue::InternalLinkage, Ent,
                                   "__ikaslr_ptr_" + Name);
    Ptr->setSection(".data..ikaslr_tramp_tbl");
    Ptr->setAlignment(Align(8));
    appendToUsed(M, {Ptr});   // 防止被优化掉
  }

  /// 改造一个函数：body 入 .rand.text，新建同名跳板入 .tramp.text。
  /// 把一个符号的地址物化成**64 位绝对立即数**，返回该地址的值。
  ///
  /// 这是函数体可搬移的关键（论文 §3.4.3）。内核以 -mcmodel=kernel 编译，
  /// 对外引用都是 PC 相对的（`call rel32`、`mov off(%rip)`），函数体一旦被搬到
  /// 别的地址，这些偏移就全错了——实测 `seq_puts_body` 里的 `call strlen`
  /// 搬移后落到了一个无关的 vmalloc 页上并触发取指缺页。
  ///
  /// x86-64 上的解法是 64 位绝对寻址：`movabsq $sym, %reg`（R_X86_64_64），
  /// 绝对地址不随代码位置改变。IR 层面没法直接表达——
  /// `inttoptr(ptrtoint @f)` 会被后端折叠回 `call rel32`，
  /// 而 LLVM 14 也不支持每函数的 code-model 属性（实测两者都无效）。
  /// 因此用内联汇编 + `"s"`（符号）约束强制物化。
  Value *materializeAbs(IRBuilder<> &B, Constant *Sym) {
    Type *I8Ptr = Type::getInt8PtrTy(Sym->getContext());
    FunctionType *AsmTy = FunctionType::get(I8Ptr, {Sym->getType()}, false);

    // 各架构把"符号地址 -> 64 位绝对立即数"的写法不同：
    //   x86-64 : 一条 movabs（R_X86_64_64）
    //   AArch64: movz/movk 四条（R_AARCH64_MOVW_UABS_G0_NC..G3）——
    //            没有 64 位立即数指令，必须分四段拼；`adrp` 是 PC 相对的，
    //            正是搬移后失效的那种寻址，不能用。
    const char *Asm;
    if (IsAArch64)
      // AArch64 **不能用绝对立即数**（movz/movk 那套）：内核镜像以 PIE 链接
      // （CONFIG_RELOCATABLE），链接器直接拒绝 R_AARCH64_MOVW_UABS_*
      // （实测报 "can not be used when making a shared object"）。
      //
      // 正确做法是**随函数一起搬移的字面量池**：`ldr $0, =sym` 让汇编器在
      // 本函数所在节（.rand.text.F）内放一条字面量，用 PC 相对的 ldr 取它。
      // 节内相对偏移随函数整体搬移保持不变，而字面量本身是 R_AARCH64_ABS64，
      // 由内核启动时的重定位填成最终地址——两边都成立。
      // 这正是论文 §3.4.3 所说"arm64 上共享偏移表/字面量池是刚需"。
      Asm = "ldr $0, =$1";
    else
      Asm = "movabsq $1, $0";

    InlineAsm *IA = InlineAsm::get(AsmTy, Asm, "=r,s",
                                   /*hasSideEffects=*/false);
    return B.CreateCall(IA, {Sym});
  }

  /// 常量里是否（递归地）引用了全局符号。
  static bool refsGlobal(Constant *C, unsigned Depth = 0) {
    if (Depth > 8)
      return false;
    if (isa<GlobalValue>(C))
      return true;
    if (auto *CE = dyn_cast<ConstantExpr>(C))
      for (unsigned i = 0; i < CE->getNumOperands(); ++i)
        if (auto *Op = dyn_cast<Constant>(CE->getOperand(i)))
          if (refsGlobal(Op, Depth + 1))
            return true;
    return false;
  }

  /// 把一个（可能是常量表达式的）引用重建为"绝对地址 + 指令"形式。
  ///
  /// 只处理裸的 GlobalVariable 操作数是不够的：实测 `single_open` 里的
  /// `kmalloc_caches+0x28` 是 GEP 常量表达式、`d_splice_alias` 里的
  /// `rename_lock` 被折进了内存操作数，两者都以 ConstantExpr 出现，
  /// 漏掉它们就仍是 `(%rip)` 相对寻址，搬移后读到错误地址（实测触发缺页）。
  Value *rebuildAbs(IRBuilder<> &B, Constant *C) {
    if (isa<GlobalValue>(C)) {
      Value *A = materializeAbs(B, C);
      return B.CreateBitCast(A, C->getType());
    }
    if (auto *CE = dyn_cast<ConstantExpr>(C)) {
      Instruction *I = CE->getAsInstruction();
      for (unsigned i = 0; i < I->getNumOperands(); ++i)
        if (auto *Op = dyn_cast<Constant>(I->getOperand(i)))
          if (refsGlobal(Op))
            I->setOperand(i, rebuildAbs(B, Op));
      B.Insert(I);
      return I;
    }
    return C;
  }

  /// 消除函数体内所有指向区域外的 PC 相对引用，使其可整体搬移。
  ///
  /// 两类引用（见 ../../Documentation/crr/实现/14-code-model-and-unmovable.md）：
  ///   · 对外部函数的直接调用 -> 绝对地址 + 间接调用
  ///   · 对全局变量的引用     -> 绝对地址
  /// 函数**内部**的相对跳转随函数整体搬移自动保持正确，不必处理。
  ///
  /// 统一按"操作数里含全局符号就重建"处理，直接调用因此自然变成间接调用。
  void makeBodyPositionIndependent(Function *Body) {
    // 栈保护会让后端生成一条到 __stack_chk_fail 的**直接**调用，那是 PC 相对的，
    // 搬移后失效；被随机化的函数体因此关掉栈保护（仅限这些函数）。
    Body->removeFnAttr(Attribute::StackProtect);
    Body->removeFnAttr(Attribute::StackProtectStrong);
    Body->removeFnAttr(Attribute::StackProtectReq);

    // llvm.memcpy/memset/memmove 会被后端降级成到 memcpy 等的直接调用，
    // 同样是 PC 相对。这里先把它们换成对内核同名符号的**绝对地址间接调用**。
    SmallVector<MemIntrinsic *, 8> Mem;
    for (BasicBlock &BB : *Body)
      for (Instruction &I : BB)
        if (auto *MI = dyn_cast<MemIntrinsic>(&I))
          Mem.push_back(MI);
    for (MemIntrinsic *MI : Mem) {
      Module *M = Body->getParent();
      LLVMContext &Ctx = M->getContext();
      Type *I8Ptr = Type::getInt8PtrTy(Ctx);
      Type *I64 = Type::getInt64Ty(Ctx);
      IRBuilder<> B(MI);
      const char *Nm = isa<MemCpyInst>(MI) ? "memcpy"
                     : isa<MemMoveInst>(MI) ? "memmove" : "memset";
      Type *Arg1 = isa<MemSetInst>(MI) ? Type::getInt32Ty(Ctx) : I8Ptr;
      FunctionType *FT = FunctionType::get(I8Ptr, {I8Ptr, Arg1, I64}, false);
      FunctionCallee C = M->getOrInsertFunction(Nm, FT);
      Value *Abs = materializeAbs(B, cast<Constant>(C.getCallee()));
      Value *FP = B.CreateBitCast(Abs, FT->getPointerTo());
      Value *A0 = B.CreateBitCast(MI->getArgOperand(0), I8Ptr);
      Value *A1 = MI->getArgOperand(1);
      if (!isa<MemSetInst>(MI))
        A1 = B.CreateBitCast(A1, I8Ptr);
      Value *A2 = B.CreateZExtOrTrunc(MI->getArgOperand(2), I64);
      B.CreateCall(FT, FP, {A0, A1, A2});
      MI->eraseFromParent();
    }

    SmallVector<Instruction *, 32> Work;
    for (BasicBlock &BB : *Body)
      for (Instruction &I : BB)
        Work.push_back(&I);

    for (Instruction *I : Work) {
      auto *CB = dyn_cast<CallBase>(I);

      // **内联汇编整条跳过**：它的操作数可能带 "i"（立即数）一类约束，
      // 换成运行期值就不满足约束了。实测 WARN_ON 的 "i"(__FILE__) 会因此
      // 报 "invalid operand for inline asm constraint 'i'"。
      // 这类函数本就被 gen_funcs.py 以"含 BUG/WARN"为由排除在随机化范围之外。
      if (CB && CB->isInlineAsm())
        continue;

      // 内建函数（llvm.memcpy 等）的 callee 必须保持直接形式，否则后端报错。
      Function *Intrin = CB ? CB->getCalledFunction() : nullptr;
      bool SkipCallee = Intrin && Intrin->isIntrinsic();

      IRBuilder<> B(I);
      for (unsigned n = 0; n < I->getNumOperands(); ++n) {
        auto *C = dyn_cast<Constant>(I->getOperand(n));
        if (!C || !refsGlobal(C))
          continue;
        if (SkipCallee && CB && n == CB->arg_size())   // callee 操作数
          continue;
        if (auto *GV = dyn_cast<GlobalValue>(C))
          if (GV->getName().startswith("__ikaslr_"))
            continue;
        I->setOperand(n, rebuildAbs(B, C));
      }
    }
  }

  bool transform(Module &M, Function *F) {
    LLVMContext &Ctx = M.getContext();
    std::string Name = F->getName().str();
    Type *I8Ptr = Type::getInt8PtrTy(Ctx);
    FunctionType *FTy = F->getFunctionType();

    // 1. 先建跳板（临时名），再把**所有既有调用点**改指向它。
    //
    //    只改名是不够的：调用指令引用的是函数**对象**而非名字，把原函数改名
    //    为 F_body 之后，调用点会跟着指向 F_body，等于绕过跳板。实测确实如此
    //    （caller_fn 直接 jmp target_fn_body）。因此必须 RAUW。
    Function *T = Function::Create(FTy, F->getLinkage(),
                                   Name + ".ikaslr.tramp", &M);
    F->replaceAllUsesWith(T);

    // 2. RAUW 之后再改名并落段：此时 F 已无调用者，安全。
    F->setName(Name + "_body");
    F->setSection(".rand.text." + Name);
    F->addFnAttr(Attribute::NoInline);

    // 关键：消除函数体内指向区域外的 PC 相对引用，否则搬移后必崩（§3.4.3）。
    makeBodyPositionIndependent(F);

    T->setName(Name);
    T->setSection(".tramp.text." + Name);
    T->addFnAttr(Attribute::NoInline);
    T->setVisibility(F->getVisibility());

    // 跳板是我们凭空造出来的函数，不会继承翻译单元的代码生成选项。内核以
    // -fno-asynchronous-unwind-tables 编译并在链接脚本里丢弃 .eh_frame，
    // 而新建函数默认带 uwtable，于是只有跳板会产生 .eh_frame，链接期报
    // "unplaced orphan section `.eh_frame'" 而失败（实测）。
    // 因此显式去掉 uwtable 并标 nounwind；同时继承原函数的这两项属性。
    T->removeFnAttr(Attribute::UWTable);   // LLVM 14 的写法
    T->addFnAttr(Attribute::NoUnwind);

    // 跳板要**继承原函数的代码生成属性**，否则它与内核其余部分不一致。
    // 尤其是 "frame-pointer"：内核开 CONFIG_FRAME_POINTER 时每个函数都建帧指针，
    // 而凭空造的函数默认不建，栈回溯走到跳板就断链（实测回溯里满是 '?'）。
    // target-cpu/target-features 关系到指令选择，也一并继承。
    for (const char *A : {"frame-pointer", "target-cpu", "target-features",
                          "no-trapping-math", "stack-protector-buffer-size"})
      if (F->hasFnAttribute(A))
        T->addFnAttr(F->getFnAttribute(A));

    // 3. target 槽，初值为函数体的链接期地址。
    //    必须在 RAUW 之后创建，否则这条引用也会被一并替换成跳板。
    auto *Target = new GlobalVariable(
        M, I8Ptr, false, GlobalValue::InternalLinkage,
        ConstantExpr::getBitCast(F, I8Ptr), "__ikaslr_target_" + Name);
    Target->setSection(".data..ikaslr_target");
    Target->setAlignment(Align(8));

    FunctionCallee Enter =
        M.getOrInsertFunction("ikaslr_enter", Type::getVoidTy(Ctx));
    FunctionCallee Leave =
        M.getOrInsertFunction("ikaslr_leave", Type::getVoidTy(Ctx));

    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", T);
    IRBuilder<> B(BB);

    B.CreateCall(Enter);
    // 必须是 volatile load：target 由随机化线程在运行期改写，不能被提升或复用。
    LoadInst *LI = B.CreateLoad(I8Ptr, Target, /*isVolatile=*/true);
    Value *Callee = B.CreateBitCast(LI, FTy->getPointerTo());

    SmallVector<Value *, 8> Args;
    for (Argument &A : T->args())
      Args.push_back(&A);
    CallInst *CI = B.CreateCall(FTy, Callee, Args);
    CI->setTailCall(false);

    B.CreateCall(Leave);
    if (FTy->getReturnType()->isVoidTy())
      B.CreateRetVoid();
    else
      B.CreateRet(CI);

    emitTableEntry(M, T, F, Target, Name);

    if (Verbose)
      errs() << "ikaslr: transformed " << Name << "\n";
    return true;
  }

  /// 只报告：随机化函数与非随机化函数之间的直接调用。
  /// 对应 §6.4 的编译期检查（构建后的映像扫描见 scripts/ikaslr/scan_xregion.py）。
  void verify(Module &M, const std::set<std::string> &Sel) {
    unsigned Cross = 0;
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      bool FIn = Sel.count(F.getName().str()) > 0;
      for (BasicBlock &BB : F)
        for (Instruction &I : BB) {
          auto *CI = dyn_cast<CallInst>(&I);
          if (!CI)
            continue;
          Function *Callee = CI->getCalledFunction();
          if (!Callee || Callee->isDeclaration())
            continue;
          bool CIn = Sel.count(Callee->getName().str()) > 0;
          if (FIn != CIn) {
            Cross++;
            errs() << "ikaslr: cross-region call " << F.getName() << " -> "
                   << Callee->getName() << "\n";
          }
        }
    }
    if (Cross)
      errs() << "ikaslr: " << Cross << " cross-region call(s) in "
             << M.getName() << "\n";
  }

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    IsAArch64 = Triple(M.getTargetTriple()).isAArch64();
    std::set<std::string> Sel = loadFuncList();
    if (Sel.empty())
      return PreservedAnalyses::all();

    if (VerifyOnly) {
      verify(M, Sel);
      return PreservedAnalyses::all();
    }

    SmallVector<Function *, 8> Todo;
    for (Function &F : M) {
      if (F.isDeclaration() || F.getName().endswith("_body"))
        continue;
      if (Sel.count(F.getName().str()))
        Todo.push_back(&F);
    }
    if (Todo.empty())
      return PreservedAnalyses::all();

    for (Function *F : Todo)
      transform(M, F);

    return PreservedAnalyses::none();
  }

  static bool isRequired() { return true; }
};

} // namespace

llvm::PassPluginLibraryInfo getIKaslrPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "IKaslr", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            // 先在流水线**最前**给选中函数打 noinline：内联器一旦把它们内联，
            // 内联副本就绕过了跳板，不变式失效。
            PB.registerPipelineStartEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(IKaslrMarkPass());
                });
            // 改造放在优化流水线末尾，使其发生在常规优化之后。
            PB.registerOptimizerLastEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(IKaslrPass());
                });
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "ikaslr") {
                    MPM.addPass(IKaslrPass());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getIKaslrPluginInfo();
}
