#include <elfio/elfio.hpp>

#include <optional>

using namespace ELFIO;

struct KernelBinary {
    uint32_t start_pc = 0;
    uint32_t kernel_pc = 0;
    const uint8_t* data;
    size_t size = 0;
    uint32_t load_offset = 0;  // File offset where loadable data starts
};

std::optional<KernelBinary> loadKernelBinary(const std::string& kernel_name) {
    if (kernel_name.empty())
        return std::nullopt;

    elfio reader;
    if (!reader.load("sw/test/build/kernel.elf")) {
        fprintf(stderr, "loadKernelBinary: failed to load sw/test/build/kernel.elf\n");
        return std::nullopt;
    }

    fprintf(stderr, "Segments (%d):\n", (int)reader.segments.size());
    fprintf(stderr, "  [Nr] %-12s %-10s %-10s %-10s %-10s %-5s\n", "Type", "Offset", "VirtAddr", "FileSize", "MemSize", "Flags");
    for (unsigned i = 0; i < reader.segments.size(); ++i) {
        const segment* seg = reader.segments[i];
        const char* type_str = "OTHER";
        switch (seg->get_type()) {
            case PT_NULL:    type_str = "NULL"; break;
            case PT_LOAD:    type_str = "LOAD"; break;
            case PT_DYNAMIC: type_str = "DYNAMIC"; break;
            case PT_INTERP:  type_str = "INTERP"; break;
            case PT_NOTE:    type_str = "NOTE"; break;
            case PT_PHDR:    type_str = "PHDR"; break;
            case PT_GNU_STACK: type_str = "GNU_STACK"; break;
            case PT_GNU_RELRO: type_str = "GNU_RELRO"; break;
            default: break;
        }
        fprintf(stderr, "  [%2d] %-12s 0x%08llx 0x%08llx 0x%08llx 0x%08llx %c%c%c\n",
                i,
                type_str,
                (unsigned long long)seg->get_offset(),
                (unsigned long long)seg->get_virtual_address(),
                (unsigned long long)seg->get_file_size(),
                (unsigned long long)seg->get_memory_size(),
                (seg->get_flags() & PF_R) ? 'R' : '-',
                (seg->get_flags() & PF_W) ? 'W' : '-',
                (seg->get_flags() & PF_X) ? 'X' : '-');
    }

    fprintf(stderr, "\nSections (%d):\n", (int)reader.sections.size());
    fprintf(stderr, "  [Nr] %-20s %-10s %-10s %-8s %-8s\n", "Name", "Type", "Addr", "Size", "Flags");
    for (unsigned i = 0; i < reader.sections.size(); ++i) {
        const section* sec = reader.sections[i];
        const char* type_str = "OTHER";
        switch (sec->get_type()) {
            case SHT_NULL:     type_str = "NULL"; break;
            case SHT_PROGBITS: type_str = "PROGBITS"; break;
            case SHT_SYMTAB:   type_str = "SYMTAB"; break;
            case SHT_STRTAB:   type_str = "STRTAB"; break;
            case SHT_RELA:     type_str = "RELA"; break;
            case SHT_HASH:     type_str = "HASH"; break;
            case SHT_DYNAMIC:  type_str = "DYNAMIC"; break;
            case SHT_NOTE:     type_str = "NOTE"; break;
            case SHT_NOBITS:   type_str = "NOBITS"; break;
            case SHT_REL:      type_str = "REL"; break;
            case SHT_DYNSYM:   type_str = "DYNSYM"; break;
            default: break;
        }
        fprintf(stderr, "  [%2d] %-20s %-10s 0x%08llx %8lld %c%c%c\n",
                i,
                sec->get_name().c_str(),
                type_str,
                (unsigned long long)sec->get_address(),
                (unsigned long long)sec->get_size(),
                (sec->get_flags() & SHF_WRITE) ? 'W' : '-',
                (sec->get_flags() & SHF_ALLOC) ? 'A' : '-',
                (sec->get_flags() & SHF_EXECINSTR) ? 'X' : '-');
    }

    return KernelBinary{0, 0, nullptr, 0, 0};
}