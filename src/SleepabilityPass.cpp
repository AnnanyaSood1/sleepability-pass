//===- SleepabilityPass.cpp -----------------------------------------------===//
//
// A standalone, out-of-tree LLVM module pass (new pass manager) that computes,
// for every defined function in a module, a sleepability SUMMARY: whether the
// function can, directly or transitively, reach a blocking primitive.
//
// This is the C-side ("callee summary") half of CLSC, the Context-Lattice
// Sleepability Checker. The Rust-side typestate and the call-site join live in
// the companion repository `guardstate`, which reads the summary file this
// pass writes (`.clsc_summary`, see writeSummaryFile below).
//
// Summary lattice (per function, joined monotonically):
//
//   ATOMIC-SAFE  <=  IF-ARG(i...)  <=  MAY SLEEP
//                         |
//                     UNKNOWN (an unresolved indirect call was reached)
//
//   IF-ARG(i) means "may sleep iff argument i carries the gfp direct-reclaim
//   bit". It lets a forwarding wrapper such as
//       void *rust_helper_krealloc(..., gfp_t flags, ...) { return
//       krealloc(..., flags, ...); }
//   be decided at each call site instead of being reported as always sleeping.
//
// Pipeline:
//   1. Load the primitive table (built-in, optionally extended from a file).
//   2. Walk the call graph's strongly connected components bottom-up
//      (scc_iterator yields callees before callers); inside an SCC, iterate the
//      members' summaries to a fixpoint.
//   3. A final whole-module round-robin pass re-checks every function until
//      nothing changes (a safety net for functions the SCC walk did not reach).
//   4. Print a deterministic per-function verdict, and optionally write the
//      machine-readable summary file with a witness chain per function.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Command-line options (all optional; the defaults reproduce the test suite).
//===----------------------------------------------------------------------===//

static cl::opt<std::string> PrimitivesFile(
    "sleepability-primitives",
    cl::desc("File extending the primitive table: lines 'sleep NAME' or "
             "'gfp NAME ARGINDEX' ('#' comments allowed)"),
    cl::init(""));

static cl::opt<std::string> SummaryFile(
    "sleepability-summary",
    cl::desc("Write the machine-readable .clsc_summary file to this path"),
    cl::init(""));

static cl::opt<std::string> IndirectPolicy(
    "sleepability-indirect",
    cl::desc("Indirect calls: 'unknown' (default, reported as UNKNOWN) or "
             "'ignore' (treated as non-sleeping; unsound)"),
    cl::init("unknown"));

static cl::opt<unsigned long long>
    ReclaimMask("sleepability-reclaim-mask",
                cl::desc("The gfp bit that means 'may enter direct reclaim' "
                         "(__GFP_DIRECT_RECLAIM of the analysed kernel)"),
                cl::init(0x400));

namespace {

static const char *Version = "0.2.0";

//===----------------------------------------------------------------------===//
// Knowledge base: the primitives.
//===----------------------------------------------------------------------===//

struct Primitives {
  // Unconditionally blocking, recognised by symbol name.
  std::set<std::string> Sleep = {"mutex_lock", "schedule", "msleep",
                                 "might_sleep"};
  // Allocators: name -> index of the gfp flags argument.
  std::map<std::string, unsigned> Gfp = {{"kmalloc", 1}};

  bool load(const std::string &Path, std::string &Err) {
    auto Buf = MemoryBuffer::getFile(Path);
    if (!Buf) {
      Err = "cannot read primitives file '" + Path + "'";
      return false;
    }
    SmallVector<StringRef, 64> Lines;
    (*Buf)->getBuffer().split(Lines, '\n');
    unsigned No = 0;
    for (StringRef L : Lines) {
      ++No;
      L = L.split('#').first.trim();
      if (L.empty())
        continue;
      SmallVector<StringRef, 4> T;
      L.split(T, ' ', -1, false);
      if (T.size() == 2 && T[0] == "sleep") {
        Sleep.insert(T[1].str());
      } else if (T.size() == 3 && T[0] == "gfp") {
        unsigned Idx;
        if (T[2].getAsInteger(10, Idx)) {
          Err = Path + ":" + std::to_string(No) + ": bad argument index";
          return false;
        }
        Gfp[T[1].str()] = Idx;
      } else {
        Err = Path + ":" + std::to_string(No) +
              ": expected 'sleep NAME' or "
              "'gfp NAME ARGINDEX'";
        return false;
      }
    }
    return true;
  }
};

//===----------------------------------------------------------------------===//
// The summary of one function.
//===----------------------------------------------------------------------===//

struct Summary {
  bool May = false;            // may sleep on some path, unconditionally
  bool Unknown = false;        // reaches an unresolved indirect call
  std::set<unsigned> CondArgs; // may sleep iff one of these args has the bit
  std::string Reason;          // human-readable cause of the first fact
  std::string Via;             // next hop of the witness chain

  enum Class { AtomicSafe, IfArg, UnknownC, MaySleep };
  Class cls() const {
    if (May)
      return MaySleep;
    if (Unknown)
      return UnknownC;
    if (!CondArgs.empty())
      return IfArg;
    return AtomicSafe;
  }
  bool operator==(const Summary &O) const {
    return May == O.May && Unknown == O.Unknown && CondArgs == O.CondArgs;
  }
};

// Add a fact, keeping the reason of the strongest fact seen first.
static void addMay(Summary &S, const std::string &Why, const std::string &Via) {
  if (!S.May) {
    S.May = true;
    S.Reason = Why;
    S.Via = Via;
  }
}
static void addUnknown(Summary &S, const std::string &Why) {
  if (!S.Unknown) {
    S.Unknown = true;
    if (!S.May) {
      S.Reason = Why;
      S.Via = "(indirect)";
    }
  }
}
static void addCond(Summary &S, const std::set<unsigned> &Args,
                    const std::string &Why, const std::string &Via) {
  bool Fresh = S.CondArgs.empty();
  S.CondArgs.insert(Args.begin(), Args.end());
  if (Fresh && !S.May && !S.Unknown) {
    S.Reason = Why;
    S.Via = Via;
  }
}

//===----------------------------------------------------------------------===//
// Abstract value of a gfp flags operand, relative to the reclaim bit.
//===----------------------------------------------------------------------===//

struct Flag {
  enum Kind { No, Yes, Args, Unknown } K = Unknown;
  std::set<unsigned> A; // for Args: the bit is set iff one of these args has it
  static Flag no() {
    Flag F;
    F.K = No;
    return F;
  }
  static Flag yes() {
    Flag F;
    F.K = Yes;
    return F;
  }
  static Flag unknown() { return Flag(); }
  static Flag arg(unsigned I) {
    Flag F;
    F.K = Args;
    F.A = {I};
    return F;
  }
};

// Bitwise OR, and the join at control-flow merges: the bit may be set if it
// may be set in either operand.
static Flag orFlags(const Flag &L, const Flag &R) {
  if (L.K == Flag::Yes || R.K == Flag::Yes)
    return Flag::yes();
  if (L.K == Flag::Unknown || R.K == Flag::Unknown)
    return Flag::unknown();
  if (L.K == Flag::No)
    return R;
  if (R.K == Flag::No)
    return L;
  Flag F = L;
  F.A.insert(R.A.begin(), R.A.end());
  return F;
}

class FlagEval {
public:
  explicit FlagEval(uint64_t Mask) : Mask(Mask) {}

  // What do we know about the reclaim bit of V, a value inside F?
  Flag eval(const Value *V) {
    SmallPtrSet<const Value *, 16> Seen;
    return go(V, Seen);
  }

private:
  uint64_t Mask;

  Flag go(const Value *V, SmallPtrSet<const Value *, 16> &Seen) {
    if (!Seen.insert(V).second)
      return Flag::no(); // a cycle through phis adds no new bits
    if (const auto *CI = dyn_cast<ConstantInt>(V))
      return (CI->getZExtValue() & Mask) ? Flag::yes() : Flag::no();
    if (const auto *A = dyn_cast<Argument>(V))
      return Flag::arg(A->getArgNo());
    if (const auto *C = dyn_cast<CastInst>(V)) // zext / trunc / bitcast
      return go(C->getOperand(0), Seen);
    if (const auto *BO = dyn_cast<BinaryOperator>(V)) {
      if (BO->getOpcode() == Instruction::Or)
        return orFlags(go(BO->getOperand(0), Seen),
                       go(BO->getOperand(1), Seen));
      if (BO->getOpcode() == Instruction::And) {
        // x & C with C lacking the bit clears it, whatever x is.
        for (const Value *Op : {BO->getOperand(0), BO->getOperand(1)})
          if (const auto *CI = dyn_cast<ConstantInt>(Op))
            if (!(CI->getZExtValue() & Mask))
              return Flag::no();
        Flag L = go(BO->getOperand(0), Seen), R = go(BO->getOperand(1), Seen);
        if (L.K == Flag::No || R.K == Flag::No)
          return Flag::no();
        if (isa<ConstantInt>(BO->getOperand(1)))
          return L;
        if (isa<ConstantInt>(BO->getOperand(0)))
          return R;
        return Flag::unknown();
      }
      return Flag::unknown();
    }
    if (const auto *P = dyn_cast<PHINode>(V)) {
      Flag F = Flag::no();
      for (const Value *In : P->incoming_values())
        F = orFlags(F, go(In, Seen));
      return F;
    }
    if (const auto *S = dyn_cast<SelectInst>(V))
      return orFlags(go(S->getTrueValue(), Seen), go(S->getFalseValue(), Seen));
    if (const auto *L = dyn_cast<LoadInst>(V))
      return fromLocalSlot(L, Seen);
    return Flag::unknown();
  }

  // At -O0, clang spills each parameter to an alloca and reloads it. Follow a
  // load back to the slot's single store when the slot does not escape.
  Flag fromLocalSlot(const LoadInst *L, SmallPtrSet<const Value *, 16> &Seen) {
    const auto *Slot = dyn_cast<AllocaInst>(L->getPointerOperand());
    if (!Slot)
      return Flag::unknown();
    const StoreInst *Only = nullptr;
    for (const User *U : Slot->users()) {
      if (isa<LoadInst>(U))
        continue;
      const auto *St = dyn_cast<StoreInst>(U);
      if (!St || St->getPointerOperand() != Slot || Only)
        return Flag::unknown(); // escapes, or stored more than once
      Only = St;
    }
    if (!Only)
      return Flag::unknown();
    return go(Only->getValueOperand(), Seen);
  }
};

//===----------------------------------------------------------------------===//
// The analysis.
//===----------------------------------------------------------------------===//

class Analysis {
public:
  Analysis(Module &M, const Primitives &P, bool IndirectUnknown)
      : M(M), P(P), IndirectUnknown(IndirectUnknown), FE(ReclaimMask) {}

  std::map<const Function *, Summary> Results;
  std::set<std::string> UnmodelledExternals;
  unsigned InlineAsmCalls = 0;

  void run() {
    CallGraph CG(M);
    // Bottom-up over SCCs; iterate inside each SCC to a fixpoint.
    for (scc_iterator<CallGraph *> It = scc_begin(&CG); !It.isAtEnd(); ++It) {
      std::vector<const Function *> Funcs;
      for (CallGraphNode *N : *It)
        if (const Function *F = N->getFunction())
          if (!F->isDeclaration())
            Funcs.push_back(F);
      stabilise(Funcs);
    }
    // Safety net: anything the walk did not reach, and a final global check.
    std::vector<const Function *> All;
    for (const Function &F : M)
      if (!F.isDeclaration())
        All.push_back(&F);
    stabilise(All);
  }

  // Witness chain: follow Via pointers from F to the primitive.
  std::string witness(const Function *F) const {
    std::string W = F->getName().str();
    std::set<const Function *> Seen = {F};
    const Function *Cur = F;
    for (int Hop = 0; Hop < 64; ++Hop) {
      auto It = Results.find(Cur);
      if (It == Results.end() || It->second.Via.empty())
        break;
      W += ">" + It->second.Via;
      const Function *Next = M.getFunction(It->second.Via);
      if (!Next || Next->isDeclaration() || !Seen.insert(Next).second)
        break;
      Cur = Next;
    }
    return W;
  }

private:
  Module &M;
  const Primitives &P;
  bool IndirectUnknown;
  FlagEval FE;

  void stabilise(const std::vector<const Function *> &Funcs) {
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (const Function *F : Funcs) {
        Summary S = summarise(*F);
        auto It = Results.find(F);
        if (It == Results.end() || !(It->second == S)) {
          Results[F] = S;
          Changed = true;
        }
      }
    }
  }

  // Apply a callee's effect that depends on its argument Idx at call CB.
  void applyArgCondition(Summary &S, const CallBase &CB, unsigned Idx,
                         const std::string &CalleeName) {
    if (Idx >= CB.arg_size()) {
      addMay(S,
             "calls " + CalleeName + "() with too few arguments (conservative)",
             CalleeName);
      return;
    }
    Flag Fl = FE.eval(CB.getArgOperand(Idx));
    switch (Fl.K) {
    case Flag::No:
      return; // provably no reclaim bit: atomic-safe here
    case Flag::Yes:
      addMay(S,
             "calls " + CalleeName +
                 "() with direct-reclaim gfp flags (may sleep)",
             CalleeName);
      return;
    case Flag::Args: {
      std::string Lst;
      for (unsigned A : Fl.A)
        Lst += (Lst.empty() ? "" : ",") + std::to_string(A);
      addCond(S, Fl.A,
              "may sleep iff arg " + Lst + " has the reclaim bit (passed to " +
                  CalleeName + "())",
              CalleeName);
      return;
    }
    case Flag::Unknown:
      addMay(S,
             "calls " + CalleeName +
                 "() with non-constant gfp flags "
                 "(conservatively may sleep)",
             CalleeName);
      return;
    }
  }

  Summary summarise(const Function &F) {
    Summary S;
    for (const Instruction &I : instructions(F)) {
      const auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      if (CB->isInlineAsm()) {
        ++InlineAsmCalls; // assumption: inline asm does not call the scheduler
        continue;
      }
      const Function *Callee = CB->getCalledFunction();
      if (!Callee) {
        if (IndirectUnknown)
          addUnknown(S,
                     "indirect call through a function pointer (unresolved)");
        continue;
      }
      std::string Name = Callee->getName().str();
      if (Callee->isIntrinsic())
        continue;

      if (P.Sleep.count(Name)) {
        addMay(S, "calls blocking primitive '" + Name + "()'", Name);
        continue;
      }
      auto G = P.Gfp.find(Name);
      if (G != P.Gfp.end()) {
        applyArgCondition(S, *CB, G->second, Name);
        continue;
      }
      if (Callee->isDeclaration()) {
        UnmodelledExternals.insert(Name);
        continue; // not a known primitive: assumed non-sleeping (see DESIGN.md)
      }
      auto It = Results.find(Callee);
      if (It == Results.end())
        continue; // same SCC, not yet summarised: the SCC fixpoint revisits
      const Summary &C = It->second;
      if (C.May)
        addMay(S, "reaches may-sleep callee " + Name + "()", Name);
      if (C.Unknown)
        addUnknown(S, "reaches callee " + Name +
                          "() with an unresolved indirect call");
      for (unsigned A : C.CondArgs)
        applyArgCondition(S, *CB, A, Name);
    }
    if (S.Reason.empty())
      S.Reason = "no path to a blocking primitive";
    return S;
  }
};

//===----------------------------------------------------------------------===//
// Output.
//===----------------------------------------------------------------------===//

static const char *label(Summary::Class C) {
  switch (C) {
  case Summary::MaySleep:
    return "MAY SLEEP  ";
  case Summary::UnknownC:
    return "UNKNOWN    ";
  case Summary::IfArg:
    return "IF-FLAGS   ";
  case Summary::AtomicSafe:
    return "ATOMIC-SAFE";
  }
  return "?";
}

static const char *fileTag(Summary::Class C) {
  switch (C) {
  case Summary::MaySleep:
    return "may_sleep";
  case Summary::UnknownC:
    return "unknown";
  case Summary::IfArg:
    return "if_arg";
  case Summary::AtomicSafe:
    return "atomic_safe";
  }
  return "?";
}

// SHA-256 of the module's IR text, without the ModuleID comment line, so the
// key depends on the IR and not on the file name.
static std::string irHash(const Module &M) {
  std::string Text;
  raw_string_ostream OS(Text);
  M.print(OS, nullptr);
  OS.flush();
  StringRef Body(Text);
  if (Body.starts_with("; ModuleID"))
    Body = Body.split('\n').second;
  SHA256 H;
  H.update(Body);
  auto Digest = H.final();
  return toHex(ArrayRef<uint8_t>(Digest.data(), Digest.size()), true);
}

// Everything besides the IR that changes the result: the pass version, the
// options, and the primitive table (built-in entries and the file's contents).
static std::string configKey(const Primitives &P) {
  std::string K = std::string("version=") + Version +
                  " mask=" + utohexstr(ReclaimMask, true) +
                  " indirect=" + IndirectPolicy.getValue();
  for (const std::string &S : P.Sleep)
    K += " sleep:" + S;
  for (const auto &G : P.Gfp)
    K += " gfp:" + G.first + "/" + std::to_string(G.second);
  SHA256 H;
  H.update(K);
  auto Digest = H.final();
  return toHex(ArrayRef<uint8_t>(Digest.data(), Digest.size()), true);
}

// If a summary already exists for this exact IR and configuration, keep it.
static bool summaryUpToDate(const std::string &Path, const std::string &Hash,
                            const std::string &Config) {
  auto Buf = MemoryBuffer::getFile(Path);
  if (!Buf)
    return false;
  StringRef B = (*Buf)->getBuffer();
  return B.contains("# ir-sha256: " + Hash + "\n") &&
         B.contains("# config-sha256: " + Config + "\n");
}

struct SleepabilityPass : PassInfoMixin<SleepabilityPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    Primitives P;
    if (!PrimitivesFile.empty()) {
      std::string Err;
      if (!P.load(PrimitivesFile, Err)) {
        errs() << "sleepability: error: " << Err << "\n";
        return PreservedAnalyses::all();
      }
    }
    if (IndirectPolicy != "unknown" && IndirectPolicy != "ignore") {
      errs() << "sleepability: error: -sleepability-indirect must be "
                "'unknown' or 'ignore'\n";
      return PreservedAnalyses::all();
    }

    std::string Hash, Config;
    if (!SummaryFile.empty()) {
      Hash = irHash(M);
      Config = configKey(P);
      if (summaryUpToDate(SummaryFile, Hash, Config)) {
        outs() << "summary up to date for ir-sha256 " << Hash.substr(0, 12)
               << "..., not recomputed: " << SummaryFile << "\n";
        return PreservedAnalyses::all();
      }
    }

    Analysis A(M, P, IndirectPolicy == "unknown");
    A.run();

    // Deterministic report, sorted by name.
    std::vector<std::pair<std::string, const Function *>> Rows;
    for (const Function &F : M)
      if (!F.isDeclaration())
        Rows.push_back({F.getName().str(), &F});
    std::sort(Rows.begin(), Rows.end());

    size_t Width = 0;
    for (const auto &R : Rows)
      Width = std::max(Width, R.first.size());

    unsigned Count[4] = {0, 0, 0, 0};
    outs() << "=== Sleepability analysis (C-side CanSleep summary) ===\n";
    outs() << "module: " << M.getName() << "\n\n";
    for (const auto &R : Rows) {
      const Summary &S = A.Results[R.second];
      Summary::Class C = S.cls();
      ++Count[C];
      outs() << "  [" << label(C) << "]  " << R.first;
      for (size_t i = R.first.size(); i < Width; ++i)
        outs() << ' ';
      outs() << "  -- " << S.Reason << "\n";
    }
    outs() << "\nsummary: " << Count[Summary::MaySleep] << " may-sleep, "
           << Count[Summary::IfArg] << " conditional, "
           << Count[Summary::UnknownC] << " unknown, "
           << Count[Summary::AtomicSafe] << " atomic-safe (" << Rows.size()
           << " functions analysed)\n";

    if (!SummaryFile.empty()) {
      std::error_code EC;
      raw_fd_ostream Out(SummaryFile, EC, sys::fs::OF_Text);
      if (EC) {
        errs() << "sleepability: error: cannot write " << SummaryFile << ": "
               << EC.message() << "\n";
        return PreservedAnalyses::all();
      }
      Out << "# clsc-summary 1\n";
      Out << "# producer: sleepability-pass " << Version << " (LLVM "
          << LLVM_VERSION_STRING << ")\n";
      Out << "# module: " << M.getName() << "\n";
      Out << "# ir-sha256: " << Hash << "\n";
      Out << "# config-sha256: " << Config << "\n";
      Out << "# reclaim-mask: 0x" << utohexstr(ReclaimMask, true) << "\n";
      Out << "# indirect-calls: " << IndirectPolicy << "\n";
      for (const auto &R : Rows) {
        const Summary &S = A.Results[R.second];
        Summary::Class C = S.cls();
        Out << fileTag(C) << " " << R.first;
        if (!S.CondArgs.empty()) {
          std::string Lst;
          for (unsigned I : S.CondArgs)
            Lst += (Lst.empty() ? "" : ",") + std::to_string(I);
          Out << " arg=" << Lst << " mask=0x" << utohexstr(ReclaimMask, true);
        }
        if (C != Summary::AtomicSafe)
          Out << " via=" << A.witness(R.second);
        Out << "\n";
      }
      for (const std::string &E : A.UnmodelledExternals)
        Out << "# unmodelled-external: " << E << "\n";
      if (A.InlineAsmCalls)
        Out << "# inline-asm-calls: " << A.InlineAsmCalls << "\n";
      outs() << "wrote summary: " << SummaryFile << "\n";
    }
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
