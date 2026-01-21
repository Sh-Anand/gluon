#ifndef LOADER_HPP
#define LOADER_HPP

#include <elfio/elfio.hpp>

#include <cassert>
#include <optional>
#include <unordered_map>

using namespace ELFIO;

struct GPUBinary {
    const uint8_t* data;
    size_t size;
};

static elfio reader;
static std::unordered_map<std::string, uint32_t> elf_symbol_map;
uint32_t elf_min_vaddr = 0; // this should always be 0 but keeping it just in case
uint8_t* binary_data = nullptr;
size_t size = 0;

uint32_t getSymbolAddress(const std::string& symbol_name, uint32_t reloc_addr) {
    assert(!symbol_name.empty() && binary_data);
    auto it = elf_symbol_map.find(symbol_name);
    assert(it != elf_symbol_map.end() && "symbol not found");
    uint32_t addr = it->second - elf_min_vaddr;
    return reloc_addr + addr;
}

void parseELF(const std::string& elf_path) {
    assert(reader.load(elf_path) && "elf loading failed");
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

    uint8_t* data = (uint8_t*)calloc(1, total_size);

    // memcpy PT_LOADs
    for (unsigned i = 0; i < reader.segments.size(); ++i) {
        const segment* seg = reader.segments[i];
        if (seg->get_type() != PT_LOAD)
            continue;
        size_t offset = seg->get_virtual_address() - min_vaddr;
        memcpy(data + offset, seg->get_data(), seg->get_file_size());
    }

    size = total_size;
    binary_data = data;
    elf_min_vaddr = min_vaddr;
}

// also builds symbol cache
void applyRelocations(uint32_t reloc_addr) {
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
                size_t data_offset = offset - elf_min_vaddr;
                uint32_t new_value = static_cast<uint32_t>(addend + reloc_addr);
                memcpy(binary_data + data_offset, &new_value, sizeof(uint32_t));
                fprintf(stderr, "  [%zu] R_RISCV_RELATIVE @ 0x%08llx: %lld + 0x%x = 0x%08x\n", 
                        j, (unsigned long long)offset, (long long)addend, reloc_addr, new_value);
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
}

GPUBinary initELF() {
    if (!binary_data)
        parseELF("sw/test/build/kernel.elf");
    return GPUBinary{binary_data, size};
}

#endif