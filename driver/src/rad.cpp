#include "rad.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <lld/Common/Driver.h>
#include <llvm/Support/raw_ostream.h>

LLD_HAS_DRIVER(elf);

static void rad_initialize_llvm_once(void) {
    static int initialized = 0;
    if (initialized)
        return;
    LLVMInitializeRISCVTargetInfo();
    LLVMInitializeRISCVTarget();
    LLVMInitializeRISCVTargetMC();
    LLVMInitializeRISCVAsmPrinter();
    LLVMInitializeRISCVAsmParser();
    initialized = 1;
}

static LLVMTargetMachineRef rad_create_target_machine(const char *triple) {
    if (!triple || triple[0] == '\0') {
        return NULL;
    }

    LLVMTargetRef target = NULL;
    char *error_message = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &error_message) != 0) {
        if (error_message) {
            fprintf(stderr, "radKernelLaunch: LLVMGetTargetFromTriple failed: %s\n",
                    error_message);
            LLVMDisposeMessage(error_message);
        }
        return NULL;
    }

    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target,
        triple,
        "generic",
        "+vortex",
        LLVMCodeGenLevelDefault,
        LLVMRelocDefault,
        LLVMCodeModelDefault);

    if (!tm) {
        fprintf(stderr, "radKernelLaunch: failed to create target machine for %s\n", triple);
    }

    return tm;
}

static int rad_link_with_lld(const char *input_path, const char *output_path) {
    std::vector<const char *> args = {
        "ld.lld",
        "-shared",
        "-nostdlib",
        "-o",
        output_path,
        input_path
    };

    std::vector<lld::DriverDef> drivers = {{lld::Gnu, &lld::elf::link}};
    lld::Result result = lld::lldMain(args, llvm::outs(), llvm::errs(), drivers);

    if (result.retCode != 0) {
        fprintf(stderr, "radKernelLaunch: lld link failed (code %d)\n", result.retCode);
        return -1;
    }

    if (!result.canRunAgain) {
        fprintf(stderr, "radKernelLaunch: lld reported unsafe re-entry; subsequent links may fail\n");
    }

    return 0;
}

extern "C" void radKernelLaunch(const char *kernel_name, ...) {
    va_list args;
    va_start(args, kernel_name);
    va_end(args);

    extern const unsigned char _binary_build_hello_kernel_bc_start[];
    extern const unsigned char _binary_build_hello_kernel_bc_end[];

    const unsigned char *bitcode = _binary_build_hello_kernel_bc_start;
    const unsigned char *bitcode_end = _binary_build_hello_kernel_bc_end;
    const unsigned long bitcode_size =
        (unsigned long)(bitcode_end > bitcode ? (bitcode_end - bitcode) : 0);

    if (!bitcode_size) {
        fprintf(stderr, "radKernelLaunch: missing kernel bitcode\n");
        return;
    }

    rad_initialize_llvm_once();

    LLVMContextRef context = LLVMContextCreate();
    if (!context)
        return;

    LLVMMemoryBufferRef buffer = LLVMCreateMemoryBufferWithMemoryRangeCopy(
        (const char *)bitcode, (size_t)bitcode_size, "rad_kernel");
    LLVMModuleRef module = NULL;

    if (!buffer || LLVMParseBitcodeInContext2(context, buffer, &module) != 0 || !module) {
        fprintf(stderr, "radKernelLaunch: failed to parse kernel bitcode\n");
        if (module) {
            LLVMDisposeModule(module);
        }
        if (buffer) {
            LLVMDisposeMemoryBuffer(buffer);
        }
        LLVMContextDispose(context);
        return;
    }

    const char *module_triple = LLVMGetTarget(module);
    char *fallback_triple = NULL;
    if (!module_triple || !module_triple[0]) {
        fallback_triple = LLVMGetDefaultTargetTriple();
        module_triple = fallback_triple;
    }

    LLVMTargetMachineRef target_machine = rad_create_target_machine(module_triple);
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    if (options) {
        LLVMPassBuilderOptionsSetVerifyEach(options, 0);
        LLVMPassBuilderOptionsSetDebugLogging(options, 1);
    }

    if (target_machine &&
        LLVMRunPasses(module, "default<O2>", target_machine, options) != 0) {
        fprintf(stderr, "radKernelLaunch: LLVMRunPasses failed\n");
    } else if (target_machine) {
        LLVMMemoryBufferRef obj_buf = NULL;
        char *codegen_error = NULL;
        if (LLVMTargetMachineEmitToMemoryBuffer(target_machine,
                                               module,
                                               LLVMObjectFile,
                                               &codegen_error,
                                               &obj_buf)) {
            fprintf(stderr, "radKernelLaunch: codegen emit failed: %s\n",
                    codegen_error ? codegen_error : "<unknown>");
            if (codegen_error)
                LLVMDisposeMessage(codegen_error);
        } else if (obj_buf) {
            const char *buf_start = LLVMGetBufferStart(obj_buf);
            size_t buf_size = LLVMGetBufferSize(obj_buf);
            const char *out_path = "build/hello_kernel_codegen.o";
            const char *linked_path = "build/hello_kernel_linked.so";
            FILE *out = fopen(out_path, "wb");
            if (out) {
                if (fwrite(buf_start, 1, buf_size, out) != buf_size) {
                    fprintf(stderr, "radKernelLaunch: failed to write codegen object\n");
                }
                fclose(out);

                const bool linked = (rad_link_with_lld(out_path, linked_path) == 0);
                const char *disasm_target = linked ? linked_path : out_path;

                if (!linked) {
                    fprintf(stderr, "radKernelLaunch: linker failed, falling back to raw object disassembly\n");
                }

                const char *muon_root = getenv("MUON_32");
                char cmd[1024];
                if (muon_root && muon_root[0] != '\0') {
                    snprintf(cmd, sizeof(cmd), "%s/bin/llvm-objdump -d %s",
                             muon_root, disasm_target);
                } else {
                    snprintf(cmd, sizeof(cmd), "llvm-objdump -d %s", disasm_target);
                }
                if (system(cmd) != 0) {
                    fprintf(stderr, "radKernelLaunch: failed to run objdump command\n");
                }
            } else {
                fprintf(stderr, "radKernelLaunch: unable to open %s for writing\n", out_path);
            }
        }
        if (obj_buf)
            LLVMDisposeMemoryBuffer(obj_buf);
    }

    if (options)
        LLVMDisposePassBuilderOptions(options);
    if (target_machine)
        LLVMDisposeTargetMachine(target_machine);
    if (fallback_triple)
        LLVMDisposeMessage(fallback_triple);
    if (module)
        LLVMDisposeModule(module);
    if (buffer)
        LLVMDisposeMemoryBuffer(buffer);
    LLVMContextDispose(context);
}
