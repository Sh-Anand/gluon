#include <elfio/elfio.hpp>

#include <optional>

using namespace ELFIO;

struct KernelBinary {
    uint32_t start_pc = 0;
    const uint8_t* data;
    size_t size = 0;
};

static std::uint64_t g_device_mem_used = GPU_MEM_START_ADDR;

std::optional<uint32_t> allocateDeviceMemory(size_t bytes) {
    static const std::uint64_t capacity = static_cast<std::uint64_t>(GPU_DRAM_SIZE);
    size_t aligned_bytes = bytes + (bytes % sizeof(uint32_t));
    if (g_device_mem_used > capacity)
        return std::nullopt;
    if (bytes > capacity - g_device_mem_used)
        return std::nullopt;
    uint32_t addr = static_cast<uint32_t>(g_device_mem_used);
    g_device_mem_used += aligned_bytes;
    return addr;
}

int parseELF() {
elfio reader;
    if (!reader.load("sw/test/build/kernel.elf")) {
        fprintf(stderr, "loadKernelBinary: failed to load sw/test/build/kernel.elf\n");
        return -1;
    }

    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    for (unsigned i = 0; i < reader.segments.size(); ++i) {
        const segment* seg = reader.segments[i];
        if (seg->get_type() != PT_LOAD)
            continue;
        uint64_t seg_start = seg->get_virtual_address();
        uint64_t seg_end = seg_start + seg->get_memory_size();
        if (seg_start < min_vaddr)
            min_vaddr = seg_start;
        if (seg_end > max_vaddr)
            max_vaddr = seg_end;
    }

    if (min_vaddr == UINT64_MAX || max_vaddr == 0) {
        fprintf(stderr, "loadKernelBinary: no PT_LOAD segments found\n");
        return -1;
    }

    size_t total_size = max_vaddr - min_vaddr;

    uint8_t* data = (uint8_t*)calloc(1, total_size);

    // allocate GPU memory
    auto gpu_base_opt = allocateDeviceMemory(total_size);
    if (!gpu_base_opt) {
        fprintf(stderr, "loadKernelBinary: failed to allocate %llu bytes of GPU memory\n",
                (unsigned long long)total_size);
        return -1;
    }
    uint32_t gpu_base = *gpu_base_opt;
    fprintf(stderr, "loadKernelBinary: Allocated GPU memory at: 0x%08x (size: %zu bytes)\n", gpu_base, total_size);

    // find .rela.dyn section for dynamic relocations
    section* rela_dyn_sec = nullptr;
    for (unsigned i = 0; i < reader.sections.size(); ++i) {
        if (reader.sections[i]->get_name() == ".rela.dyn") {
            rela_dyn_sec = reader.sections[i];
            break;
        }
    }

    if (!rela_dyn_sec) {
        fprintf(stderr, "loadKernelBinary: No .rela.dyn section found\n");
    } else {
        // find symbol table for resolving symbols
        section* symtab_sec = nullptr;
        for (unsigned i = 0; i < reader.sections.size(); ++i) {
            if (reader.sections[i]->get_type() == SHT_DYNSYM || reader.sections[i]->get_type() == SHT_SYMTAB) {
                symtab_sec = reader.sections[i];
                break;
            }
        }
        symbol_section_accessor* symbols = nullptr;
        if (symtab_sec)
            symbols = new symbol_section_accessor(reader, symtab_sec);

        size_t num_entries = rela_dyn_sec->get_size() / rela_dyn_sec->get_entry_size();
        fprintf(stderr, ".rela.dyn: %zu entries\n", num_entries);
        fprintf(stderr, "  %-10s %-24s %-10s %s\n", "Offset", "TypeName", "Addend", "Symbol");

        relocation_section_accessor rela(reader, rela_dyn_sec);
        for (size_t j = 0; j < num_entries; ++j) {
            Elf64_Addr offset;
            Elf_Word symbol_idx;
            unsigned type;
            Elf_Sxword addend;
            rela.get_entry(j, offset, symbol_idx, type, addend);

            const char* type_name = "UNKNOWN";
            switch (type) {
                case 0:  type_name = "R_RISCV_NONE"; break;
                case 1:  type_name = "R_RISCV_32"; break;
                case 2:  type_name = "R_RISCV_64"; break;
                case 3:  type_name = "R_RISCV_RELATIVE"; break;
                case 5:  type_name = "R_RISCV_JUMP_SLOT"; break;
                case 10: type_name = "R_RISCV_TLS_DTPMOD32"; break;
                case 11: type_name = "R_RISCV_TLS_DTPREL32"; break;
                case 12: type_name = "R_RISCV_TLS_TPREL32"; break;
                default: break;
            }

            std::string sym_name = "";
            Elf64_Addr sym_value = 0;
            if (symbols && symbol_idx > 0) {
                Elf_Xword sym_size;
                unsigned char sym_bind, sym_type, sym_other;
                Elf_Half sym_section;
                symbols->get_symbol(symbol_idx, sym_name, sym_value, sym_size, sym_bind, sym_type, sym_section, sym_other);
            }

            fprintf(stderr, "  0x%08llx %-24s %-10lld %s (0x%llx)\n",
                    (unsigned long long)offset,
                    type_name,
                    (long long)addend,
                    sym_name.empty() ? "<none>" : sym_name.c_str(),
                    (unsigned long long)sym_value);
        }
        fprintf(stderr, "\n");

        delete symbols;
    }

    return 0;
}

std::optional<KernelBinary> loadKernelBinary(const std::string& kernel_name) {
    if (kernel_name.empty())
        return std::nullopt;

    if (parseELF() != 0) {
        fprintf(stderr, "loadKernelBinary: failed to parse ELF file\n");
        return std::nullopt;
    }

    return KernelBinary{0, nullptr, 0};
}