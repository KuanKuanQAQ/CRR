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
