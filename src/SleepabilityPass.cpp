//===- SleepabilityPass.cpp -----------------------------------------------===//
//
// A standalone, out-of-tree LLVM module pass (new pass manager) that computes,
// for every defined function in a module, whether it "may sleep" -- the kernel
// CanSleep property. A function may sleep if it can, directly or transitively,
// reach a blocking primitive.
//
// This is the C-side ("callee summary") half of a Cross-Language Sleepability
// Checker (CLSC). The Rust typestate side and the FFI join are deliberately out
// of scope for this artifact (see README).
//
// Pipeline:
//   1. Build the call graph.
//   2. Mark leaf blocking primitives by name (mutex_lock, schedule, msleep,
//      might_sleep), plus the argument-sensitive allocator kmalloc.
//   3. Propagate CanSleep bottom-up over the call graph's strongly connected
//      components (scc_iterator yields callees before callers).
//   4. Print a deterministic per-function verdict.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace {

//===----------------------------------------------------------------------===//
// Knowledge base: what counts as a leaf blocking primitive.
//===----------------------------------------------------------------------===//

// Unconditionally-blocking kernel primitives, recognised by symbol name.
// These are external declarations in the module; they are the atoms of the
// analysis.
static const std::set<std::string> BlockingPrimitives = {
    "mutex_lock",
    "schedule",
    "msleep",
    "might_sleep",
};

// The allocator kmalloc is *argument sensitive*: it may sleep iff its gfp_t
// flags request direct reclaim.
static const char *AllocPrimitive = "kmalloc";

// The sleep-relevant bit of a gfp_t mask. In the real kernel this is
// __GFP_DIRECT_RECLAIM: GFP_KERNEL sets it (may sleep), GFP_ATOMIC does not
// (atomic-safe). Kept in one place so it can be re-pointed at real kernel
// headers.
static const uint64_t GFP_DIRECT_RECLAIM_BIT = 0x400;

//===----------------------------------------------------------------------===//
// Result model.
//===----------------------------------------------------------------------===//

enum class Verdict { AtomicSafe, MaySleep };

struct FuncResult {
  Verdict verdict = Verdict::AtomicSafe;
  std::string reason;
};

//===----------------------------------------------------------------------===//
// Argument sensitivity for gfp flags.
//===----------------------------------------------------------------------===//

// Does this gfp flags value *provably* request direct reclaim (i.e. may sleep)?
//
//   Known == true,  return true   -> provably sleeps
//   Known == true,  return false  -> provably atomic-safe
//   Known == false, return false  -> unprovable (caller applies a conservative
//                                     policy)
//
// Handles a literal constant, and -- as the stretch goal -- the intraprocedural
// known-bits case `X | C` where the constant C already sets the reclaim bit
// (an OR can only set bits, so the result sets it regardless of X).
static bool flagsMaySleep(const Value *Flags, bool &Known) {
  Known = false;

  if (const auto *CI = dyn_cast<ConstantInt>(Flags)) {
    Known = true;
    return (CI->getZExtValue() & GFP_DIRECT_RECLAIM_BIT) != 0;
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(Flags)) {
    if (BO->getOpcode() == Instruction::Or) {
      for (const Value *Op : {BO->getOperand(0), BO->getOperand(1)}) {
        if (const auto *CI = dyn_cast<ConstantInt>(Op)) {
          if ((CI->getZExtValue() & GFP_DIRECT_RECLAIM_BIT) != 0) {
            Known = true; // definitely set, whatever the other operand is
            return true;
          }
        }
      }
    }
  }

  return false; // flags not provably reclaim-bearing and not provably clean
}

//===----------------------------------------------------------------------===//
// Direct (non-transitive) blocking check for a single function.
//===----------------------------------------------------------------------===//

// If F itself performs a blocking operation, return a human-readable reason;
// otherwise return "".
static std::string directBlockingReason(const Function &F) {
  for (const Instruction &I : instructions(F)) {
    const auto *CB = dyn_cast<CallBase>(&I);
    if (!CB)
      continue;
    const Function *Callee = CB->getCalledFunction();
    if (!Callee) // indirect / function-pointer call: out of scope (see README)
      continue;
    StringRef Name = Callee->getName();

    if (BlockingPrimitives.count(Name.str()))
      return (Twine("calls blocking primitive '") + Name + "()'").str();

    if (Name == AllocPrimitive && CB->arg_size() >= 2) {
      const Value *Flags = CB->getArgOperand(1);
      bool Known = false;
      bool Sleeps = flagsMaySleep(Flags, Known);
      if (Sleeps)
        return "calls kmalloc() with direct-reclaim gfp flags (may sleep)";
      if (Known)
        continue; // provably atomic allocation (e.g. GFP_ATOMIC): not blocking
      return "calls kmalloc() with non-constant gfp flags "
             "(conservatively may sleep)";
    }
  }
  return "";
}

//===----------------------------------------------------------------------===//
// The pass.
//===----------------------------------------------------------------------===//

struct SleepabilityPass : PassInfoMixin<SleepabilityPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    CallGraph CG(M);
    DenseMap<const Function *, FuncResult> Results;

    // Verdict for F from its own body plus already-finalised callees.
    // Same-SCC callees are intentionally invisible here: they are not yet in
    // Results, and within-SCC sleeping is captured by the direct scan below.
    auto reasonFor =
        [&](const Function *F) -> std::pair<bool, std::string> {
      std::string DR = directBlockingReason(*F);
      if (!DR.empty())
        return {true, DR};
      for (const Instruction &Inst : instructions(*F)) {
        const auto *CB = dyn_cast<CallBase>(&Inst);
        if (!CB)
          continue;
        const Function *Callee = CB->getCalledFunction();
        if (!Callee)
          continue;
        auto It = Results.find(Callee);
        if (It != Results.end() && It->second.verdict == Verdict::MaySleep)
          return {true,
                  (Twine("reaches may-sleep callee ") + Callee->getName() + "()")
                      .str()};
      }
      return {false, "no path to a blocking primitive"};
    };

    // 1-3. Walk SCCs bottom-up. scc_iterator over the call graph yields SCCs in
    // reverse-topological order, so every callee is finalised before its
    // callers. A whole SCC collapses to one verdict: if any member sleeps
    // (directly, or via a finalised lower SCC), all members may sleep.
    for (scc_iterator<CallGraph *> It = scc_begin(&CG); !It.isAtEnd(); ++It) {
      std::vector<const Function *> Funcs;
      for (CallGraphNode *N : *It)
        if (const Function *F = N->getFunction())
          if (!F->isDeclaration())
            Funcs.push_back(F);
      if (Funcs.empty())
        continue;

      bool Sleeps = false;
      const Function *Trigger = nullptr;
      std::string TrigReason;
      for (const Function *F : Funcs) {
        auto R = reasonFor(F);
        if (R.first) {
          Sleeps = true;
          Trigger = F;
          TrigReason = R.second;
          break;
        }
      }

      Verdict V = Sleeps ? Verdict::MaySleep : Verdict::AtomicSafe;
      for (const Function *F : Funcs) {
        std::string Reason;
        if (!Sleeps)
          Reason = "no path to a blocking primitive";
        else if (F == Trigger)
          Reason = TrigReason;
        else // another member of a sleeping call cycle
          Reason = (Twine("in a call cycle with may-sleep function ") +
                    Trigger->getName() + "()")
                       .str();
        Results[F] = {V, Reason};
      }
    }

    // Completeness safety net: cover any defined function the SCC walk did not
    // reach (e.g. an uncalled internal function), including cycles among them.
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (const Function &F : M) {
        if (F.isDeclaration() || Results.count(&F))
          continue;
        auto R = reasonFor(&F);
        if (R.first) {
          Results[&F] = {Verdict::MaySleep, R.second};
          Changed = true;
        }
      }
    }
    for (const Function &F : M)
      if (!F.isDeclaration() && !Results.count(&F))
        Results[&F] = {Verdict::AtomicSafe, "no path to a blocking primitive"};

    // 4. Deterministic report (sorted by function name).
    std::vector<std::pair<std::string, FuncResult>> Rows;
    for (const Function &F : M)
      if (!F.isDeclaration())
        Rows.push_back({F.getName().str(), Results[&F]});
    std::sort(Rows.begin(), Rows.end(),
              [](const auto &A, const auto &B) { return A.first < B.first; });

    size_t Width = 0;
    for (const auto &R : Rows)
      Width = std::max(Width, R.first.size());

    unsigned Sleepy = 0, Safe = 0;
    outs() << "=== Sleepability analysis (C-side CanSleep summary) ===\n";
    outs() << "module: " << M.getName() << "\n\n";
    for (const auto &R : Rows) {
      bool S = R.second.verdict == Verdict::MaySleep;
      (S ? Sleepy : Safe)++;
      outs() << "  [" << (S ? "MAY SLEEP  " : "ATOMIC-SAFE") << "]  ";
      outs() << R.first;
      for (size_t i = R.first.size(); i < Width; ++i)
        outs() << ' ';
      outs() << "  -- " << R.second.reason << "\n";
    }
    outs() << "\nsummary: " << Sleepy << " may-sleep, " << Safe
           << " atomic-safe (" << Rows.size() << " functions analysed)\n";

    return PreservedAnalyses::all();
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// New-PM plugin registration.
//===----------------------------------------------------------------------===//

llvm::PassPluginLibraryInfo getSleepabilityPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "Sleepability", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "sleepability") {
                    MPM.addPass(SleepabilityPass());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getSleepabilityPluginInfo();
}
