#ifndef LOADER_HPP
#define LOADER_HPP

#include <elfio/elfio.hpp>

#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace ELFIO;

class ELFLoader {

public:
    elfio reader;
    std::unordered_map<std::string, uint32_t> elf_symbol_map;
    uint32_t elf_min_vaddr; // this should always be 0 but keeping it just in case
    uint32_t gpu_addr;
    uint8_t* binary_data;
    size_t size;
    std::unordered_map<uint32_t, std::bitset<128>> fn_regs;      // func -> GPRs it writes (.rad.regs)
    std::unordered_map<uint32_t, std::vector<uint32_t>> fn_refs; // func -> symbols it references (.rad.refs)
    std::unordered_map<std::string, uint8_t> kernel_regs;

    ELFLoader() : elf_min_vaddr(0), binary_data(nullptr), size(0) {}

    ELFLoader(const std::string& elf_path) : elf_min_vaddr(0), gpu_addr(0), binary_data(nullptr), size(0) {
        parseELF(elf_path);
    }

    ~ELFLoader() {
        free(binary_data);
    }

    uint32_t getSymbolAddress(const std::string& symbol_name) {
        assert(!symbol_name.empty() && binary_data);
        auto it = elf_symbol_map.find(symbol_name);
        assert(it != elf_symbol_map.end() && "symbol not found");
        uint32_t addr = it->second - elf_min_vaddr;
        return gpu_addr + addr;
    }

    // rename regs a warp allocates: GPRs written by everything reachable from _start and the kernel
    uint8_t getKernelRegs(const std::string& kernel_name) {
        auto cached = kernel_regs.find(kernel_name);
        if (cached != kernel_regs.end())
            return cached->second;
        std::bitset<128> used;
        std::unordered_set<uint32_t> seen;
        std::vector<uint32_t> work{elf_symbol_map.at("_start"), elf_symbol_map.at(kernel_name)};
        while (!work.empty()) {
            uint32_t fn = work.back();
            work.pop_back();
            // refs to data or local labels have no regs entry
            if (!fn_regs.count(fn) || !seen.insert(fn).second)
                continue;
            used |= fn_regs[fn];
            for (uint32_t ref : fn_refs[fn])
                work.push_back(ref);
        }
        used.reset(0); // x0 is preassigned but still takes an entry
        return kernel_regs[kernel_name] = used.count() + 1;
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

        // non-alloc sections from the muon toolchain, resolved to link-time addresses
        auto words = [&](const char* name) {
            std::vector<uint32_t> w;
            if (const section* sec = reader.sections[name]) {
                w.resize(sec->get_size() / 4);
                memcpy(w.data(), sec->get_data(), w.size() * 4);
            }
            return w;
        };
        std::vector<uint32_t> regs = words(".rad.regs"); // {func, mask[4]}
        for (size_t i = 0; i + 5 <= regs.size(); i += 5)
            for (int j = 0; j < 4; ++j)
                fn_regs[regs[i]] |= std::bitset<128>(regs[i + 1 + j]) << (32 * j);
        std::vector<uint32_t> refs = words(".rad.refs"); // {from, to}
        for (size_t i = 0; i + 2 <= refs.size(); i += 2)
            fn_refs[refs[i]].push_back(refs[i + 1]);
    }

    // also builds symbol cache
    void applyRelocations(uint32_t reloc_addr) {
        this->gpu_addr = reloc_addr;
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
};

#endif
