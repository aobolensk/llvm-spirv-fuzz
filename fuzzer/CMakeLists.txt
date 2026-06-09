cmake_minimum_required(VERSION 3.20)

project(FuzzXLLVMSPIRVCrashFuzzer LANGUAGES C CXX)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(HandleLLVMOptions)

set(FUZZER_SANITIZERS "fuzzer" CACHE STRING
    "Comma-separated sanitizer list for the fuzzer binary")

add_executable(llvm_spirv_crash_fuzzer llvm_spirv_crash_fuzzer.cpp)
target_compile_features(llvm_spirv_crash_fuzzer PRIVATE cxx_std_17)
target_include_directories(llvm_spirv_crash_fuzzer PRIVATE
    ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(llvm_spirv_crash_fuzzer PRIVATE
    ${LLVM_DEFINITIONS}
)

find_package(SPIRV-Tools CONFIG QUIET PATHS ${SPIRV_TOOLS_DIR})

function(_fuzzx_pick_spirv_tools_target out_var)
    foreach(_cand SPIRV-Tools-static SPIRV-Tools SPIRV-Tools-shared)
        if(TARGET ${_cand})
            set(${out_var} ${_cand} PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

_fuzzx_pick_spirv_tools_target(_spirv_tools_target)

# Some SPIRV-Tools build trees ship a stub top-level SPIRV-ToolsConfig.cmake
# that points at a missing target file; the working one is under source/.
if(NOT _spirv_tools_target AND SPIRV_TOOLS_DIR AND
   EXISTS "${SPIRV_TOOLS_DIR}/source/SPIRV-ToolsTarget.cmake")
    include("${SPIRV_TOOLS_DIR}/source/SPIRV-ToolsTarget.cmake")
    _fuzzx_pick_spirv_tools_target(_spirv_tools_target)
endif()

if(_spirv_tools_target)
    message(STATUS "FuzzX: linking SPIR-V validator via ${_spirv_tools_target}")
    target_compile_definitions(llvm_spirv_crash_fuzzer PRIVATE FUZZX_HAVE_SPIRV_TOOLS=1)
    target_link_libraries(llvm_spirv_crash_fuzzer PRIVATE ${_spirv_tools_target})
    # Build-tree target files don't carry INTERFACE_INCLUDE_DIRECTORIES.
    if(SPIRV_TOOLS_DIR AND EXISTS "${SPIRV_TOOLS_DIR}/../include/spirv-tools/libspirv.h")
        target_include_directories(llvm_spirv_crash_fuzzer PRIVATE
            "${SPIRV_TOOLS_DIR}/../include")
    endif()
else()
    message(STATUS "FuzzX: SPIRV-Tools not found; spirv-val runs via subprocess "
                   "fallback. Pass -DSPIRV_TOOLS_DIR=/path/to/SPIRV-Tools/build "
                   "(or install) to link libspirv in-process.")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(llvm_spirv_crash_fuzzer PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=${FUZZER_SANITIZERS}
    )
    target_link_options(llvm_spirv_crash_fuzzer PRIVATE
        -fsanitize=${FUZZER_SANITIZERS}
    )
endif()

if(TARGET LLVM)
    target_link_libraries(llvm_spirv_crash_fuzzer PRIVATE LLVM)
else()
    llvm_map_components_to_libnames(LLVM_LIBS
        Analysis
        AsmParser
        BitReader
        BitWriter
        CodeGen
        Core
        IRReader
        MC
        Passes
        SPIRVCodeGen
        SPIRVDesc
        SPIRVInfo
        Support
        Target
        TargetParser
        TransformUtils
    )
    target_link_libraries(llvm_spirv_crash_fuzzer PRIVATE ${LLVM_LIBS})
endif()
