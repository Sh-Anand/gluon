#include "rad.h"
#include "rad_driver.h"

#include <errno.h>
#include <cstdint>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <elf.h>

#include <fstream>
#include <memory>
#include <optional>
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
#include <llvm-c/Object.h>
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
        LLVMRelocPIC,
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
        "-m",
        "elf32lriscv",
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

extern char **environ;

static std::optional<std::vector<std::uint8_t>> rad_read_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < 0)
        return std::nullopt;
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<size_t>(size));
    if (!data.empty()) {
        if (!file.read(reinterpret_cast<char *>(data.data()), size))
            return std::nullopt;
    }
    return data;
}

static void rad_write_disassembly(const std::string &binary_path,
                                  const char *triple,
                                  const std::string &output_path) {
    const char *override_tool = ::getenv("RAD_LLVM_OBJDUMP");
    std::string tool_storage;
    if (override_tool && override_tool[0]) {
        tool_storage = override_tool;
    } else {
        const char *muon_root = ::getenv("MUON_32");
        if (!muon_root || !muon_root[0]) {
            std::ofstream out(output_path, std::ios::out | std::ios::trunc);
            if (out)
                out << "MUON_32 not set; skipping disassembly\n";
            return;
        }
        tool_storage = std::string(muon_root) + "/bin/llvm-objdump";
    }
    const char *tool = tool_storage.c_str();
    std::string triple_flag = std::string("--triple=") + (triple && triple[0] ? triple : "riscv32-unknown-elf");
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0)
        return;
    int fd = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd == -1) {
        posix_spawn_file_actions_destroy(&actions);
        return;
    }
    posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fd);
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(tool));
    argv.push_back(const_cast<char *>("-d"));
    argv.push_back(const_cast<char *>(triple_flag.c_str()));
    argv.push_back(const_cast<char *>(binary_path.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int spawn_rc = posix_spawn(&pid, tool, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fd);
    if (spawn_rc != 0) {
        std::ofstream out(output_path, std::ios::out | std::ios::trunc);
        if (out)
            out << tool << " spawn failed: " << strerror(spawn_rc) << '\n';
        return;
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) == -1)
        return;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::ofstream out(output_path, std::ios::out | std::ios::trunc);
        if (out)
            out << tool << " exited with status " << status << '\n';
    }
}

static std::optional<std::uint32_t> rad_find_symbol_pc(const std::vector<std::uint8_t> &binary,
                                                       const char *symbol_name) {
    if (!symbol_name)
        return std::nullopt;
    LLVMMemoryBufferRef buffer = LLVMCreateMemoryBufferWithMemoryRangeCopy(
        reinterpret_cast<const char *>(binary.data()),
        binary.size(),
        "kernel_binary");
    if (!buffer)
        return std::nullopt;
    LLVMObjectFileRef object = LLVMCreateObjectFile(buffer);
    if (!object) {
        LLVMDisposeMemoryBuffer(buffer);
        return std::nullopt;
    }
    LLVMSymbolIteratorRef symbol = LLVMGetSymbols(object);
    std::optional<std::uint64_t> vaddr;
    while (!LLVMIsSymbolIteratorAtEnd(object, symbol)) {
        const char *name = LLVMGetSymbolName(symbol);
        if (name && strcmp(name, symbol_name) == 0) {
            unsigned long long addr = LLVMGetSymbolAddress(symbol);
            vaddr = addr;
            break;
        }
        LLVMMoveToNextSymbol(symbol);
    }
    LLVMDisposeSymbolIterator(symbol);
    LLVMDisposeObjectFile(object);
    if (!vaddr)
        return std::nullopt;
    if (binary.size() < sizeof(Elf32_Ehdr))
        return std::nullopt;
    const auto *ehdr = reinterpret_cast<const Elf32_Ehdr *>(binary.data());
    if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0)
        return std::nullopt;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32)
        return std::nullopt;
    if (ehdr->e_phentsize != sizeof(Elf32_Phdr))
        return std::nullopt;
    std::size_t ph_size = static_cast<std::size_t>(ehdr->e_phnum) * sizeof(Elf32_Phdr);
    if (binary.size() < static_cast<std::size_t>(ehdr->e_phoff) + ph_size)
        return std::nullopt;
    const auto *phdrs = reinterpret_cast<const Elf32_Phdr *>(binary.data() + ehdr->e_phoff);
    for (std::size_t i = 0; i < ehdr->e_phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD)
            continue;
        std::uint64_t start = phdrs[i].p_vaddr;
        std::uint64_t size = phdrs[i].p_memsz;
        if (*vaddr < start || *vaddr >= start + size)
            continue;
        std::uint64_t offset = (*vaddr - start) + phdrs[i].p_offset;
        if (offset > UINT32_MAX)
            return std::nullopt;
        return static_cast<std::uint32_t>(offset);
    }
    return std::nullopt;
}

struct KernelBinary {
    std::vector<std::uint8_t> image;
    std::uint32_t entry_pc;
};

static std::optional<KernelBinary> rad_build_kernel_binary(const char *kernel_name) {
    extern const unsigned char __gluon_kernel_start[];
    extern const unsigned char __gluon_kernel_end[];
    const unsigned char *bitcode = __gluon_kernel_start;
    const unsigned char *bitcode_end = __gluon_kernel_end;
    const unsigned long bitcode_size =
        (unsigned long)(bitcode_end > bitcode ? (bitcode_end - bitcode) : 0);
    if (!bitcode_size) {
        fprintf(stderr, "radKernelLaunch: missing kernel bitcode\n");
        return std::nullopt;
    }
    rad_initialize_llvm_once();
    LLVMContextRef context = LLVMContextCreate();
    if (!context)
        return std::nullopt;
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
        return std::nullopt;
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
    std::optional<KernelBinary> output;
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
            (void)::mkdir("build", 0777);
            std::string kernel_name_str = kernel_name ? kernel_name : "kernel";
            std::string obj_path = "build/" + kernel_name_str + "_codegen.o";
            std::string linked_path = "build/" + kernel_name_str + "_linked.so";
            std::ofstream out(obj_path, std::ios::binary);
            if (!out) {
                fprintf(stderr, "radKernelLaunch: unable to open %s for writing\n", obj_path.c_str());
            } else {
                out.write(buf_start, static_cast<std::streamsize>(buf_size));
                out.close();
                if (!out) {
                    fprintf(stderr, "radKernelLaunch: failed to write codegen object\n");
                } else if (rad_link_with_lld(obj_path.c_str(), linked_path.c_str()) != 0) {
                    fprintf(stderr, "radKernelLaunch: linker failed\n");
                } else {
                    std::string disasm_path = "build/" + kernel_name_str + "_linked.dis";
                    rad_write_disassembly(linked_path, module_triple, disasm_path);
                    auto binary_data = rad_read_file(linked_path);
                    if (!binary_data) {
                        fprintf(stderr, "radKernelLaunch: failed to read linked binary\n");
                    } else {
                        auto entry_pc = rad_find_symbol_pc(*binary_data, kernel_name);
                        if (!entry_pc) {
                            fprintf(stderr, "radKernelLaunch: missing entry symbol %s\n", kernel_name);
                        } else {
                            KernelBinary kernel_binary;
                            kernel_binary.image = std::move(*binary_data);
                            kernel_binary.entry_pc = *entry_pc;
                            output = std::move(kernel_binary);
                        }
                    }
                }
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
    return output;
}

extern "C" void radKernelLaunch(const char *kernel_name,
                                 radDim3 grid_dim,
                                 radDim3 block_dim) {
    if (!kernel_name)
        return;
    if (!rad::IsConnectionReady()) {
        fprintf(stderr, "radKernelLaunch: connection not initialized\n");
        return;
    }
    auto kernel_binary = rad_build_kernel_binary(kernel_name);
    if (!kernel_binary)
        return;
    if (grid_dim.x > UINT16_MAX || grid_dim.y > UINT16_MAX || grid_dim.z > UINT16_MAX) {
        fprintf(stderr, "radKernelLaunch: grid dimension exceeds limit\n");
        return;
    }
    if (block_dim.x > UINT16_MAX || block_dim.y > UINT16_MAX || block_dim.z > UINT16_MAX) {
        fprintf(stderr, "radKernelLaunch: block dimension exceeds limit\n");
        return;
    }
    if (kernel_binary->image.size() > UINT32_MAX) {
        fprintf(stderr, "radKernelLaunch: binary too large\n");
        return;
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(36 + kernel_binary->image.size());
    const auto push_u32 = [&payload](std::uint32_t value) {
        payload.push_back(static_cast<std::uint8_t>(value & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    };
    const auto push_u16 = [&payload](std::uint16_t value) {
        payload.push_back(static_cast<std::uint8_t>(value & 0xFF));
        payload.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    };
    const auto push_u8 = [&payload](std::uint8_t value) {
        payload.push_back(value);
    };
    push_u32(kernel_binary->entry_pc);
    push_u16(static_cast<std::uint16_t>(grid_dim.x));
    push_u16(static_cast<std::uint16_t>(grid_dim.y));
    push_u16(static_cast<std::uint16_t>(grid_dim.z));
    push_u16(static_cast<std::uint16_t>(block_dim.x));
    push_u16(static_cast<std::uint16_t>(block_dim.y));
    push_u16(static_cast<std::uint16_t>(block_dim.z));
    push_u8(1);
    push_u32(1);
    push_u8(0);
    push_u32(0);
    push_u32(static_cast<std::uint32_t>(kernel_binary->image.size()));
    push_u32(0);
    push_u16(0);
    payload.insert(payload.end(), kernel_binary->image.begin(), kernel_binary->image.end());
    if (payload.size() > UINT32_MAX) {
        fprintf(stderr, "radKernelLaunch: payload too large\n");
        return;
    }
    rad::KernelLaunchHeader header{};
    header.command_id = 0;
    header.host_offset = 0;
    header.payload_size = static_cast<std::uint32_t>(payload.size());
    std::uint32_t gpu_addr = rad::GetGpuAddress();
    if (gpu_addr == 0) {
        gpu_addr = 0x8000;
    }
    header.gpu_addr = gpu_addr;
    auto response = rad::SubmitKernelLaunch(header, payload);
    if (!response) {
        fprintf(stderr, "radKernelLaunch: failed to submit kernel launch\n");
    }
}
