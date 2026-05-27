# llvm-spirv-fuzz

A crash-only libFuzzer harness for the LLVM SPIR-V backend, inspired by
[FuzzX](https://github.com/SemiAnalysisAI/FuzzX).

## Build

```
# drop or symlink an llvm-project checkout
ln -s /path/to/llvm-project third_party/llvm-project

# instrumented LLVM (sancov + assertions + SPIRV;X86)
./scripts/build_instrumented_llvm.sh

# libFuzzer target
./scripts/build_directed_fuzzer.sh
```

To reuse an existing LLVM build, set `LLVM_DIR=/path/to/llvm-build/lib/cmake/llvm`
for the second step.

## Run

```
./scripts/run_directed_fuzzer.sh -runs=10000
```

| Var | Default | Purpose |
| --- | --- | --- |
| `FUZZER_BIN` | `build/fuzzer/llvm_spirv_crash_fuzzer` | binary to run |
| `CORPUS_DIR` | `$FUZZX_RUNTIME_ROOT/corpus/directed` | libFuzzer corpus |
| `ARTIFACT_DIR` | `$FUZZX_RUNTIME_ROOT/artifacts/directed` | libFuzzer crash dumps |
| `FUZZX_FINDINGS_DIR` | `$FUZZX_RUNTIME_ROOT/findings` | `.bc` / `.ll` for each finding |
| `FUZZX_RUNTIME_ROOT` | `${TMPDIR:-/tmp}/fuzzx-spirv-$USER` | parent for the above |
