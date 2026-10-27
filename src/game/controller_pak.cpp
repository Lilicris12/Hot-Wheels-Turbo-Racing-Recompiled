// File-backed Controller Pak.
//
// Hot Wheels Turbo Racing saves exclusively to the Controller Pak (there is no EEPROM code
// in the ROM), but the runtime only provides "no pak inserted" stubs for the osPfs API. The
// game's osPfs* functions are renamed to hwtr_osPfs* in symbols/names.toml and ignored by the
// recompiler, and are implemented here on top of a standard 32 KiB .mpk image, the same
// format emulators use, so existing emulator saves can be dropped in.
//
// MPK layout (128 pages of 256 bytes, single bank):
//   page 0      ID area (label + 4 copies of the ID block)
//   page 1      inode table, page 2: backup inode table
//   pages 3-4   note (directory) table: 16 entries of 32 bytes
//   pages 5-127 file data; inode entries chain pages (0x0001 = last page, 0x0003 = free)

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <vector>

#include "hwtr/hwtr.hpp"
#include "recomp.h"
#include "librecomp/helpers.hpp"

namespace {
    constexpr size_t page_size = 0x100;
    constexpr size_t num_pages = 128;
    constexpr size_t pak_size = page_size * num_pages;
    constexpr uint16_t first_data_page = 5;
    constexpr size_t inode_page = 1;
    constexpr size_t inode_backup_page = 2;
    constexpr size_t note_table_offset = 3 * page_size;
    constexpr size_t note_size = 0x20;
    constexpr size_t max_notes = 16;
    constexpr uint16_t inode_last_page = 0x0001;
    constexpr uint16_t inode_free = 0x0003;
    constexpr size_t block_size = 32;
    constexpr int max_paks = 4;

    // libultra PFS error codes.
    constexpr int32_t PFS_ERR_NOPACK = 1;
    constexpr int32_t PFS_ERR_INVALID = 5;
    constexpr int32_t PFS_DATA_FULL = 7;
    constexpr int32_t PFS_DIR_FULL = 8;
    constexpr int32_t PFS_ERR_EXIST = 9;

    // OSPfs field offsets.
    constexpr int32_t pfs_status = 0x00;
    constexpr int32_t pfs_queue = 0x04;
    constexpr int32_t pfs_channel = 0x08;
    constexpr int32_t pfs_version = 0x4C;
    constexpr int32_t pfs_dir_size = 0x50;
    constexpr int32_t pfs_inode_table = 0x54;
    constexpr int32_t pfs_minode_table = 0x58;
    constexpr int32_t pfs_dir_table = 0x5C;
    constexpr int32_t pfs_inode_start_page = 0x60;
    constexpr int32_t pfs_banks = 0x64;
    constexpr int32_t pfs_activebank = 0x65;
    constexpr int32_t PFS_INITIALIZED = 0x1;

    class ControllerPak {
    public:
        explicit ControllerPak(int channel) : channel(channel) {}

        void load() {
            if (loaded) {
                return;
            }
            loaded = true;
            data.assign(pak_size, 0);
            std::ifstream in{ path(), std::ios::binary };
            if (in.good()) {
                in.read(reinterpret_cast<char*>(data.data()), pak_size);
                if (in.gcount() == (std::streamsize)pak_size && inode_checksum_valid()) {
                    return;
                }
            }
            format();
            save();
        }

        void save() const {
            std::filesystem::path p = path();
            std::filesystem::path tmp = p;
            tmp += ".tmp";
            {
                std::ofstream out{ tmp, std::ios::binary | std::ios::trunc };
                out.write(reinterpret_cast<const char*>(data.data()), pak_size);
            }
            std::error_code ec;
            std::filesystem::rename(tmp, p, ec);
        }

        uint16_t inode(size_t page) const {
            size_t off = inode_page * page_size + page * 2;
            return (uint16_t)((data[off] << 8) | data[off + 1]);
        }

        void set_inode(size_t page, uint16_t value) {
            for (size_t table : { inode_page, inode_backup_page }) {
                size_t off = table * page_size + page * 2;
                data[off] = value >> 8;
                data[off + 1] = value & 0xFF;
            }
        }

        void update_inode_checksum() {
            uint8_t sum = 0;
            for (size_t i = first_data_page * 2; i < page_size; i++) {
                sum += data[inode_page * page_size + i];
            }
            data[inode_page * page_size + 1] = sum;
            data[inode_backup_page * page_size + 1] = sum;
        }

        uint8_t* note(size_t index) { return &data[note_table_offset + index * note_size]; }

        static uint32_t read32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
        static uint16_t read16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
        static void write32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
        static void write16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }

        bool note_used(size_t index) {
            const uint8_t* n = note(index);
            return read32(n + 0) != 0 && read16(n + 4) != 0;
        }

        size_t free_pages() const {
            size_t count = 0;
            for (size_t page = first_data_page; page < num_pages; page++) {
                if (inode(page) == inode_free) {
                    count++;
                }
            }
            return count;
        }

        // Follows a file's page chain. Returns an empty list on a corrupt chain.
        std::vector<size_t> chain(size_t index) {
            std::vector<size_t> pages;
            size_t page = read16(note(index) + 6) & 0xFF;
            while (page >= first_data_page && page < num_pages && pages.size() < num_pages) {
                pages.push_back(page);
                uint16_t next = inode(page);
                if (next == inode_last_page) {
                    return pages;
                }
                page = next & 0xFF;
            }
            return {};
        }

        std::vector<uint8_t> data;

    private:
        std::filesystem::path path() const {
            std::filesystem::path dir = hwtr::get_app_folder_path() / "saves";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir / ("controller_pak_" + std::to_string(channel + 1) + ".mpk");
        }

        bool inode_checksum_valid() const {
            uint8_t sum = 0;
            for (size_t i = first_data_page * 2; i < page_size; i++) {
                sum += data[inode_page * page_size + i];
            }
            return data[inode_page * page_size + 1] == sum;
        }

        // Writes a blank, formatted pak image (equivalent to the libultra repair/format path).
        void format() {
            std::fill(data.begin(), data.end(), 0);

            // ID block: 24-byte serial, device id, bank count, version, two checksums.
            std::array<uint8_t, 32> id{};
            std::mt19937 rng{ std::random_device{}() };
            for (size_t i = 0; i < 24; i++) {
                id[i] = (uint8_t)rng();
            }
            write16(&id[24], 0x0001); // device id
            id[26] = 0x01;            // banks
            id[27] = 0x00;            // version
            uint16_t sum = 0;
            for (size_t i = 0; i < 28; i += 2) {
                sum += read16(&id[i]);
            }
            write16(&id[28], sum);
            write16(&id[30], (uint16_t)(0xFFF2 - sum));
            for (size_t off : { 0x20, 0x60, 0x80, 0xC0 }) {
                std::memcpy(&data[off], id.data(), id.size());
            }

            for (size_t page = first_data_page; page < num_pages; page++) {
                set_inode(page, inode_free);
            }
            update_inode_checksum();
        }

        int channel;
        bool loaded = false;
    };

    std::mutex pak_mutex;
    std::array<ControllerPak, max_paks> paks{ ControllerPak{0}, ControllerPak{1}, ControllerPak{2}, ControllerPak{3} };

    ControllerPak* pak_for(uint8_t* rdram, gpr pfs) {
        int32_t channel = MEM_W(pfs_channel, pfs);
        if (channel < 0 || channel >= max_paks) {
            return nullptr;
        }
        ControllerPak* pak = &paks[channel];
        pak->load();
        return pak;
    }

    void read_rdram(uint8_t* rdram, gpr addr, uint8_t* out, size_t len) {
        for (size_t i = 0; i < len; i++) {
            out[i] = MEM_BU((int32_t)i, addr);
        }
    }

    void write_rdram(uint8_t* rdram, gpr addr, const uint8_t* in, size_t len) {
        for (size_t i = 0; i < len; i++) {
            MEM_BU((int32_t)i, addr) = in[i];
        }
    }

    gpr stack_arg(uint8_t* rdram, recomp_context* ctx, int index) {
        return (gpr)(int32_t)MEM_W(index * 4, ctx->r29);
    }

    // Finds a note by (company, game code, name, extension). Returns -1 if absent.
    int find_note(uint8_t* rdram, ControllerPak& pak, uint16_t company, uint32_t game_code, gpr name_addr, gpr ext_addr) {
        uint8_t name[16];
        uint8_t ext[4];
        read_rdram(rdram, name_addr, name, sizeof(name));
        read_rdram(rdram, ext_addr, ext, sizeof(ext));
        for (size_t i = 0; i < max_notes; i++) {
            const uint8_t* n = pak.note(i);
            if (ControllerPak::read32(n + 0) == game_code && ControllerPak::read16(n + 4) == company &&
                std::memcmp(n + 0x0C, ext, 4) == 0 && std::memcmp(n + 0x10, name, 16) == 0 && pak.note_used(i)) {
                return (int)i;
            }
        }
        return -1;
    }

}

extern "C" void hwtr_osPfsIsPlug(uint8_t* rdram, recomp_context* ctx) {
    // Report a pak in every connected controller port.
    gpr bit_pattern = ctx->r5;
    uint8_t mask = 0;
    for (int i = 0; i < max_paks; i++) {
        if (hwtr::input::get_connected_device_info(i).connected_device != ultramodern::input::Device::None) {
            mask |= 1 << i;
        }
    }
    MEM_BU(0, bit_pattern) = mask;
    _return<int32_t>(ctx, 0);
}

extern "C" void hwtr_osPfsInitPak(uint8_t* rdram, recomp_context* ctx) {
    gpr queue = ctx->r4;
    gpr pfs = ctx->r5;
    int32_t channel = (int32_t)ctx->r6;

    if (channel < 0 || channel >= max_paks ||
        hwtr::input::get_connected_device_info(channel).connected_device == ultramodern::input::Device::None) {
        _return<int32_t>(ctx, PFS_ERR_NOPACK);
        return;
    }

    MEM_W(pfs_status, pfs) = PFS_INITIALIZED;
    MEM_W(pfs_queue, pfs) = (int32_t)queue;
    MEM_W(pfs_channel, pfs) = channel;
    MEM_W(pfs_version, pfs) = 0;
    MEM_W(pfs_dir_size, pfs) = (int32_t)max_notes;
    MEM_W(pfs_inode_table, pfs) = (int32_t)inode_page * 8;
    MEM_W(pfs_minode_table, pfs) = (int32_t)inode_backup_page * 8;
    MEM_W(pfs_dir_table, pfs) = 3 * 8;
    MEM_W(pfs_inode_start_page, pfs) = first_data_page;
    MEM_BU(pfs_banks, pfs) = 1;
    MEM_BU(pfs_activebank, pfs) = 0;

    std::lock_guard lock{ pak_mutex };
    paks[channel].load();
    _return<int32_t>(ctx, 0);
}

extern "C" void hwtr_osPfsRepairId(uint8_t* rdram, recomp_context* ctx) {
    _return<int32_t>(ctx, 0);
}

extern "C" void hwtr_osPfsChecker(uint8_t* rdram, recomp_context* ctx) {
    _return<int32_t>(ctx, 0);
}

extern "C" void hwtr_osPfsFreeBlocks(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ pak_mutex };
    ControllerPak* pak = pak_for(rdram, ctx->r4);
    if (pak == nullptr) {
        _return<int32_t>(ctx, PFS_ERR_NOPACK);
        return;
    }
    MEM_W(0, ctx->r5) = (int32_t)(pak->free_pages() * page_size);
    _return<int32_t>(ctx, 0);
}

extern "C" void hwtr_osPfsNumFiles(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ pak_mutex };
    ControllerPak* pak = pak_for(rdram, ctx->r4);
    if (pak == nullptr) {
        _return<int32_t>(ctx, PFS_ERR_NOPACK);
        return;
    }
    int32_t used = 0;
    for (size_t i = 0; i < max_notes; i++) {
        used += pak->note_used(i) ? 1 : 0;
    }
    MEM_W(0, ctx->r5) = (int32_t)max_notes;
    MEM_W(0, ctx->r6) = used;
    _return<int32_t>(ctx, 0);
}

// s32 osPfsFindFile(OSPfs*, u16 company_code, u32 game_code, u8* game_name, u8* ext_name, s32* file_no)
extern "C" void hwtr_osPfsFindFile(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ pak_mutex };
    ControllerPak* pak = pak_for(rdram, ctx->r4);
    if (pak == nullptr) {
        _return<int32_t>(ctx, PFS_ERR_NOPACK);
        return;
    }
    int index = find_note(rdram, *pak, (uint16_t)ctx->r5, (uint32_t)ctx->r6, ctx->r7, stack_arg(rdram, ctx, 4));
    if (index < 0) {
        _return<int32_t>(ctx, PFS_ERR_INVALID);
        return;
    }
    MEM_W(0, stack_arg(rdram, ctx, 5)) = index;
    _return<int32_t>(ctx, 0);
}

// s32 osPfsAllocateFile(OSPfs*, u16 company_code, u32 game_code, u8* game_name, u8* ext_name,
//                       int file_size_in_bytes, s32* file_no)
extern "C" void hwtr_osPfsAllocateFile(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ pak_mutex };
    ControllerPak* pak = pak_for(rdram, ctx->r4);
    if (pak == nullptr) {
        _return<int32_t>(ctx, PFS_ERR_NOPACK);
        return;
    }
    uint16_t company = (uint16_t)ctx->r5;
    uint32_t game_code = (uint32_t)ctx->r6;
    gpr name_addr = ctx->r7;
    gpr ext_addr = stack_arg(rdram, ctx, 4);
    int32_t size = (int32_t)stack_arg(rdram, ctx, 5);
    gpr file_no_addr = stack_arg(rdram, ctx, 6);

    if (company == 0 || game_code == 0 || size <= 0) {
        _return<int32_t>(ctx, PFS_ERR_INVALID);
        return;
    }

    size_t pages_needed = (size + page_size - 1) / page_size;

    int existing = find_note(rdram, *pak, company, game_code, name_addr, ext_addr);
    if (existing >= 0) {
        MEM_W(0, file_no_addr) = existing;
        _return<int32_t>(ctx, pak->chain(existing).size() == pages_needed ? 0 : PFS_ERR_EXIST);
        return;
    }

    if (pak->free_pages() < pages_needed) {
        _return<int32_t>(ctx, PFS_DATA_FULL);
        return;
    }

    int slot = -1;
    for (size_t i = 0; i < max_notes; i++) {
        if (!pak->note_used(i)) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        _return<int32_t>(ctx, PFS_DIR_FULL);
        return;
    }

    // Link free pages into a chain.
    std::vector<size_t> pages;
    for (size_t page = first_data_page; page < num_pages && pages.size() < pages_needed; page++) {
        if (pak->inode(page) == inode_free) {
            pages.push_back(page);
        }
    }
    for (size_t i = 0; i < pages.size(); i++) {
        pak->set_inode(pages[i], i + 1 < pages.size() ? (uint16_t)pages[i + 1] : inode_last_page);
        std::fill_n(&pak->data[pages[i] * page_size], page_size, 0);
    }
    pak->update_inode_checksum();

    uint8_t* n = pak->note(slot);
    std::fill_n(n, note_size, 0);
    ControllerPak::write32(n + 0, game_code);
    ControllerPak::write16(n + 4, company);
    ControllerPak::write16(n + 6, (uint16_t)pages[0]);
    n[8] = 0x02; // status: valid
    read_rdram(rdram, ext_addr, n + 0x0C, 4);
    read_rdram(rdram, name_addr, n + 0x10, 16);

    pak->save();
    MEM_W(0, file_no_addr) = slot;
    _return<int32_t>(ctx, 0);
}

// s32 osPfsDeleteFile(OSPfs*, u16 company_code, u32 game_code, u8* game_name, u8* ext_name)
extern "C" void hwtr_osPfsDeleteFile(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ pak_mutex };
    ControllerPak* pak = pak_for(rdram, ctx->r4);
    if (pak == nullptr) {
        _return<int32_t>(ctx, PFS_ERR_NOPACK);
        return;
    }
    int index = find_note(rdram, *pak, (uint16_t)ctx->r5, (uint32_t)ctx->r6, ctx->r7, stack_arg(rdram, ctx, 4));
    if (index < 0) {
        _return<int32_t>(ctx, PFS_ERR_INVALID);
        return;
    }
    for (size_t page : pak->chain(index)) {
        pak->set_inode(page, inode_free);
    }
    pak->update_inode_checksum();
    std::fill_n(pak->note(index), note_size, 0);
    pak->save();
    _return<int32_t>(ctx, 0);
}

// s32 osPfsReadWriteFile(OSPfs*, s32 file_no, u8 flag, int offset, int size_in_bytes, u8* data_buffer)
extern "C" void hwtr_osPfsReadWriteFile(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ pak_mutex };
    ControllerPak* pak = pak_for(rdram, ctx->r4);
    if (pak == nullptr) {
        _return<int32_t>(ctx, PFS_ERR_NOPACK);
        return;
    }
    int32_t file_no = (int32_t)ctx->r5;
    bool write = ((uint8_t)ctx->r6) == 1; // PFS_WRITE
    int32_t offset = (int32_t)ctx->r7;
    int32_t size = (int32_t)stack_arg(rdram, ctx, 4);
    gpr buffer = stack_arg(rdram, ctx, 5);

    if (file_no < 0 || file_no >= (int32_t)max_notes || !pak->note_used(file_no) ||
        offset < 0 || size < 0 || (offset % block_size) != 0 || (size % block_size) != 0) {
        _return<int32_t>(ctx, PFS_ERR_INVALID);
        return;
    }

    std::vector<size_t> pages = pak->chain(file_no);
    if ((size_t)(offset + size) > pages.size() * page_size) {
        _return<int32_t>(ctx, PFS_ERR_INVALID);
        return;
    }

    for (int32_t done = 0; done < size; ) {
        size_t file_pos = (size_t)(offset + done);
        size_t page = pages[file_pos / page_size];
        size_t in_page = file_pos % page_size;
        size_t count = std::min<size_t>(page_size - in_page, (size_t)(size - done));
        uint8_t* pak_ptr = &pak->data[page * page_size + in_page];
        if (write) {
            read_rdram(rdram, buffer + done, pak_ptr, count);
        }
        else {
            write_rdram(rdram, buffer + done, pak_ptr, count);
        }
        done += (int32_t)count;
    }

    if (write) {
        pak->save();
    }
    _return<int32_t>(ctx, 0);
}

// s32 osPfsFileState(OSPfs*, s32 file_no, OSPfsState* state)
extern "C" void hwtr_osPfsFileState(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ pak_mutex };
    ControllerPak* pak = pak_for(rdram, ctx->r4);
    if (pak == nullptr) {
        _return<int32_t>(ctx, PFS_ERR_NOPACK);
        return;
    }
    int32_t file_no = (int32_t)ctx->r5;
    gpr state = ctx->r6;
    if (file_no < 0 || file_no >= (int32_t)max_notes || !pak->note_used(file_no)) {
        _return<int32_t>(ctx, PFS_ERR_INVALID);
        return;
    }
    const uint8_t* n = pak->note(file_no);
    // OSPfsState: u32 file_size, u32 game_code, u16 company_code, char ext_name[4], char game_name[16]
    MEM_W(0x0, state) = (int32_t)(pak->chain(file_no).size() * page_size);
    MEM_W(0x4, state) = (int32_t)ControllerPak::read32(n + 0);
    MEM_HU(0x8, state) = ControllerPak::read16(n + 4);
    write_rdram(rdram, state + 0x0A, n + 0x0C, 4);
    write_rdram(rdram, state + 0x0E, n + 0x10, 16);
    _return<int32_t>(ctx, 0);
}
