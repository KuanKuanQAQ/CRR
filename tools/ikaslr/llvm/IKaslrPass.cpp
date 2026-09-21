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
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/Constants.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

static cl::opt<std::string> FuncsFile(
    "ikaslr-funcs-file", cl::init(""),
    cl::desc("File listing function names to randomize, one per line"));

static cl::opt<bool> VerifyOnly(
    "ikaslr-verify", cl::init(false),
    cl::desc("Only report cross-region calls; do not transform"));

/// 关闭 fixed_out 改写（仅用于 A/B 测量 fixed_out 本身的开销）。
/// 用环境变量而非 cl::opt：-mllvm 选项在插件注册之前就被解析，那条路走不通。
static bool envFlag(const char *Name) {
  const char *E = getenv(Name);
  return E && *E && strcmp(E, "0") != 0;
}
static bool noOutWrap() { return envFlag("IKASLR_NO_OUTWRAP"); }

static cl::opt<bool> Verbose(
    "ikaslr-verbose", cl::init(false),
    cl::desc("Print each transformed function"));

namespace {

/// 读入随机化函数名单（**保序**）。文件路径来自选项或 IKASLR_FUNCS 环境变量。
/// '#' 起始的行与空行忽略；返回的下标即返回标记里的 k
/// （0 起，只数非注释非空行）。跳转桩位于 `stub_base + 8k`，因此 pass 与
/// `link-vmlinux.sh` 生成桩文件时**必须用同一套 k 分配**（同名单、同跳过规则）。
static std::vector<std::string> loadFuncListOrdered() {
  std::vector<std::string> Names;
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
    Names.push_back(Line);
  }
  // 微基准用的被随机化函数（kernel/ikaslr/microbench.c）总是排在名单之后：它们
  // 得经过真实的跳板才量得出跳板的开销，又不该要求每份名单都手工带上。未开
  // CONFIG_IKASLR_MICROBENCH 时这些函数不存在，对应的 k 只是桩表末尾的几个空槽。
  if (!Names.empty())
    for (const char *N : {"ikaslr_mb_leaf", "ikaslr_mb_out", "ikaslr_mb_in",
                          "ikaslr_mb_ind", "ikaslr_mb_poly0", "ikaslr_mb_poly1",
                          "ikaslr_mb_poly2", "ikaslr_mb_poly3"})
      Names.push_back(N);
  return Names;
}

/// 同上，去序返回集合（成员判定用，顺序无关处沿用）。
static std::set<std::string> loadFuncList() {
  std::vector<std::string> V = loadFuncListOrdered();
  return std::set<std::string>(V.begin(), V.end());
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
  /// CONFIG_IKASLR_COUNT_GLOBAL：所有核共用第 0 对计数（扩展性实验的对照实现）。
  /// 由顶层 Makefile 经环境变量传入（cl::opt 走不通，见 noOutWrap）。
  bool CountGlobal = envFlag("IKASLR_COUNT_GLOBAL");
  std::set<std::string> Sel;/* 被随机化的函数名单，由 run() 载入 */
  std::map<std::string, unsigned> Kmap; /* 函数名 -> 返回标记里的 k（名单下标）*/
  unsigned NrOutWrapped = 0;/* 被包成 fixed_out 序列的调用点数 */
  unsigned NrTokenCalls = 0;/* arm64：改写成标记—分支序列的调用点数 */
  std::set<std::string> OutEmitted; /* arm64：本模块已发出定义的 fixed_out_<G> */
  bool NoOutWrap = false;   /* IKASLR_NO_OUTWRAP=1 时不生成 fixed_out（A/B 用）*/


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
    //   AArch64: movn/movk 三条 + 修补表（立即数编译期留空，启动期填），
    //            见下方分支的说明；`adrp` 是 PC 相对的，正是搬移后失效的那种
    //            寻址，不能用。
    const char *Asm;
    if (IsAArch64)
      // AArch64 **不用字面量池，改为 movn/movk 占位序列 + 启动期修补表**
      // （设计对齐改造 §3 问题 4，2026-09-20 可行性实验已通过）。
      //
      // 为什么放弃字面量池：第 4 章把只执行保护扩大到整个随机化区域后，函数体内
      // PC 相对的 `ldr` 去读同节字面量属于**数据读取**，会触发观察点。改为把地址
      // 编进指令立即数，函数体内从此没有任何数据读取，与 x86-64 的形态一致。
      //
      // 为什么 PIE 下可行：难点从来不是 movn/movk 本身，而是修改指令立即数字段的
      // 重定位（R_AARCH64_MOVW_UABS_*）被 PIE 链接器拒绝。这里的立即数在编译期
      // **留空**（全 0），不产生任何 MOVW 重定位；真正的地址放在 `.ikaslr_fixups`
      // 里以 `.quad` 记录（R_AARCH64_RELATIVE，PIE 允许，由内核早期重定位填好），
      // 由内核在 ikaslr_init 里一次性把它拆成三段写进映像母本的三条指令立即数
      // （变体始终从母本复制，因此天然带上补丁）。
      //
      // 三条指令够用：内核地址（映像/直接映射/vmalloc/module）高 16 位恒为全 1，
      // movn 把 bits[63:16] 置 1 并写 bits[15:0]，两条 movk 写 [31:16]、[47:32]。
      //
      // 修补表条目布局（16 字节，与 struct ikaslr_fixup 对应）：
      //   .long  1b - .   本条目到那条 movn 指令的相对偏移（跨节 label 差，
      //                    同一次链接内解析为常量）
      //   .long  0        保留（对齐到 8 字节，供运行期存放标志）
      //   .quad  sym      目标符号的绝对地址（R_AARCH64_RELATIVE）
      Asm = "1: movn $0, #0\n\t"
            "movk $0, #0, lsl #16\n\t"
            "movk $0, #0, lsl #32\n\t"
            ".pushsection .ikaslr_fixups,\"a\"\n\t"
            ".long 1b - .\n\t"
            ".long 0\n\t"
            ".quad $1\n\t"
            ".popsection";
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

  /// llvm.memcpy/memset/memmove 会被后端降级成到 memcpy 等的**直接**调用，
  /// 那是 PC 相对的。这里先在 IR 层把它们换成对内核同名符号的显式调用，
  /// 后续由 wrapOutboundCalls 包上 fixed_out、再由物化改成绝对间接调用。
  void lowerMemIntrinsics(Function *Body) {
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
      Value *A0 = B.CreateBitCast(MI->getArgOperand(0), I8Ptr);
      Value *A1 = MI->getArgOperand(1);
      if (!isa<MemSetInst>(MI))
        A1 = B.CreateBitCast(A1, I8Ptr);
      Value *A2 = B.CreateZExtOrTrunc(MI->getArgOperand(2), I64);
      B.CreateCall(C, {A0, A1, A2});
      MI->eraseFromParent();
    }
  }

  /// 运行时钩子自身不能再被包一层，否则无限递归。
  static bool isIKaslrRuntime(StringRef N) {
    return N == "ikaslr_enter" || N == "ikaslr_leave" ||
           N == "ikaslr_out_enter" || N == "ikaslr_out_leave";
  }

  /// 登记一个合法的跨区域目标（§3.5.2）。链接器把 .data..ikaslr_whitelist 汇总成
  /// 白名单，fixed_out 在放行前查询。每模块去重；跨模块的重复由内核初始化时再去重。
  void emitWhitelist(Module &M, Function *Callee) {
    std::string N = ("__ikaslr_wl_" + Callee->getName()).str();
    if (M.getNamedGlobal(N))
      return;
    Type *I8Ptr = Type::getInt8PtrTy(M.getContext());
    auto *G = new GlobalVariable(M, I8Ptr, /*isConstant=*/false,
                                 GlobalValue::InternalLinkage,
                                 ConstantExpr::getBitCast(Callee, I8Ptr), N);
    G->setSection(".data..ikaslr_whitelist");
    G->setAlignment(Align(8));
    appendToUsed(M, {G});
  }

  /// 把函数体内**离开随机化区域**的调用改写成 fixed_out 序列（§3.4.2）：
  ///
  ///     ikaslr_out_enter(target);   // 记录离开 + 白名单检查（第 5 章再叠加 PA）
  ///     r = target(...);
  ///     ikaslr_out_leave();         // 重新进入
  ///
  /// **哪些调用要包**：目标地址固定、且不属于随机化机制自身的调用，即
  ///   ② 同编译单元内的 static 函数、③ 其他编译单元的全局函数。
  /// **哪些不包**：
  ///   ① 调用被随机化的函数——RAUW 之后落到 fixed_in 跳板，跳板自己 enter/leave，
  ///      再包一层只会徒增两次原子操作；
  ///   · ikaslr_out_enter/out_leave 自身（无限递归）；
  ///   · 内联汇编、intrinsic（不是真正的调用）；
  ///   · musttail（其后不允许插指令）。
  ///
  /// **间接调用不包**：目标是运行期值，静态白名单覆盖不到，包了只会让
  /// ikaslr_out_enter 每次都走"未登记目标"的告警路径。间接调用的合法性检查
  /// 属于第 5 章 PA CFI 的范畴。
  ///
  /// 计数不平衡的安全方向：out_enter 减、out_leave 加。若调用不返回（noreturn），
  /// 计数偏**小**——区域显得更空，随机化更容易进行，不会误判为"仍有执行流"。
  /// 反向（计数偏大）才会让随机化永远等不到空，因此必须保证每个 out_enter 后面
  /// 紧跟着唯一的 out_leave：CallInst 不是终结指令，其 getNextNode() 必然存在，
  /// 插入点唯一，配对精确。
  void wrapOutboundCalls(Function *Body) {
    Module &M = *Body->getParent();
    LLVMContext &Ctx = M.getContext();
    Type *I8Ptr = Type::getInt8PtrTy(Ctx);
    FunctionCallee OutEnter =
        M.getOrInsertFunction("ikaslr_out_enter", Type::getVoidTy(Ctx), I8Ptr);
    FunctionCallee OutLeave =
        M.getOrInsertFunction("ikaslr_out_leave", Type::getVoidTy(Ctx));

    SmallVector<std::pair<CallInst *, Function *>, 32> Out;
    for (BasicBlock &BB : *Body)
      for (Instruction &I : BB) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI || CI->isInlineAsm() || CI->isMustTailCall())
          continue;
        // getCalledFunction() 对"经 bitcast 常量表达式的调用"返回 null
        //（memcpy 的声明签名与内核不一致时就是这种形态），故手工剥离。
        auto *Callee =
            dyn_cast<Function>(CI->getCalledOperand()->stripPointerCasts());
        if (!Callee || Callee->isIntrinsic())
          continue;
        StringRef N = Callee->getName();
        if (Sel.count(N.str()) || N.endswith("_body"))
          continue;                       // ① 走 fixed_in 跳板
        if (isIKaslrRuntime(N) || N.startswith("__ikaslr_"))
          continue;
        Out.push_back({CI, Callee});
      }

    for (auto &P : Out) {
      CallInst *CI = P.first;
      Function *Callee = P.second;
      IRBuilder<> B(CI);
      B.CreateCall(OutEnter, {ConstantExpr::getBitCast(Callee, I8Ptr)});
      B.SetInsertPoint(CI->getNextNode());
      B.CreateCall(OutLeave);
      CI->setTailCall(false);   // 尾调用会直接 jmp 走，跳过 out_leave
      emitWhitelist(M, Callee);
    }
    NrOutWrapped += Out.size();
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

    SmallVector<Instruction *, 32> Work;
    for (BasicBlock &BB : *Body)
      for (Instruction &I : BB)
        Work.push_back(&I);

    for (Instruction *I : Work) {
      // PHI 要特殊处理：不能在 PHI 之前插指令（PHI 必须连续位于块首），
      // 取值必须在**对应的前驱块**里物化。否则生成的是非法 IR，
      // 实测会让后端在指令选择阶段直接段错误
      // （'X86 DAG->DAG Instruction Selection' on @register_filesystem_body）。
      if (auto *PN = dyn_cast<PHINode>(I)) {
        for (unsigned n = 0; n < PN->getNumIncomingValues(); ++n) {
          auto *C = dyn_cast<Constant>(PN->getIncomingValue(n));
          if (!C || !refsGlobal(C))
            continue;
          if (auto *GV = dyn_cast<GlobalValue>(C))
            if (GV->getName().startswith("__ikaslr_"))
              continue;
          IRBuilder<> PB(PN->getIncomingBlock(n)->getTerminator());
          PN->setIncomingValue(n, rebuildAbs(PB, C));
        }
        continue;
      }

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

  /// 经栈传递参数的函数：论文的 fixed_in / fixed_out 跳板在 `bl` 之前先
  /// `stp x17, x30, [sp, #-16]!`，被调方看到的 sp 低了 16 字节，凡是经栈传递的
  /// 参数（及变参溢出区）全部错位（设计对齐改造 §3 问题 1）。作者 2026-09-20
  /// 决定：这类函数直接拒绝并记名（arm64 上 9 参以上的内核函数基本不存在）。
  ///
  /// 在 IR 层按调用约定近似判断栈传参：
  ///   AArch64 AAPCS64：整型/指针 8 个寄存器(x0-x7)、浮点 8 个(v0-v7)；
  ///   x86-64 SysV：整型/指针 6 个、浮点 8 个；sret 在 x86-64 占一个整型寄存器
  ///   （arm64 用专用的 x8，不占）。超出者或按值传聚合(byval)落栈。
  /// 变参在 rejectReason 里已单列（isVarArg），此处不重复。
  const char *stackArgReason(Function *F) {
    const unsigned IntRegs = IsAArch64 ? 8 : 6;
    const unsigned FpRegs = 8;
    unsigned IntUsed = 0, FpUsed = 0;
    AttributeList AL = F->getAttributes();
    if (AL.hasAttrSomewhere(Attribute::StructRet) && !IsAArch64)
      IntUsed = 1;                      // sret 隐藏指针占 rdi
    for (Argument &A : F->args()) {
      if (A.hasByValAttr() || A.hasAttribute(Attribute::ByRef))
        return "stack-passed aggregate arg (byval)";
      Type *T = A.getType();
      if (T->isFloatingPointTy() ||
          (T->isVectorTy() && T->getScalarType()->isFloatingPointTy()))
        ++FpUsed;
      else
        ++IntUsed;                      // 整型、指针、非浮点向量（保守归整型类）
    }
    if (IntUsed > IntRegs || FpUsed > FpRegs)
      return "stack-passed args (>8 int or >8 fp; trampoline would misalign sp)";
    return nullptr;
  }

  /// arm64 标记—分支序列（§2.3）目前只处理"≤8 个整型/指针参数、返回 void 或
  /// 整型/指针"的调用点。含浮点/按值聚合参数、sret、>8 参、musttail 的调用点
  /// 暂不支持——含这类调用点的函数整体拒绝（也就把"调用了栈传参外部函数者也拒"
  /// 一并覆盖了）。间接调用是支持的（目标是运行期的跳板地址）。
  const char *callSiteRejectReason(Function *F) {
    for (BasicBlock &BB : *F)
      for (Instruction &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB || CB->isInlineAsm())
          continue;
        Function *Cal = CB->getCalledFunction();
        if (Cal && Cal->isIntrinsic())
          continue;                   // 交后端/lowerMemIntrinsics 处理
        if (auto *CI = dyn_cast<CallInst>(CB))
          if (CI->isMustTailCall())
            return "musttail call in body (no return point for token sequence)";
        if (CB->hasStructRetAttr())
          return "sret call in body (aggregate return via hidden ptr)";
        unsigned IntN = 0;
        for (unsigned i = 0; i < CB->arg_size(); ++i) {
          if (CB->paramHasAttr(i, Attribute::ByVal) ||
              CB->paramHasAttr(i, Attribute::StructRet))
            return "fp/byval/sret arg in body call";
          Type *T = CB->getArgOperand(i)->getType();
          if (T->isFloatingPointTy() ||
              (T->isVectorTy() && T->getScalarType()->isFloatingPointTy()))
            return "fp arg in body call (token sequence handles int/ptr only)";
          ++IntN;
        }
        if (IntN > 8)
          return "call with >8 int args in body (would misalign sp)";
        Type *RT = CB->getType();
        if (!RT->isVoidTy() && !RT->isIntegerTy() && !RT->isPointerTy())
          return "non-int/ptr return in body call";
      }
    return nullptr;
  }

  /// 这个函数能不能做成"跳板 + 可搬移函数体"？不能的话说明为什么。
  /// 返回 nullptr 表示可以。
  const char *rejectReason(Function *F) {
    // 变参函数：跳板要把收到的变参原样转给函数体，而 IR 层做不到——
    // 只能转发**具名**参数，变参部分连同 x86-64 的 %al（向量寄存器计数）
    // 一起丢失，函数体里的 va_start 读到的就是垃圾。
    // 唯一能原样转发的是 musttail call，但 musttail 要求其后紧跟 ret，
    // 而跳板在调用之后**必须**执行 ikaslr_leave()，两者不相容。
    // 实测：随机化 seq_printf 会让 /proc/self/status 读出垃圾，
    // 表现为 vsnprintf 的 "field width too large" 告警与读野指针的缺页。
    if (F->isVarArg())
      return "variadic (cannot forward varargs through a trampoline)";
    // naked 函数没有编译器生成的序言/尾声，函数体就是一段裸汇编，
    // 既不能加跳板也谈不上位置无关改造。
    if (F->hasFnAttribute(Attribute::Naked))
      return "naked (hand-written prologue)";
    // 经栈传递参数的函数：跳板 stp 后 bl 会使被调方栈参数错位。
    if (const char *R = stackArgReason(F))
      return R;
    // arm64 标记—分支序列对函数体内调用点的限制。
    if (IsAArch64)
      if (const char *R = callSiteRejectReason(F))
        return R;
    return nullptr;
  }

  /// arm64：把函数体内每个调用点改写成 §2.3 的"标记装入 x30 + 物化目标到 x16 +
  /// br"序列（方案甲）。直接调用的目标是 fixed_out_<G>（外部）或 <callee> 本身
  /// （被随机化函数的入口跳板即其原名）；间接调用的目标是运行期函数指针值
  /// （取地址得到的就是跳板地址）。两者都用 `b`/`br` 而非 `bl`：`bl` 会把真实
  /// 返回地址写进 x30 落到栈上，正是返回标记要消除的泄露与陈旧返回来源。
  ///
  /// 参数按 AAPCS64 钉到 x0..x7（本 pass 已在 callSiteRejectReason 里保证 ≤8 个
  /// 整型/指针参数、无浮点/聚合），统一 zext/ptrtoint 到 i64 传入；返回值经 x0
  /// 取回再截断/inttoptr 回原类型。整段等价于一次调用，故把全部调用者保存寄存器
  /// 列为 clobber，并要求函数体带 "frame-pointer"="all"（否则调用全变内联汇编后
  /// 被当叶函数，x30 不保存而被本序列破坏）。
  ///
  /// k 为**包含函数**的名单下标，用于 resolve_token 找回本函数的重入桩；
  /// Δ = 返回点 - 函数体入口，由两条 adr 相减在运行期算出，与搬移无关。
  void rewriteCallSitesAArch64(Module &M, Function *Body, unsigned K) {
    LLVMContext &Ctx = M.getContext();
    Type *I8Ptr = Type::getInt8PtrTy(Ctx);
    Type *I64 = Type::getInt64Ty(Ctx);

    SmallVector<CallInst *, 32> Calls;
    for (BasicBlock &BB : *Body)
      for (Instruction &I : BB)
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          if (CI->isInlineAsm())
            continue;
          Function *Cal = CI->getCalledFunction();
          if (Cal && Cal->isIntrinsic())
            continue;
          Calls.push_back(CI);
        }

    for (CallInst *CI : Calls) {
      IRBuilder<> B(CI);
      Function *Callee = dyn_cast_or_null<Function>(
          CI->getCalledOperand()->stripPointerCasts());
      bool Indirect = !Callee;

      // 收集参数，统一成 i64。
      //
      // 参数里若是引用全局符号的常量（&global、GEP 常量表达式等）必须**先绝对
      // 物化**再放进标记序列的内联汇编：makeBodyPositionIndependent 会“整条跳过
      // 内联汇编”，而标记序列本身就是内联汇编且在它之前生成，因此这类参数不会被
      // 它处理，后端会把 &global 降级成 PC 相对的 adrp/add——函数体一搬移就指错
      // （实测 find_vm_area 体内 spin_lock(&vmap_area_lock) 在 S3 变体里崩在错误
      // 的锁地址上）。这里就地物化，与函数体内其它全局引用一致走 .ikaslr_fixups。
      SmallVector<Value *, 10> Ops;
      unsigned N = CI->arg_size();
      for (unsigned i = 0; i < N; ++i) {
        Value *A = CI->getArgOperand(i);
        if (auto *C = dyn_cast<Constant>(A))
          if (refsGlobal(C))
            A = rebuildAbs(B, C);
        Type *T = A->getType();
        if (T->isPointerTy())
          A = B.CreatePtrToInt(A, I64);
        else if (T->isIntegerTy())
          A = B.CreateZExtOrTrunc(A, I64);
        Ops.push_back(A);
      }

      Type *RT = CI->getType();
      bool HasOut = !RT->isVoidTy();

      // 目标符号 / 目标值。
      GlobalValue *TargetSym = nullptr;   // 直接调用
      Value *TargetVal = nullptr;         // 间接调用：运行期目标指针
      GlobalValue *IoutSym = nullptr;     // 间接调用：__ikaslr_indirect_out 符号
      if (Indirect) {
        TargetVal = B.CreatePtrToInt(CI->getCalledOperand(), I64);
        IoutSym = M.getNamedValue("__ikaslr_indirect_out");
        if (!IoutSym)
          IoutSym = Function::Create(
              FunctionType::get(Type::getVoidTy(Ctx), false),
              GlobalValue::ExternalLinkage, "__ikaslr_indirect_out", &M);
      } else if (Sel.count(Callee->getName().str())) {
        // 被随机化函数：token+br 到它所在模块导出的 __ikaslr_in_<Kc>，而非函数名
        // 本身。转换成功者该符号是 fixed_in 的别名；被拒者是 fixed_out 式包装。
        // 这样即便被调方在它自己的 TU 里被拒（本模块看不到），也不会 br 到一个末尾
        // 是 ret 的普通函数上（曾致 S3 崩溃）。
        std::string IN = "__ikaslr_in_" + std::to_string(Kmap[Callee->getName().str()]);
        GlobalValue *G = M.getNamedValue(IN);
        if (!G)
          G = Function::Create(FunctionType::get(Type::getVoidTy(Ctx), false),
                               GlobalValue::ExternalLinkage, IN, &M);
        TargetSym = G;
      } else {
        // 外部目标：出口跳板 fixed_out_<G>。每模块本地一份（不同模块各自发出，
        // 故用 InternalLinkage，避免跨模块重定义）。
        std::string GN = Callee->getName().str();
        std::string TN = "fixed_out_" + GN;
        GlobalValue *G = M.getNamedValue(TN);
        if (!G)
          G = Function::Create(FunctionType::get(Type::getVoidTy(Ctx), false),
                               GlobalValue::InternalLinkage, TN, &M);
        TargetSym = G;
        if (OutEmitted.insert(GN).second)
          M.appendModuleInlineAsm(buildFixedOutAsm(GN, Callee->hasLocalLinkage()));
        emitWhitelist(M, Callee);
      }

      // 组装内联汇编。**参数寄存器 x0..x(N-1) 全部标为输入输出（in-out）**：
      // 一次真正的调用会破坏所有参数寄存器，若只当作输入，LLVM 会以为它们在
      // asm 之后仍保留原值，从而把跨调用存活的值放在参数寄存器里而不落栈——
      // 实测 kern_path 把 getname 结果留在 x1、经 filename_lookup 破坏后当 filename
      // 传给 putname，崩在 putname(0xc)。把 x0..x(N-1) 做成输出即修复。
      //
      // 输出个数 OutRegs = max(N, 有返回值?1:0)；x0..x(OutRegs-1) 皆为输出，
      // 前 N 个各绑定一个参数（tied），x0 兼作返回值。其余调用者保存寄存器进 clobber。
      unsigned OutRegs = N > (HasOut ? 1u : 0u) ? N : (HasOut ? 1u : 0u);

      std::string Cons;
      auto addSep = [&](const std::string &s) {
        if (!Cons.empty() && Cons.back() != ',')
          Cons += ",";
        Cons += s;
      };
      // 输出：={x0}..={x(OutRegs-1)}
      for (unsigned r = 0; r < OutRegs; ++r)
        addSep("={x" + std::to_string(r) + "}");
      // 输入：前 N 个参数各 tied 到对应输出（放进 x0..x(N-1) 且标记被写）
      SmallVector<Type *, 10> ParamTys;
      for (unsigned i = 0; i < N; ++i) {
        addSep(std::to_string(i));        // tie 到输出 i
        ParamTys.push_back(I64);
      }
      unsigned FIdx = OutRegs + N;        // Fbody 的 $ 编号
      addSep("s");                        // Fbody
      ParamTys.push_back(I8Ptr);
      unsigned PtrIdx = 0, IoutIdx = 0, TIdx = 0;
      if (Indirect) {
        PtrIdx = FIdx + 1;                // 运行期目标指针
        addSep("r");
        ParamTys.push_back(I64);
        IoutIdx = FIdx + 2;              // __ikaslr_indirect_out 符号（绝对物化）
        addSep("s");
        ParamTys.push_back(I8Ptr);
      } else {
        TIdx = FIdx + 1;                 // 目标跳板符号
        addSep("s");
        ParamTys.push_back(I8Ptr);
      }
      // clobber：x(OutRegs)..x18、x30、向量寄存器、条件码、内存。
      for (unsigned r = OutRegs; r <= 18; ++r)
        addSep("~{x" + std::to_string(r) + "}");
      addSep("~{x30}");
      for (unsigned v = 0; v <= 7; ++v)
        addSep("~{v" + std::to_string(v) + "}");
      for (unsigned v = 16; v <= 31; ++v)
        addSep("~{v" + std::to_string(v) + "}");
      addSep("~{cc}");
      addSep("~{memory}");

      // 汇编模板。参数由约束放进 x0..x(N-1)，模板不引用。所有跳出目标都必须
      // **绝对物化**再 br：函数体会被搬移，任何指向区域外固定符号的 PC 相对 `b`
      // 在搬移后都指错（实测间接调用用 `b __ikaslr_indirect_out` 时，变体里算成
      // 变体地址−16.8MB 而崩溃）。
      std::string F = "$" + std::to_string(FIdx);
      std::string Asm;
      Asm += "adr x30, 1f\n\t";
      Asm += "adr x16, " + F + "\n\t";
      Asm += "sub x30, x30, x16\n\t";
      Asm += "movk x30, #" + std::to_string(K) + ", lsl #32\n\t";
      if (Indirect) {
        // 间接调用：目标可能是普通函数（末尾 ret）。不能把标记放进 x30 直接跳，
        // 否则它会 ret 到标记崩溃。经固定的 __ikaslr_indirect_out（目标放 x16，
        // thunk 用真实返回地址 blr、返回后按标记解析回体）。thunk 地址亦绝对物化
        // 到 x17（不能 PC 相对 b，见上）。
        Asm += "mov x16, $" + std::to_string(PtrIdx) + "\n\t";
        Asm += "3: movn x17, #0\n\t movk x17, #0, lsl #16\n\t "
               "movk x17, #0, lsl #32\n\t";
        Asm += ".pushsection .ikaslr_fixups,\"a\"\n\t"
               ".long 3b - .\n\t .long 0\n\t .quad $" +
               std::to_string(IoutIdx) + "\n\t.popsection\n\t";
        Asm += "br x17\n\t";
      } else {
        // 直接调用：目标是固定的入口/出口跳板，物化其绝对地址后 br。
        Asm += "3: movn x16, #0\n\t movk x16, #0, lsl #16\n\t "
               "movk x16, #0, lsl #32\n\t";
        Asm += ".pushsection .ikaslr_fixups,\"a\"\n\t"
               ".long 3b - .\n\t .long 0\n\t .quad $" +
               std::to_string(TIdx) + "\n\t.popsection\n\t";
        Asm += "br x16\n\t";
      }
      Asm += "1:\n\t";

      // 返回类型：OutRegs 个 i64（0 个→void，1 个→i64，多个→匿名结构体）。
      Type *AsmRet;
      if (OutRegs == 0)
        AsmRet = Type::getVoidTy(Ctx);
      else if (OutRegs == 1)
        AsmRet = I64;
      else {
        SmallVector<Type *, 8> El(OutRegs, I64);
        AsmRet = StructType::get(Ctx, El);
      }
      FunctionType *AsmTy = FunctionType::get(AsmRet, ParamTys, false);
      InlineAsm *IA = InlineAsm::get(AsmTy, Asm, Cons, /*hasSideEffects=*/true);

      SmallVector<Value *, 10> Args(Ops.begin(), Ops.end());
      Args.push_back(ConstantExpr::getBitCast(Body, I8Ptr));  // Fbody
      if (Indirect) {
        Args.push_back(TargetVal);                             // 目标指针
        Args.push_back(ConstantExpr::getBitCast(IoutSym, I8Ptr)); // thunk 符号
      } else {
        Args.push_back(ConstantExpr::getBitCast(TargetSym, I8Ptr));
      }

      CallInst *NewCI = B.CreateCall(IA, Args);

      if (HasOut) {
        Value *R = (OutRegs == 1) ? (Value *)NewCI
                                  : (Value *)B.CreateExtractValue(NewCI, 0);
        if (RT->isPointerTy())
          R = B.CreateIntToPtr(R, RT);
        else
          R = B.CreateZExtOrTrunc(R, RT);
        CI->replaceAllUsesWith(R);
      }
      CI->eraseFromParent();
      ++NrTokenCalls;
    }
  }

  // ---- 方案甲的汇编跳板（arm64，论文 §3.3.2 形态）----
  // 模板与逐条说明见 kernel/ikaslr/tramp.S 头注释；追踪协议见 kernel/ikaslr/track.c。
  // 跳板只用 x16/x17 与 x9/x10，不碰参数/返回值寄存器，热路径无寄存器保存。

  /// 本核计数加一。x16 = current；破坏 x9、x10、x17。Exit 选 exit（+8）还是 enter。
  /// thread_info 的字段偏移经绝对符号 + :lo12: 在链接期填入（tramp.S 定义）。
  std::string countAsm(bool Exit) {
    std::string Sym = Exit ? "ikaslr_cnt+8" : "ikaslr_cnt";
    std::string s;
    if (!CountGlobal)
      s += "\tldr\tw17, [x16, :lo12:__ikaslr_ti_cpu]\n";
    s += "\tadrp\tx9, " + Sym + "\n";
    s += "\tadd\tx9, x9, :lo12:" + Sym + "\n";
    if (!CountGlobal)
      s += "\tadd\tx9, x9, x17, lsl #6\n";
    s += "1:\tldxr\tx17, [x9]\n";
    s += "\tadd\tx17, x17, #1\n";
    s += "\tstxr\tw10, x17, [x9]\n";
    s += "\tcbnz\tw10, 1b\n";
    return s;
  }

  static std::string setInsideAsm() {
    return "\tdmb\tish\n"
           "\tmov\tw17, #1\n"
           "\tstr\tw17, [x16, :lo12:__ikaslr_ti_inside]\n";
  }

  static std::string loadBlockedAsm() {
    return "\tadrp\tx9, ikaslr_blocked\n"
           "\tldr\tw9, [x9, :lo12:ikaslr_blocked]\n";
  }

  /// 每函数入口跳板 fixed_in_<Name>，符号名即原函数名（外部调用者不改）。
  /// 落在 .tramp.text.<Name>（固定区）。经 `bl __ikaslr_stub_<K>` 直接分支到桩。
  std::string buildFixedInAsm(const std::string &Name, unsigned K, bool External) {
    std::string s;
    std::string sec = ".tramp.text." + Name;
    s += "\t.pushsection " + sec + ",\"ax\",%progbits\n";
    if (External)
      s += "\t.globl " + Name + "\n";
    s += "\t.type " + Name + ",%function\n";
    // 每随机化函数由**本模块**导出 __ikaslr_in_<K>：区域内调用点一律 token+br 到它
    // （见 rewriteCallSitesAArch64）。转换成功者它就是 fixed_in 的别名；被拒者由
    // buildRejectedInAsm 发一个 fixed_out 式包装。这样调用方不必知道被调方在它自己
    // 那个 TU 里是否被拒（跨模块拒绝不一致曾致 S3 崩在 token 上）。
    s += "\t.globl __ikaslr_in_" + std::to_string(K) + "\n";
    s += "__ikaslr_in_" + std::to_string(K) + ":\n";
    s += Name + ":\n";
    s += "\tmrs\tx16, sp_el0\n";
    s += "\tldr\tw17, [x16, :lo12:__ikaslr_ti_inside]\n";
    s += "\tstp\tx17, x30, [sp, #-16]!\n";   // 进入前的标志 + 返回地址/标记
    s += "\tcbnz\tw17, 2f\n";                // 区域内部的调用：不计数
    s += countAsm(/*Exit=*/false);
    s += setInsideAsm();
    s += "2:";
    s += loadBlockedAsm();
    s += "\tcbnz\tw9, 8f\n";
    s += "3:\tbl\t__ikaslr_stub_" + std::to_string(K) + "\n";  // 入口桩 -> 函数体
    s += "\tldr\tw17, [sp]\n";
    s += "\tcbnz\tw17, 5f\n";
    s += "\tmrs\tx16, sp_el0\n";             // 跨边界返回：清标志先于 exit++
    s += "\tstr\twzr, [x16, :lo12:__ikaslr_ti_inside]\n";
    s += countAsm(/*Exit=*/true);
    s += "\tldp\tx17, x30, [sp], #16\n";
    s += "\tret\n";
    s += "5:";                               // 返回到区域内的调用者
    s += loadBlockedAsm();
    s += "\tcbnz\tw9, 9f\n";
    s += "6:\tldp\tx17, x30, [sp], #16\n";
    s += "\ttbz\tx30, #63, 7f\n";
    s += "\tret\n";                          // 兜底：继承标志的异常处理程序
    s += "7:\tb\tresolve_token\n";
    s += "8:\tbl\t__ikaslr_blocked_slow\n";
    s += "\tb\t3b\n";
    s += "9:\tbl\t__ikaslr_blocked_slow\n";
    s += "\tb\t6b\n";
    s += "\t.size " + Name + ", .-" + Name + "\n";
    s += "\t.popsection\n";
    return s;
  }

  /// 每目标出口跳板 fixed_out_<GName>。本地符号（每模块一份）。`bl G` 写死在
  /// 只读固定区，目标集合由构造保证；总是经 resolve_token 回函数体。
  std::string buildFixedOutAsm(const std::string &GName, bool /*GLocal*/) {
    std::string t = "fixed_out_" + GName;
    std::string s;
    s += "\t.pushsection .tramp.text." + t + ",\"ax\",%progbits\n";
    s += "\t.type " + t + ",%function\n";
    s += t + ":\n";
    s += "\tmrs\tx16, sp_el0\n";
    s += "\tldr\tw17, [x16, :lo12:__ikaslr_ti_inside]\n";
    s += "\tstp\tx17, x30, [sp, #-16]!\n";   // 进入前的标志 + 返回标记
    s += "\tcbz\tw17, 2f\n";                 // 未被计数的执行流：直接调用
    s += "\tstr\twzr, [x16, :lo12:__ikaslr_ti_inside]\n";
    s += countAsm(/*Exit=*/true);            // 离开区域
    s += "2:\tbl\t" + GName + "\n";
    s += "\tldr\tw17, [sp]\n";
    s += "\tcbz\tw17, 4f\n";
    s += "\tmrs\tx16, sp_el0\n";
    s += countAsm(/*Exit=*/false);           // 回到区域：先加一再查阻断
    s += setInsideAsm();
    s += loadBlockedAsm();
    s += "\tcbnz\tw9, 9f\n";
    s += "4:\tldp\tx17, x30, [sp], #16\n";
    s += "\tb\tresolve_token\n";
    s += "9:\tbl\t__ikaslr_blocked_slow\n";
    s += "\tb\t4b\n";
    s += "\t.size " + t + ", .-" + t + "\n";
    s += "\t.popsection\n";
    return s;
  }

  /// 名单里但被拒的函数的 __ikaslr_in_<K>：它不被随机化，函数体留在原地，因此
  /// 区域内调用点对它的 token+br 应当被当作**离开区域调用一个外部函数**处理——
  /// 即一个 fixed_out 式包装（正常 bl 原函数，返回后按标记 resolve_token 回去）。
  /// 全局符号，由本函数的定义模块发出一份。签名被拒者（>8 参）不会被这条路命中：
  /// 任何要 token 调用它的函数自己就先因“体内 >8 参调用”被拒了。
  std::string buildRejectedInAsm(const std::string &Name, unsigned K) {
    std::string sym = "__ikaslr_in_" + std::to_string(K);
    std::string s;
    s += "\t.pushsection .tramp.text." + sym + ",\"ax\",%progbits\n";
    s += "\t.globl " + sym + "\n";
    s += "\t.type " + sym + ",%function\n";
    s += sym + ":\n";
    s += "\tmrs\tx16, sp_el0\n";
    s += "\tldr\tw17, [x16, :lo12:__ikaslr_ti_inside]\n";
    s += "\tstp\tx17, x30, [sp, #-16]!\n";
    s += "\tcbz\tw17, 2f\n";
    s += "\tstr\twzr, [x16, :lo12:__ikaslr_ti_inside]\n";
    s += countAsm(/*Exit=*/true);              // 离开区域
    s += "2:\tbl\t" + Name + "\n";           // 调原地未搬移的函数
    s += "\tldr\tw17, [sp]\n";
    s += "\tcbz\tw17, 4f\n";
    s += "\tmrs\tx16, sp_el0\n";
    s += countAsm(/*Exit=*/false);             // 回到区域
    s += setInsideAsm();
    s += loadBlockedAsm();
    s += "\tcbnz\tw9, 9f\n";
    s += "4:\tldp\tx17, x30, [sp], #16\n";
    s += "\ttbz\tx30, #63, 5f\n";
    s += "\tret\n";                           // 兜底：真实返回地址（继承标志的异常）
    s += "5:\tb\tresolve_token\n";
    s += "9:\tbl\t__ikaslr_blocked_slow\n";
    s += "\tb\t4b\n";
    s += "\t.size " + sym + ", .-" + sym + "\n";
    s += "\t.popsection\n";
    return s;
  }

  /// arm64（方案甲）改造一个函数：F 改名 F_body 落 .rand.text；发 prefix data
  /// 重入序列、`__ikaslr_body_<k>` 别名、汇编入口跳板（符号名=原名）。不建 IR
  /// C 跳板、不发 target 槽——地址翻译改由链接期桩 + resolve_token 承担。
  bool transformAArch64(Module &M, Function *F) {
    LLVMContext &Ctx = M.getContext();
    std::string Name = F->getName().str();
    Type *I32 = Type::getInt32Ty(Ctx);
    FunctionType *FTy = F->getFunctionType();
    unsigned K = Kmap[Name];
    bool External = !F->hasLocalLinkage();

    // 1. 建一个**声明**（无函数体）占用原名，RAUW 既有调用点指向它；真正的
    //    定义由下面的 module asm 发出（已验证：IR 声明 + 模块汇编定义同名符号
    //    可正确链接，同模块调用者落到汇编跳板）。
    Function *T = Function::Create(FTy, F->getLinkage(),
                                   Name + ".ikaslr.tramp", &M);
    F->replaceAllUsesWith(T);

    // 2. 改名落段。占名声明随即改回原名——必须在改写调用点**之前**：自递归函数
    //    体内对自己的调用此刻已指向 T，调用点改写按名字判断目标是否在名单里，
    //    T 若还叫 "<Name>.ikaslr.tramp" 就会被当成外部函数、发出一个引用不存在
    //    符号的 fixed_out（S3 规模下 ___pskb_trim 等 6 个递归函数实测链接失败）。
    F->setName(Name + "_body");
    T->setName(Name);
    F->setSection(".rand.text." + Name);
    F->addFnAttr(Attribute::NoInline);
    F->addFnAttr("frame-pointer", "all");   // 调用全变内联汇编后仍要保存 x30

    // 3. 函数体内部：mem intrinsic 降级、调用点改标记序列、数据引用绝对物化。
    lowerMemIntrinsics(F);
    rewriteCallSitesAArch64(M, F, K);
    makeBodyPositionIndependent(F);

    // 4. prefix data：重入序列三条指令（编码固定），随函数体一起搬移。
    //    重入桩 `b F_body-12` 落到这里：adr 取当前入口、加 Δ、br。
    Constant *Pre = ConstantArray::get(
        ArrayType::get(I32, 3),
        {ConstantInt::get(I32, 0x10000070),   // adr  x16, .+12
         ConstantInt::get(I32, 0x8b110210),   // add  x16, x16, x17
         ConstantInt::get(I32, 0xd61f0200)}); // br   x16
    F->setPrefixData(Pre);

    // 5. 别名 __ikaslr_body_<k> -> F_body：M2 链接期用 nm 查出哪些 k 存在并生成
    //    对应的跳转桩；k 与名单下标一致（见 loadFuncListOrdered）。
    GlobalAlias::create(F->getValueType(), 0, GlobalValue::ExternalLinkage,
                        "__ikaslr_body_" + std::to_string(K), F);

    // 6. module asm 定义原名符号（fixed_in_F）；IR 里的 T 只是它的声明。
    M.appendModuleInlineAsm(buildFixedInAsm(Name, K, External));

    if (Verbose)
      errs() << "ikaslr: transformed " << Name << " (k=" << K << ")\n";
    return true;
  }

  bool transform(Module &M, Function *F) {
    if (IsAArch64)
      return transformAArch64(M, F);

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

    // x86-64 旧路径（待 M7 按方案甲移植）。三步，顺序不能换：
    //   1. 把 llvm.mem* 降级成对 memcpy/memset/memmove 的显式调用；
    //   2. 把离开区域的调用包成 fixed_out 的 C 调用序列；
    //   3. 消除所有指向区域外的 PC 相对引用（§3.4.3），顺带把 out_enter/leave 绝对化。
    lowerMemIntrinsics(F);
    if (!NoOutWrap)
      wrapOutboundCalls(F);
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
    // 参数与返回值的属性决定 ABI：sret（返回大结构体的隐藏指针）、byval
    // （按值传结构体）、zeroext/signext（窄整型的扩展方式）。跳板若缺了它们，
    // 调用者按一种约定传、跳板按另一种收，是静默的寄存器/栈错位。
    // 必须原样搬到跳板签名上，稍后也要原样加到跳板对函数体的调用上。
    T->setAttributes(F->getAttributes());

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
    {
      // 只搬参数与返回值属性，不搬函数级属性（那些属于函数本身，不属于调用点）。
      AttributeList AL = F->getAttributes();
      SmallVector<AttributeSet, 8> PA;
      for (unsigned n = 0; n < FTy->getNumParams(); ++n)
        PA.push_back(AL.getParamAttrs(n));
      CI->setAttributes(
          AttributeList::get(Ctx, AttributeSet(), AL.getRetAttrs(), PA));
    }

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
    NoOutWrap = noOutWrap();
    std::vector<std::string> Order = loadFuncListOrdered();
    Sel = std::set<std::string>(Order.begin(), Order.end());
    Kmap.clear();
    for (unsigned k = 0; k < Order.size(); ++k)
      Kmap.emplace(Order[k], k);        // k = 名单下标 = 返回标记里的 k
    if (Sel.empty())
      return PreservedAnalyses::all();

    if (VerifyOnly) {
      verify(M, Sel);
      return PreservedAnalyses::all();
    }

    SmallVector<Function *, 8> Todo;
    SmallVector<std::string, 4> Skipped;
    for (Function &F : M) {
      if (F.isDeclaration() || F.getName().endswith("_body"))
        continue;
      if (!Sel.count(F.getName().str()))
        continue;
      if (const char *Why = rejectReason(&F)) {
        errs() << "ikaslr: skipping " << F.getName() << ": " << Why << "\n";
        Skipped.push_back(F.getName().str());
        // 仍要为它导出 __ikaslr_in_<K>（fixed_out 式包装），否则别的模块里对它的
        // token+br 会落到一个不处理标记的普通函数上。
        if (IsAArch64)
          M.appendModuleInlineAsm(
              buildRejectedInAsm(F.getName().str(), Kmap[F.getName().str()]));
        continue;
      }
      Todo.push_back(&F);
    }
    if (Todo.empty())
      return Skipped.empty() ? PreservedAnalyses::all()
                             : PreservedAnalyses::none();

    for (Function *F : Todo)
      transform(M, F);

    if (Verbose || NrOutWrapped || NrTokenCalls)
      errs() << "ikaslr: " << M.getName() << ": " << Todo.size()
             << " function(s), "
             << (IsAArch64 ? NrTokenCalls : NrOutWrapped)
             << (IsAArch64 ? " call site(s) rewritten to token sequence"
                           : " outbound call(s) routed through fixed_out")
             << (NoOutWrap && !IsAArch64 ? " [DISABLED]" : "") << "\n";

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
