#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"

#ifdef FUZZX_HAVE_SPIRV_TOOLS
#include "spirv-tools/libspirv.h"
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>

using namespace llvm;

namespace {

constexpr StringRef DefaultTriple = "spirv64-unknown-unknown";

const Target *getSPIRVTarget() {
  static const Target *T = [] {
    LLVMInitializeSPIRVTargetInfo();
    LLVMInitializeSPIRVTarget();
    LLVMInitializeSPIRVTargetMC();
    LLVMInitializeSPIRVAsmPrinter();

    std::string Error;
    Triple TT(DefaultTriple);
    const Target *Target = TargetRegistry::lookupTarget(TT, Error);
    if (!Target)
      std::abort();
    return Target;
  }();
  return T;
}

CodeGenOptLevel codeGenOptLevel(OptimizationLevel Level) {
  if (Level == OptimizationLevel::O0)
    return CodeGenOptLevel::None;
  if (Level == OptimizationLevel::O1)
    return CodeGenOptLevel::Less;
  if (Level == OptimizationLevel::O2)
    return CodeGenOptLevel::Default;
  return CodeGenOptLevel::Aggressive;
}

std::unique_ptr<TargetMachine> createTargetMachine(StringRef CPU,
                                                   OptimizationLevel Level) {
  Triple TT(DefaultTriple);
  TargetOptions Options;
  std::unique_ptr<TargetMachine> TM(getSPIRVTarget()->createTargetMachine(
      TT, CPU, "", Options, std::nullopt, std::nullopt,
      codeGenOptLevel(Level)));
  if (!TM)
    std::abort();
  return TM;
}

bool runOptimizationPipeline(Module &M, TargetMachine &TM,
                             OptimizationLevel Level) {
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  PassBuilder PB(&TM);
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager MPM =
      Level == OptimizationLevel::O0 ? PB.buildO0DefaultPipeline(Level)
                                     : PB.buildPerModuleDefaultPipeline(Level);
  MPM.run(M, MAM);
  return !verifyModule(M, &errs());
}

std::optional<SmallVector<char, 0>> emitObject(Module &M, TargetMachine &TM) {
  SmallVector<char, 0> Obj;
  raw_svector_ostream OS(Obj);
  legacy::PassManager PM;
  if (TM.addPassesToEmitFile(PM, OS, nullptr, CodeGenFileType::ObjectFile))
    return std::nullopt;
  PM.run(M);
  return Obj;
}

struct CompileResult {
  SmallVector<char, 0> Object;
  std::string FailureStage;
  int CrashRetCode = 0;
  bool Success = false;
  bool Crashed = false;
};

std::string moduleToString(Module &M) {
  std::string Text;
  raw_string_ostream OS(Text);
  M.print(OS, nullptr);
  return Text;
}

StringRef getCPU() {
  const char *Env = std::getenv("FUZZX_SPIRV_CPU");
  if (Env && *Env)
    return Env;
  return "";
}

bool envFlag(const char *Name, bool Default) {
  const char *Value = std::getenv(Name);
  if (!Value || !*Value)
    return Default;
  StringRef V(Value);
  return V != "0" && !V.equals_insensitive("false") &&
         !V.equals_insensitive("no") && !V.equals_insensitive("off");
}

// Accepted by both spvParseTargetEnv and the spirv-val binary's --target-env.
constexpr StringRef DefaultSPIRVTargetEnv = "spv1.6";

const std::string &getSPIRVTargetEnvName() {
  static const std::string Cached = [] {
    const char *Env = std::getenv("FUZZX_SPIRV_TARGET_ENV");
    return std::string((Env && *Env) ? Env : DefaultSPIRVTargetEnv);
  }();
  return Cached;
}

struct ValidationResult {
  int Code = 0;
  std::string Message;
  bool Crashed = false;
  bool Valid() const { return !Crashed && Code == 0; }
};

#ifndef FUZZX_HAVE_SPIRV_TOOLS
const char *getSPIRVValBinary() {
  static const char *Cached = std::getenv("FUZZX_SPIRV_VAL_BIN");
  return (Cached && *Cached) ? Cached : nullptr;
}

ValidationResult runSPIRVValidator(ArrayRef<char> Object) {
  ValidationResult R;
  const char *Bin = getSPIRVValBinary();
  if (!Bin) {
    R.Code = -1;
    R.Message = "FUZZX_SPIRV_VAL_BIN not set";
    return R;
  }
  SmallString<128> Path;
  if (auto EC = sys::fs::createTemporaryFile("fuzzx-spirv", "spv", Path)) {
    R.Code = -1;
    R.Message = "createTemporaryFile: " + EC.message();
    return R;
  }
  {
    std::error_code EC;
    raw_fd_ostream Out(Path, EC, sys::fs::OF_None);
    if (EC) {
      sys::fs::remove(Path);
      R.Code = -1;
      R.Message = "open temp .spv: " + EC.message();
      return R;
    }
    Out.write(Object.data(), Object.size());
  }
  const std::string &EnvName = getSPIRVTargetEnvName();
  StringRef Args[] = {Bin, "--target-env", EnvName, Path};
  std::optional<StringRef> Redirects[] = {std::nullopt, StringRef(""),
                                          std::nullopt};
  std::string ErrMsg;
  bool ExecFailed = false;
  int RC = sys::ExecuteAndWait(Bin, Args, /*Env=*/std::nullopt, Redirects,
                               /*SecondsToWait=*/0, /*MemoryLimit=*/0,
                               &ErrMsg, &ExecFailed);
  sys::fs::remove(Path);
  if (ExecFailed || RC == -1) {
    R.Code = -1;
    R.Message = "spawn failed: " + ErrMsg;
  } else if (RC == -2) {
    R.Crashed = true;
    R.Code = RC;
    R.Message = "crashed: " + ErrMsg;
  } else {
    R.Code = RC;
    if (RC != 0)
      R.Message = "spirv-val exit " + std::to_string(RC);
  }
  return R;
}
#else
spv_target_env getSPIRVTargetEnv() {
  static const spv_target_env Cached = [] {
    spv_target_env Parsed;
    if (spvParseTargetEnv(getSPIRVTargetEnvName().c_str(), &Parsed))
      return Parsed;
    return SPV_ENV_UNIVERSAL_1_6;
  }();
  return Cached;
}

spv_context getSPIRVContext() {
  // SPIRV-Tools allows re-using a single spv_context across serial
  // spvValidateBinary calls; per-call state lives in ValidationState_t.
  static spv_context Cached = spvContextCreate(getSPIRVTargetEnv());
  return Cached;
}

ValidationResult runSPIRVValidator(ArrayRef<char> Object) {
  ValidationResult R;
  if (Object.size() % sizeof(uint32_t) != 0) {
    R.Code = SPV_ERROR_INVALID_BINARY;
    R.Message = "object size not a multiple of 4";
    return R;
  }
  const uint32_t *Words = reinterpret_cast<const uint32_t *>(Object.data());
  size_t NumWords = Object.size() / sizeof(uint32_t);

  CrashRecoveryContext CRC;
  CRC.DumpStackAndCleanupOnFailure = true;
  bool Ok = CRC.RunSafely([&] {
    spv_context Ctx = getSPIRVContext();
    spv_diagnostic Diag = nullptr;
    R.Code = spvValidateBinary(Ctx, Words, NumWords, &Diag);
    if (Diag) {
      if (Diag->error)
        R.Message = Diag->error;
      spvDiagnosticDestroy(Diag);
    }
  });
  if (!Ok) {
    R.Crashed = true;
    R.Code = CRC.RetCode;
  }
  return R;
}
#endif

// ---- Miscompile round-trip oracle ------------------------------------------
//
// SPIR-V has no IL-capable OpenCL/Vulkan runtime on this host, so we cannot
// launch the codegen output on a device.  Instead we run a semantic
// differential entirely inside LLVM tooling: reverse-translate each O0/O2
// SPIR-V object back to LLVM IR with the Khronos `llvm-spirv -r`, attach a
// fixed-ABI driver, and interpret it with `lli --force-interpreter`.  The
// process exit code is the execution signature.  If the O0 and O2 round-trips
// disagree -- or either disagrees with the optimized IR interpreted directly
// (the trusted reference) -- the SPIR-V backend miscompiled the kernel.
//
// The oracle only fires for modules that expose a `fuzz_kernel` with the
// skeleton ABI `(ptr addrspace(1), ptr addrspace(1), i32)`; this keeps
// mismatches meaningful, mirroring the fixed-ABI invariant the FuzzX PTX and
// AMDGPU differential fuzzers rely on.  Anything we cannot turn into a
// signature (missing binaries, non-conforming kernel, reverse-translate or
// interpreter failure) is skipped, never reported -- only the SPIR-V backend
// is the system under test.

constexpr int kSigSkip = -1;
constexpr unsigned kDriverArrayLen = 64;
constexpr unsigned kInterpretTimeoutSecs = 10;
constexpr unsigned kTranslateTimeoutSecs = 10;

const char *getEnvBin(const char *Name) {
  const char *V = std::getenv(Name);
  return (V && *V) ? V : nullptr;
}

bool kernelMatchesABI(const Function &F) {
  if (F.isDeclaration() || F.getCallingConv() != CallingConv::SPIR_KERNEL)
    return false;
  if (!F.getReturnType()->isVoidTy() || F.arg_size() != 3)
    return false;
  auto IsGlobalPtr = [](Type *T) {
    auto *P = dyn_cast<PointerType>(T);
    return P && P->getAddressSpace() == 1;
  };
  return IsGlobalPtr(F.getArg(0)->getType()) &&
         IsGlobalPtr(F.getArg(1)->getType()) &&
         F.getArg(2)->getType()->isIntegerTy(32);
}

// Adds @__fuzzx_in / @__fuzzx_out globals and a main() that calls Kernel over a
// few trip counts and folds the output buffer into a one-byte signature
// returned as the exit status.  Demotes Kernel to a callable convention so the
// interpreter will execute the call (SPIR_KERNEL entry points are not
// callable).
void buildExecDriver(Module &M, Function &Kernel) {
  LLVMContext &Ctx = M.getContext();
  Type *I32 = Type::getInt32Ty(Ctx);
  ArrayType *ArrTy = ArrayType::get(I32, kDriverArrayLen);

  // The bundled `lli` interpreter asserts when it materializes a negative i32
  // into memory (global initializer or store), so keep the seed inputs within
  // the non-negative i32 range; otherwise every run aborts at startup and the
  // oracle would silently abstain on all inputs.
  SmallVector<Constant *, kDriverArrayLen> InVals;
  for (unsigned I = 0; I < kDriverArrayLen; ++I)
    InVals.push_back(ConstantInt::get(I32, (0x9E3779B1u * (I + 1)) & 0x7fffffffu));
  auto *In = new GlobalVariable(M, ArrTy, /*isConstant=*/false,
                                GlobalValue::InternalLinkage,
                                ConstantArray::get(ArrTy, InVals), "__fuzzx_in",
                                nullptr, GlobalValue::NotThreadLocal, 1);
  auto *Out = new GlobalVariable(M, ArrTy, /*isConstant=*/false,
                                 GlobalValue::InternalLinkage,
                                 ConstantAggregateZero::get(ArrTy),
                                 "__fuzzx_out", nullptr,
                                 GlobalValue::NotThreadLocal, 1);

  Kernel.setCallingConv(CallingConv::SPIR_FUNC);

  Function *Main = Function::Create(FunctionType::get(I32, false),
                                    GlobalValue::ExternalLinkage, "main", M);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", Main));
  Value *Zero = B.getInt32(0);
  Value *InP = B.CreateInBoundsGEP(ArrTy, In, {Zero, Zero});
  Value *OutP = B.CreateInBoundsGEP(ArrTy, Out, {Zero, Zero});

  // Fold the output buffer into an 8-bit signature returned as the process
  // exit status.  The fold is deliberately mul-free and masked to a byte each
  // step: the bundled `lli` interpreter asserts on negative i32 operands to
  // `mul`, and a wider running hash washes entropy out of the low byte the
  // exit code carries.  A per-element position/trip weight keeps the fold
  // order-sensitive so distinct outputs map to distinct signatures.
  Value *Mask = B.getInt32(0xff);
  Value *Acc = B.getInt32(0);
  for (int N : {1, 3, 7, 17}) {
    CallInst *CI = B.CreateCall(Kernel.getFunctionType(), &Kernel,
                                {InP, OutP, B.getInt32(N)});
    CI->setCallingConv(CallingConv::SPIR_FUNC);
    for (unsigned J = 0; J < kDriverArrayLen; ++J) {
      Value *EP = B.CreateInBoundsGEP(ArrTy, Out, {Zero, B.getInt32(J)});
      Value *V = B.CreateAnd(B.CreateLoad(I32, EP), Mask);
      Acc = B.CreateAdd(Acc, V);
      Acc = B.CreateAdd(Acc, B.getInt32((J + 1) * static_cast<unsigned>(N)));
      Acc = B.CreateAnd(Acc, Mask);
    }
  }
  B.CreateRet(Acc);
}

// Serializes M to a temp .bc, interprets it, returns the exit status (0-255) or
// kSigSkip when the interpreter is unavailable, fails to spawn, or the
// interpreted program crashes/times out (none of which implicate the backend).
int interpretModuleSignature(Module &M) {
  const char *Lli = getEnvBin("FUZZX_LLI_BIN");
  if (!Lli)
    return kSigSkip;
  SmallString<128> BCPath;
  if (sys::fs::createTemporaryFile("fuzzx-exec", "bc", BCPath))
    return kSigSkip;
  {
    std::error_code EC;
    raw_fd_ostream OS(BCPath, EC, sys::fs::OF_None);
    if (EC) {
      sys::fs::remove(BCPath);
      return kSigSkip;
    }
    WriteBitcodeToFile(M, OS);
  }
  StringRef Args[] = {Lli, "--force-interpreter", BCPath};
  std::optional<StringRef> Redirects[] = {std::nullopt, StringRef(""),
                                          StringRef("")};
  std::string ErrMsg;
  bool ExecFailed = false;
  int RC = sys::ExecuteAndWait(Lli, Args, /*Env=*/std::nullopt, Redirects,
                               kInterpretTimeoutSecs, /*MemoryLimit=*/0, &ErrMsg,
                               &ExecFailed);
  sys::fs::remove(BCPath);
  if (ExecFailed || RC < 0)
    return kSigSkip;
  return RC & 0xff;
}

// Runs `llvm-spirv -r` on a SPIR-V object and parses the result into a module.
std::unique_ptr<Module> reverseTranslate(ArrayRef<char> Object,
                                         LLVMContext &Ctx) {
  const char *Tr = getEnvBin("FUZZX_SPIRV_TRANSLATOR_BIN");
  if (!Tr)
    return nullptr;
  SmallString<128> SpvPath, BcPath;
  if (sys::fs::createTemporaryFile("fuzzx-rt", "spv", SpvPath))
    return nullptr;
  {
    std::error_code EC;
    raw_fd_ostream OS(SpvPath, EC, sys::fs::OF_None);
    if (EC) {
      sys::fs::remove(SpvPath);
      return nullptr;
    }
    OS.write(Object.data(), Object.size());
  }
  if (sys::fs::createTemporaryFile("fuzzx-rt", "bc", BcPath)) {
    sys::fs::remove(SpvPath);
    return nullptr;
  }
  StringRef Args[] = {Tr, "-r", SpvPath, "-o", BcPath};
  std::optional<StringRef> Redirects[] = {std::nullopt, StringRef(""),
                                          StringRef("")};
  std::string ErrMsg;
  bool ExecFailed = false;
  int RC = sys::ExecuteAndWait(Tr, Args, /*Env=*/std::nullopt, Redirects,
                               kTranslateTimeoutSecs, /*MemoryLimit=*/0, &ErrMsg,
                               &ExecFailed);
  sys::fs::remove(SpvPath);
  std::unique_ptr<Module> Out;
  if (!ExecFailed && RC == 0) {
    if (auto Buf = MemoryBuffer::getFile(BcPath)) {
      Expected<std::unique_ptr<Module>> P =
          parseBitcodeFile((*Buf)->getMemBufferRef(), Ctx);
      if (P)
        Out = std::move(*P);
      else
        consumeError(P.takeError());
    }
  }
  sys::fs::remove(BcPath);
  return Out;
}

// Forward-translates LLVM IR text to a SPIR-V object with the Khronos
// `llvm-spirv` (no -r).  This is a second, independent implementation of the
// IR -> SPIR-V contract; comparing its execution against our backend's
// isolates miscompiles to one codegen path.  Returns nullopt when the
// translator is unconfigured or fails (neither implicates our backend).
std::optional<SmallVector<char, 0>> forwardTranslate(StringRef IRText) {
  const char *Tr = getEnvBin("FUZZX_SPIRV_TRANSLATOR_BIN");
  if (!Tr || IRText.empty())
    return std::nullopt;
  SmallString<128> LlPath, SpvPath;
  if (sys::fs::createTemporaryFile("fuzzx-ft", "ll", LlPath))
    return std::nullopt;
  {
    std::error_code EC;
    raw_fd_ostream OS(LlPath, EC, sys::fs::OF_None);
    if (EC) {
      sys::fs::remove(LlPath);
      return std::nullopt;
    }
    OS << IRText;
  }
  if (sys::fs::createTemporaryFile("fuzzx-ft", "spv", SpvPath)) {
    sys::fs::remove(LlPath);
    return std::nullopt;
  }
  StringRef Args[] = {Tr, LlPath, "-o", SpvPath};
  std::optional<StringRef> Redirects[] = {std::nullopt, StringRef(""),
                                          StringRef("")};
  std::string ErrMsg;
  bool ExecFailed = false;
  int RC = sys::ExecuteAndWait(Tr, Args, /*Env=*/std::nullopt, Redirects,
                               kTranslateTimeoutSecs, /*MemoryLimit=*/0, &ErrMsg,
                               &ExecFailed);
  sys::fs::remove(LlPath);
  std::optional<SmallVector<char, 0>> Out;
  if (!ExecFailed && RC == 0) {
    if (auto Buf = MemoryBuffer::getFile(SpvPath)) {
      StringRef Data = (*Buf)->getBuffer();
      Out.emplace(Data.begin(), Data.end());
    }
  }
  sys::fs::remove(SpvPath);
  return Out;
}

int signatureFromModule(std::unique_ptr<Module> M) {
  if (!M)
    return kSigSkip;
  Function *K = M->getFunction("fuzz_kernel");
  if (!K || !kernelMatchesABI(*K))
    return kSigSkip;
  buildExecDriver(*M, *K);
  if (verifyModule(*M))
    return kSigSkip;
  return interpretModuleSignature(*M);
}

int signatureFromSpirv(ArrayRef<char> Object) {
  LLVMContext Ctx;
  return signatureFromModule(reverseTranslate(Object, Ctx));
}

// Khronos leg: forward-translate the optimized IR to SPIR-V, then round-trip it
// back through the same reverse + interpret path our backend's output uses.
int signatureFromKhronos(StringRef IRText) {
  std::optional<SmallVector<char, 0>> Spv = forwardTranslate(IRText);
  if (!Spv)
    return kSigSkip;
  return signatureFromSpirv(*Spv);
}

// Reference leg: interpret the optimized IR captured before codegen.  This IR
// never passed through the SPIR-V backend, so it is the trusted answer the
// round-tripped O0/O2 signatures are checked against.
int signatureFromIRText(StringRef IRText) {
  if (IRText.empty())
    return kSigSkip;
  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseAssemblyString(IRText, Err, Ctx);
  return signatureFromModule(std::move(M));
}

void saveFailureFinding(const uint8_t *Data, size_t Size, StringRef IRText,
                        StringRef Kind, StringRef Stage,
                        std::optional<int> CrashRetCode = std::nullopt) {
  const char *FindingsDir = std::getenv("FUZZX_FINDINGS_DIR");
  if (!FindingsDir || !*FindingsDir)
    return;
  std::error_code EC;
  std::filesystem::create_directories(FindingsDir, EC);
  auto Now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::string Base = std::string(FindingsDir) + "/" + Kind.str() + "-" +
                     Stage.str() + "-" + std::to_string(getpid()) + "-" +
                     std::to_string(Now);
  if (CrashRetCode)
    Base += "-rc" + std::to_string(*CrashRetCode);
  std::ofstream BC(Base + ".bc", std::ios::binary);
  if (BC)
    BC.write(reinterpret_cast<const char *>(Data),
             static_cast<std::streamsize>(Size));
  if (!IRText.empty()) {
    std::ofstream LL(Base + ".ll");
    if (LL)
      LL.write(IRText.data(), static_cast<std::streamsize>(IRText.size()));
  }
}

void runValidatorStage(const uint8_t *Data, size_t Size, StringRef IRText,
                       ArrayRef<char> Object, StringRef Tag) {
  ValidationResult VR = runSPIRVValidator(Object);
  if (VR.Valid())
    return;
  saveFailureFinding(Data, Size, IRText,
                     VR.Crashed ? "validator-crash" : "validator-reject",
                     (Tag + "-spirv-val").str(),
                     std::optional<int>(VR.Code));
  errs() << "FuzzX SPIR-V " << Tag << " validator "
         << (VR.Crashed ? "crash" : "reject") << " (code " << VR.Code
         << "): " << VR.Message << "\n";
  std::abort();
}

// Compares the round-tripped O0/O2 execution signatures against each other and
// against the reference (optimized IR interpreted without going through the
// backend).  A confirmed disagreement is a backend miscompile.  Any leg that
// yields kSigSkip (binaries missing, kernel ABI mismatch, reverse-translate or
// interpreter failure) abstains -- the oracle never reports on those.
void runMiscompileStage(const uint8_t *Data, size_t Size,
                        ArrayRef<char> O0Object, StringRef OptimizedIR,
                        int SigO2) {
  static const bool Debug = envFlag("FUZZX_DEBUG_ORACLE", false);
  int SigRef = signatureFromIRText(OptimizedIR);
  if (SigRef == kSigSkip) {
    if (Debug)
      errs() << "RT skip: ref=kSigSkip\n";
    return;
  }
  int SigO0 = signatureFromSpirv(O0Object);
  if (SigO0 == kSigSkip || SigO2 == kSigSkip) {
    if (Debug)
      errs() << "RT skip: o0=" << SigO0 << " o2=" << SigO2 << " (ref=" << SigRef
             << ")\n";
    return;
  }
  if (Debug)
    errs() << "RT COMPARE: ref=" << SigRef << " o0=" << SigO0 << " o2=" << SigO2
           << "\n";
  if (SigO0 == SigRef && SigO2 == SigRef)
    return;

  StringRef Bad = SigO0 != SigRef ? "o0" : "o2";
  saveFailureFinding(Data, Size, OptimizedIR, "miscompile",
                     (Bad + "-roundtrip").str());
  errs() << "FuzzX SPIR-V miscompile: ref=" << SigRef << " o0=" << SigO0
         << " o2=" << SigO2 << "\n";
  std::abort();
}

// Cross-translator differential.  Compiles the same optimized IR to SPIR-V two
// independent ways -- our in-tree LLVM SPIRVCodeGen (the O2 object) and the
// Khronos `llvm-spirv` translator -- and compares their execution signatures.
// When they disagree, the optimized IR interpreted directly (never through any
// SPIR-V backend) is the tiebreaker that names the culprit.  As with the
// round-trip oracle, any leg that cannot produce a signature abstains.
void runCrossTranslatorStage(const uint8_t *Data, size_t Size,
                             StringRef OptimizedIR, int SigOurs) {
  static const bool Debug = envFlag("FUZZX_DEBUG_ORACLE", false);
  if (SigOurs == kSigSkip) {
    if (Debug)
      errs() << "CROSS skip: ours=kSigSkip\n";
    return;
  }
  int SigKhronos = signatureFromKhronos(OptimizedIR);
  if (SigKhronos == kSigSkip) {
    if (Debug)
      errs() << "CROSS skip: khronos=kSigSkip (ours=" << SigOurs << ")\n";
    return;
  }
  if (Debug)
    errs() << "CROSS COMPARE: ours=" << SigOurs << " khronos=" << SigKhronos
           << "\n";
  if (SigOurs == SigKhronos)
    return;

  int SigRef = signatureFromIRText(OptimizedIR);
  StringRef Culprit = SigRef == kSigSkip ? "unknown"
                      : SigOurs != SigRef ? "ours"
                                          : "khronos";
  // Only our backend is the system under test: abort (so libFuzzer records the
  // reproducer) only when the reference tiebreaker pins the divergence on us.
  // A Khronos-translator or unattributable divergence is not our bug, so we do
  // not halt the campaign on it; surface it under debug instead.
  if (Culprit != "ours") {
    if (Debug)
      errs() << "CROSS divergence (culprit " << Culprit << ", not aborting): "
             << "ours=" << SigOurs << " khronos=" << SigKhronos
             << " ref=" << SigRef << "\n";
    return;
  }
  saveFailureFinding(Data, Size, OptimizedIR, "cross-miscompile",
                     "o2-vs-khronos-ours");
  errs() << "FuzzX SPIR-V cross-miscompile (culprit ours): ours=" << SigOurs
         << " khronos=" << SigKhronos << " ref=" << SigRef << "\n";
  std::abort();
}

std::unique_ptr<Module> createIRSkeletonModule(LLVMContext &Ctx,
                                               StringRef /*CPU*/) {
  auto M = std::make_unique<Module>("fuzzx_spirv_crash", Ctx);
  M->setTargetTriple(Triple(DefaultTriple));

  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *GlobalPtr = PointerType::get(Ctx, 1);
  FunctionType *FT =
      FunctionType::get(VoidTy, {GlobalPtr, GlobalPtr, I32}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 "fuzz_kernel", *M);
  F->setCallingConv(CallingConv::SPIR_KERNEL);

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *Body = BasicBlock::Create(Ctx, "body", F);
  BasicBlock *Exit = BasicBlock::Create(Ctx, "exit", F);

  IRBuilder<> B(Entry);
  Argument *In = F->getArg(0);
  Argument *Out = F->getArg(1);
  Argument *N = F->getArg(2);
  Value *Ok = B.CreateICmpSGT(N, ConstantInt::get(I32, 0));
  B.CreateCondBr(Ok, Body, Exit);

  B.SetInsertPoint(Body);
  Value *V = B.CreateAlignedLoad(I32, In, Align(4));
  Value *Salt = B.CreateMul(N, ConstantInt::getSigned(I32, -1640531527));
  Value *Mix = B.CreateXor(V, Salt);
  B.CreateAlignedStore(Mix, Out, Align(4));
  B.CreateBr(Exit);

  B.SetInsertPoint(Exit);
  B.CreateRetVoid();
  return M;
}

bool validateIRCorpusModule(const Module &M) {
  if (M.empty())
    return false;
  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (F.getCallingConv() != CallingConv::SPIR_KERNEL &&
        F.getCallingConv() != CallingConv::SPIR_FUNC &&
        F.getCallingConv() != CallingConv::C)
      return false;
  }
  return true;
}

std::unique_ptr<Module> parseIRCorpusModule(const uint8_t *Data, size_t Size,
                                            LLVMContext &Ctx, StringRef CPU,
                                            bool *Valid = nullptr) {
  if (Valid)
    *Valid = false;
  if (Size == 0)
    return createIRSkeletonModule(Ctx, CPU);
  StringRef Buffer(reinterpret_cast<const char *>(Data), Size);
  MemoryBufferRef MemBuf(Buffer, "fuzzx-spirv-ir-bitcode");
  std::unique_ptr<Module> Parsed;
  // BitcodeReader is not hardened against arbitrary mutated bytes; trap its
  // assertions so we report only SPIR-V backend findings.
  CrashRecoveryContext CRC;
  CRC.RunSafely([&]() {
    Expected<std::unique_ptr<Module>> P = parseBitcodeFile(MemBuf, Ctx);
    if (!P) {
      consumeError(P.takeError());
      return;
    }
    Parsed = std::move(*P);
  });
  if (!Parsed)
    return createIRSkeletonModule(Ctx, CPU);
  // Force the triple so corpus mutation of target metadata cannot send us to
  // a different backend.
  Parsed->setTargetTriple(Triple(DefaultTriple));
  if (!validateIRCorpusModule(*Parsed))
    return createIRSkeletonModule(Ctx, CPU);
  if (Valid)
    *Valid = true;
  return Parsed;
}

CompileResult compileIRModuleToObject(Module &M, StringRef CPU,
                                      OptimizationLevel Level,
                                      std::string *IRText = nullptr) {
  CompileResult R;
  std::unique_ptr<TargetMachine> TM = createTargetMachine(CPU, Level);

  M.setDataLayout(TM->createDataLayout());
  if (verifyModule(M, &errs())) {
    R.FailureStage = "verify";
    return R;
  }

  auto runStage = [&](const char *Stage, auto &&Fn) -> bool {
    CrashRecoveryContext CRC;
    CRC.DumpStackAndCleanupOnFailure = true;
    if (!CRC.RunSafely(std::forward<decltype(Fn)>(Fn))) {
      R.FailureStage = Stage;
      R.Crashed = true;
      R.CrashRetCode = CRC.RetCode;
      return false;
    }
    return true;
  };

  bool PipelineOk = false;
  if (!runStage("opt",
                [&] { PipelineOk = runOptimizationPipeline(M, *TM, Level); }))
    return R;
  if (!PipelineOk) {
    R.FailureStage = "opt";
    return R;
  }

  if (IRText)
    *IRText = moduleToString(M);

  std::optional<SmallVector<char, 0>> Obj;
  if (!runStage("codegen", [&] { Obj = emitObject(M, *TM); }))
    return R;
  if (!Obj) {
    R.FailureStage = "codegen";
    return R;
  }
  R.Object = std::move(*Obj);
  R.Success = true;
  return R;
}

struct CompiledObject {
  SmallVector<char, 0> Object;
  std::string IR;
};

// Returns nullopt on non-crash compile failure (finding already saved).
// Aborts on crash so libFuzzer captures the reproducer.
std::optional<CompiledObject>
runCompileStage(Module &M, StringRef CPU, OptimizationLevel Level,
                StringRef Tag, const uint8_t *Data, size_t Size) {
  CompiledObject Out;
  CompileResult R = compileIRModuleToObject(M, CPU, Level, &Out.IR);
  if (R.Success) {
    Out.Object = std::move(R.Object);
    return Out;
  }
  saveFailureFinding(
      Data, Size, Out.IR,
      R.Crashed ? "compiler-crash" : "compiler-failure",
      (Tag + "-" + R.FailureStage).str(),
      R.Crashed ? std::optional<int>(R.CrashRetCode) : std::nullopt);
  if (R.Crashed)
    std::abort();
  return std::nullopt;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  if (Size > 1 << 20)
    return 0;

  StringRef CPU = getCPU();
  LLVMContext Ctx;
  bool ValidInput = false;
  std::unique_ptr<Module> M =
      parseIRCorpusModule(Data, Size, Ctx, CPU, &ValidInput);
  if (envFlag("FUZZX_DEBUG_ORACLE", false))
    errs() << "INPUT fate: valid=" << ValidInput << " size=" << Size << "\n";
  if (!ValidInput)
    return 0;

  static const bool ValidatorEnabled = envFlag("FUZZX_SPIRV_VAL", true);

  auto O0 = runCompileStage(*M, CPU, OptimizationLevel::O0, "o0", Data, Size);
  if (!O0)
    return 0;
  if (ValidatorEnabled)
    runValidatorStage(Data, Size, O0->IR, O0->Object, "o0");

  // Re-parse a fresh copy for O2 since the O0 pipeline mutated the module.
  ValidInput = false;
  std::unique_ptr<Module> M2 =
      parseIRCorpusModule(Data, Size, Ctx, CPU, &ValidInput);
  if (!ValidInput)
    return 0;

  auto O2 = runCompileStage(*M2, CPU, OptimizationLevel::O2, "o2", Data, Size);
  if (!O2)
    return 0;
  if (ValidatorEnabled)
    runValidatorStage(Data, Size, O2->IR, O2->Object, "o2");

  // No IL-capable OpenCL/Vulkan runtime exists on this host, so we cannot
  // launch the codegen output on a device.  Instead, when a translator + lli
  // are configured, run a semantic differential entirely in LLVM tooling:
  // reverse-translate the O0/O2 SPIR-V and interpret it, checking both against
  // the optimized IR interpreted directly.  See runMiscompileStage.
  static const bool MiscompileEnabled = envFlag("FUZZX_MISCOMPILE", true);
  static const bool CrossEnabled = envFlag("FUZZX_CROSS_TRANSLATOR", true);

  // Both stages reverse-translate and interpret our O2 object; compute that
  // signature once and share it to avoid a duplicate llvm-spirv -r + lli pair
  // per input.
  if (MiscompileEnabled || CrossEnabled) {
    int SigO2 = signatureFromSpirv(O2->Object);
    if (MiscompileEnabled)
      runMiscompileStage(Data, Size, O0->Object, O2->IR, SigO2);
    if (CrossEnabled)
      runCrossTranslatorStage(Data, Size, O2->IR, SigO2);
  }
  return 0;
}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  CrashRecoveryContext::Enable();
  install_fatal_error_handler(
      [](void *, const char *Reason, bool) {
        errs() << "FuzzX SPIR-V fatal: " << Reason << "\n";
        std::abort();
      },
      nullptr);
  install_bad_alloc_error_handler(
      [](void *, const char *Reason, bool) {
        errs() << "FuzzX SPIR-V bad alloc: " << Reason << "\n";
        std::abort();
      },
      nullptr);
  // Smoke-test target initialization once at startup so any registration
  // failure surfaces before the fuzzer enters its hot loop.
  (void)createTargetMachine(getCPU(), OptimizationLevel::O0);
  return 0;
}
