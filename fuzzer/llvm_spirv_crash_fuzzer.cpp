#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Function.h"
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

  // SPIR-V has no host runtime, so we stop after codegen.  See ../README.md
  // for why a faithful differential port (the AMDGPU/PTX pattern) requires
  // setting up a Vulkan or OpenCL ICD.
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
