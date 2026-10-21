#include <cxxabi.h>
#include <elf.h>
#include <sys/mman.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "cpu.h"
#include "nb.h"

#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif
#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23
#endif

// The original 32-bit library, embedded by guest_image.S.
extern "C" const unsigned char nb_guest_image[];
extern "C" const unsigned char nb_guest_image_end[];

namespace nb {

namespace {

struct Loaded {
    gaddr base = 0;
    u32 image_size = 0;
    gaddr code_lo = 0, code_hi = 0;  // executable PT_LOAD
    u32 image_id = 0;                // hash of the build-id note
    gaddr exidx = 0;
    u32 exidx_count = 0;
    std::unordered_map<std::string, gaddr> exports;
    std::vector<std::pair<gaddr, gaddr>> funcs;  // (address, guest name pointer), sorted
} L;

std::unordered_map<std::string, gaddr> g_stub_cache;

void missing_import(Ctx& c) {
    c.cpu.dump("missing import");
    fatal("guest called unimplemented import '%s'", c.name);
}

gaddr resolve_import(const char* name) {
    if (gaddr d = find_data(name)) return d;
    auto it = g_stub_cache.find(name);
    if (it != g_stub_cache.end()) return it->second;
    Handler fn = find_handler(name);
    if (!fn) {
        NB_LOGW("import '%s' has no implementation; calls will abort", name);
        fn = missing_import;
    }
    gaddr stub = make_stub(strdup(name), fn);
    g_stub_cache[name] = stub;
    return stub;
}

u32 arm_imm(u32 w) {  // ARM data-processing modified immediate
    u32 rot = ((w >> 8) & 15) * 2, imm = w & 0xff;
    return rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
}

// Every call into an exported function goes through the PLT ("ldr pc, [ip, #n]!"), an indirect
// branch that dynarmic's arm64 backend can only resolve through a C++ hash lookup. Once the GOT
// is relocated the targets are fixed, so rewrite each entry into a direct "b target" that the
// JIT can link block-to-block.
void patch_plt(const u8* img, size_t size, gaddr base) {
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(img);
    if (!eh->e_shoff || eh->e_shoff + size_t(eh->e_shnum) * sizeof(Elf32_Shdr) > size) return;
    auto* sh = reinterpret_cast<const Elf32_Shdr*>(img + eh->e_shoff);
    const char* names = reinterpret_cast<const char*>(img + sh[eh->e_shstrndx].sh_offset);
    for (int i = 0; i < eh->e_shnum; ++i) {
        if (strcmp(names + sh[i].sh_name, ".plt") != 0) continue;
        gaddr start = base + sh[i].sh_addr, end = start + sh[i].sh_size;
        u32 patched = 0, skipped = 0;
        for (gaddr a = start; a + 12 <= end; a += 4) {
            u32 w0 = rd<u32>(a), w1 = rd<u32>(a + 4), w2 = rd<u32>(a + 8);
            // add ip, pc, #i0 ; add ip, ip, #i1 ; ldr pc, [ip, #i2]!
            if ((w0 & 0xfffff000) != 0xe28fc000 || (w1 & 0xfffff000) != 0xe28cc000 || (w2 & 0xfffff000) != 0xe5bcf000)
                continue;
            gaddr got = a + 8 + arm_imm(w0) + arm_imm(w1) + (w2 & 0xfff);
            gaddr target = rd<u32>(got);
            s64 off = (s64(target) - s64(a + 8)) / 4;
            if ((target & 3) || off < -(1 << 23) || off >= (1 << 23)) {
                ++skipped;
                continue;
            }
            wr<u32>(a, 0xea000000u | (u32(off) & 0xffffff));
            ++patched;
            a += 8;
        }
        NB_LOGI("PLT: %u entries turned into direct branches (%u left indirect)", patched, skipped);
    }
}

void load_image(const u8* img, size_t size, gaddr base) {
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(img);
    if (size < sizeof(Elf32_Ehdr) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_machine != EM_ARM)
        fatal("embedded guest image is not a 32-bit ARM ELF");
    auto* ph = reinterpret_cast<const Elf32_Phdr*>(img + eh->e_phoff);

    u32 max_va = 0;
    const Elf32_Phdr* dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD) max_va = std::max(max_va, ph[i].p_vaddr + ph[i].p_memsz);
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X)) {
            L.code_lo = base + ph[i].p_vaddr;
            L.code_hi = base + ph[i].p_vaddr + ph[i].p_memsz;
        }
        if (ph[i].p_type == PT_NOTE && ph[i].p_offset + ph[i].p_filesz <= size) {
            u32 hsh = 2166136261u;
            for (u32 k = 0; k < ph[i].p_filesz; ++k) hsh = (hsh ^ img[ph[i].p_offset + k]) * 16777619u;
            L.image_id = hsh ^ u32(size);
        }
        if (ph[i].p_type == PT_DYNAMIC) dyn = &ph[i];
        if (ph[i].p_type == PT_ARM_EXIDX) {
            L.exidx = base + ph[i].p_vaddr;
            L.exidx_count = ph[i].p_memsz / 8;
        }
    }
    L.base = base;
    L.image_size = page_round_up(max_va);
    mem_protect(base, L.image_size, PROT_READ | PROT_WRITE);
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset + ph[i].p_filesz > size) fatal("truncated guest image");
        memcpy(h(base + ph[i].p_vaddr), img + ph[i].p_offset, ph[i].p_filesz);
    }
    if (!dyn) fatal("guest image has no PT_DYNAMIC");

    gaddr symtab = 0, strtab = 0, rel = 0, jmprel = 0, init_array = 0, init_fn = 0;
    u32 relsz = 0, pltrelsz = 0, init_array_sz = 0, nsyms = 0;
    for (gaddr d = base + dyn->p_vaddr;; d += 8) {
        s32 tag = rd<s32>(d);
        u32 val = rd<u32>(d + 4);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_SYMTAB: symtab = base + val; break;
        case DT_STRTAB: strtab = base + val; break;
        case DT_HASH: nsyms = rd<u32>(base + val + 4); break;
        case DT_REL: rel = base + val; break;
        case DT_RELSZ: relsz = val; break;
        case DT_JMPREL: jmprel = base + val; break;
        case DT_PLTRELSZ: pltrelsz = val; break;
        case DT_INIT_ARRAY: init_array = base + val; break;
        case DT_INIT_ARRAYSZ: init_array_sz = val; break;
        case DT_INIT: init_fn = base + val; break;
        case DT_NEEDED: break;
        default: break;
        }
    }

    auto sym = [&](u32 i) { return hp<Elf32_Sym>(symtab + i * sizeof(Elf32_Sym)); };
    for (u32 i = 1; i < nsyms; ++i) {
        const Elf32_Sym* s = sym(i);
        if (s->st_shndx == SHN_UNDEF) continue;
        int bind = ELF32_ST_BIND(s->st_info), type = ELF32_ST_TYPE(s->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        L.exports.emplace(hs(strtab + s->st_name), base + s->st_value);
        if (type == STT_FUNC) L.funcs.push_back({(base + s->st_value) & ~1u, strtab + s->st_name});
    }
    std::sort(L.funcs.begin(), L.funcs.end());

    std::vector<gaddr> resolved(nsyms, ~0u);
    auto sym_addr = [&](u32 i) -> gaddr {
        if (resolved[i] != ~0u) return resolved[i];
        const Elf32_Sym* s = sym(i);
        gaddr a = s->st_shndx != SHN_UNDEF ? base + s->st_value : resolve_import(hs(strtab + s->st_name));
        if (s->st_shndx != SHN_UNDEF && ELF32_ST_TYPE(s->st_info) == STT_GNU_IFUNC) {
            // The symbol's value is a resolver returning the real implementation (here: the
            // __atomic_* helpers behind libstdc++'s string refcounts). Binding the resolver itself
            // made every atomic op return a function pointer, so no string was ever freed.
            gaddr impl = gaddr(call_guest(a, {}));
            NB_LOGI("ifunc %s resolved to %#x", hs(strtab + s->st_name), impl);
            a = impl;
        }
        return resolved[i] = a;
    };

    auto apply = [&](gaddr table, u32 bytes) {
        for (gaddr r = table; r < table + bytes; r += 8) {
            gaddr where = base + rd<u32>(r);
            u32 info = rd<u32>(r + 4);
            u32 type = ELF32_R_TYPE(info), si = ELF32_R_SYM(info);
            switch (type) {
            case R_ARM_RELATIVE: wr<u32>(where, rd<u32>(where) + base); break;
            case R_ARM_ABS32: wr<u32>(where, rd<u32>(where) + sym_addr(si)); break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: wr<u32>(where, sym_addr(si)); break;
            case 0: break;
            default: fatal("unsupported relocation type %u", type);
            }
        }
    };
    apply(rel, relsz);
    apply(jmprel, pltrelsz);
    patch_plt(img, size, base);
    NB_LOGI("guest library loaded at %#x (%u bytes, %zu exports, %zu stubs)", base, L.image_size, L.exports.size(),
            g_stub_cache.size());

    if (init_fn) call_guest(init_fn, {});
    for (u32 i = 0; i < init_array_sz / 4; ++i) {
        gaddr f = rd<u32>(init_array + i * 4);
        if (f && f != ~0u) call_guest(f, {});
    }
    NB_LOGI("guest static constructors done (%u)", init_array_sz / 4);
}

}  // namespace

u32 guest_image_id() { return L.image_id; }
bool guest_is_code(gaddr pc) { return pc >= L.code_lo && pc < L.code_hi; }

gaddr guest_sym(const char* name) {
    auto it = L.exports.find(name);
    return it == L.exports.end() ? 0 : it->second;
}

void guest_exidx(gaddr* start, u32* count) {
    *start = L.exidx;
    *count = L.exidx_count;
}

std::string symbolize(gaddr pc) {
    char buf[512];
    if (pc < L.base || pc >= L.base + L.image_size || L.funcs.empty()) {
        snprintf(buf, sizeof buf, "%#x", pc);
        return buf;
    }
    auto it = std::upper_bound(L.funcs.begin(), L.funcs.end(), std::make_pair(pc, ~0u));
    if (it == L.funcs.begin()) {
        snprintf(buf, sizeof buf, "%#x (lib+%#x)", pc, pc - L.base);
        return buf;
    }
    --it;
    const char* mangled = hs(it->second);
    int st = 0;
    char* dem = abi::__cxa_demangle(mangled, nullptr, nullptr, &st);
    snprintf(buf, sizeof buf, "%#x (lib+%#x) %s+%#x", pc, pc - L.base, dem ? dem : mangled, pc - it->first);
    free(dem);
    return buf;
}

void runtime_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        mem_init();
        cpu_init();
        register_libc();
        register_stdio();
        register_pthread();
        register_gles();
        register_android();
        register_jni();
        load_image(nb_guest_image, size_t(nb_guest_image_end - nb_guest_image), layout::kLibBase);
    });
}

}  // namespace nb
