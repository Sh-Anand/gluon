#include <elfio/elfio.hpp>

#include <cassert>
#include <optional>
#include <unordered_map>

using namespace ELFIO;

struct GPUBinary {
    uint32_t gpu_base_addr;
    uint32_t start_pc;
    const uint8_t* data;
    size_t size;
};

static elfio reader;
static std::unordered_map<std::string, uint32_t> elf_symbol_map;
uint32_t elf_min_vaddr = 0; // this should always be 0 but keeping it just in case
uint32_t gpu_base_addr = 0;
uint32_t start_pc = 0;
uint8_t* binary_data = nullptr;
size_t size = 0;

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

uint32_t getSymbolAddress(const std::string& symbol_name) {
    assert(!symbol_name.empty() && binary_data);
    auto it = elf_symbol_map.find(symbol_name);
    assert(it != elf_symbol_map.end() && "symbol not found");
    uint32_t addr = it->second - elf_min_vaddr;
    return gpu_base_addr + addr;
}

void readELF(const std::string& elf_path) {
    assert(reader.load(elf_path) && "elf loading failed");
}

void parseELF() {
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

    assert(min_vaddr != UINT64_MAX && max_vaddr != 0 && "no PT_LOAD segments");

    size_t total_size = max_vaddr - min_vaddr;

    auto gpu_base_opt = allocateDeviceMemory(total_size);
    assert(gpu_base_opt);
    uint32_t gpu_base = *gpu_base_opt;
    fprintf(stderr, "parseELF: Allocated GPU memory at: 0x%08x (size: %zu bytes)\n", gpu_base, total_size);

    uint8_t* data = (uint8_t*)calloc(1, total_size);

    // memcpy PT_LOADs
    for (unsigned i = 0; i < reader.segments.size(); ++i) {
        const segment* seg = reader.segments[i];
        if (seg->get_type() != PT_LOAD)
            continue;
        size_t offset = seg->get_virtual_address() - min_vaddr;
        memcpy(data + offset, seg->get_data(), seg->get_file_size());
    }

    // apply .rela.dyn
    section* rela_dyn_sec = nullptr;
    for (unsigned i = 0; i < reader.sections.size(); ++i) {
        if (reader.sections[i]->get_name() == ".rela.dyn") {
            rela_dyn_sec = reader.sections[i];
            break;
        }
    }

    if (!rela_dyn_sec) {
        fprintf(stderr, "parseELF: No .rela.dyn section found (no relocations needed)\n");
    } else {
        size_t num_entries = rela_dyn_sec->get_size() / rela_dyn_sec->get_entry_size();
        fprintf(stderr, "parseELF: Applying %zu relocations\n", num_entries);

        relocation_section_accessor rela(reader, rela_dyn_sec);
        for (size_t j = 0; j < num_entries; ++j) {
            Elf64_Addr offset;
            Elf_Word symbol_idx;
            unsigned type;
            Elf_Sxword addend;
            rela.get_entry(j, offset, symbol_idx, type, addend);

            if (type == 3) { // R_RISCV_RELATIVE
                // assuming .rela.dyn entries always point to elf virt addrs in the PT_LOAD segments
                size_t data_offset = offset - min_vaddr;
                uint32_t new_value = static_cast<uint32_t>(addend + gpu_base);
                memcpy(data + data_offset, &new_value, sizeof(uint32_t));
                fprintf(stderr, "  [%zu] R_RISCV_RELATIVE @ 0x%08llx: %lld + 0x%x = 0x%08x\n", 
                        j, (unsigned long long)offset, (long long)addend, gpu_base, new_value);
            } else {
                fprintf(stderr, "  [%zu] Unhandled relocation type %u @ 0x%08llx\n",
                        j, type, (unsigned long long)offset);
            }
        }
    }

    // build symbol cache
    for (unsigned i = 0; i < reader.sections.size(); ++i) {
        if (reader.sections[i]->get_type() == SHT_SYMTAB) {
            symbol_section_accessor symbols(reader, reader.sections[i]);
            for (unsigned j = 0; j < symbols.get_symbols_num(); ++j) {
                std::string name;
                Elf64_Addr value;
                Elf_Xword size;
                unsigned char bind, type, other;
                Elf_Half section;
                symbols.get_symbol(j, name, value, size, bind, type, section, other);
                elf_symbol_map[name] = value;
            }
            break;
        }
    }

    gpu_base_addr = gpu_base;
    binary_data = data;
    size = total_size;
    elf_min_vaddr = min_vaddr;

    uint32_t start_pc = getSymbolAddress("_start");
}

std::optional<GPUBinary> loadKernel(const std::string& kernel_name) {
    if (kernel_name.empty())
        return std::nullopt;

    readELF("sw/test/build/kernel.elf");
    parseELF();

    return GPUBinary{gpu_base_addr, start_pc, binary_data, size};
}