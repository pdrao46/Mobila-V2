// ============================================================================
//  MOBILADOR - tools/installer/payload_format.h
//
//  Layout of the self-extracting installer's payload.
//
//  The installer executable is a normal PE file with an opaque blob appended
//  after the last section.  The blob is a flat sequence of entries:
//
//      entry  :=  u32 path_len | u8 path[path_len] | u64 data_len | u8 data[]
//
//  and the last 24 bytes of the file are a footer that says where the blob
//  starts and how many entries it holds:
//
//      footer :=  u32 magic ('MOBL') | u32 version | u32 count
//              |  u64 payload_offset | u32 reserved
//
//  Two properties matter here and both are deliberate:
//
//    * nothing is compressed, so extraction is a straight copy - there is no
//      decompressor to get wrong, and the size on disk equals the size in the
//      installer (about 42 MB, dominated by adb.exe and the Android stubs);
//
//    * the footer is at the very end, so the *same* binary doubles as the
//      uninstaller: the installer writes a copy of itself truncated at
//      payload_offset, and that copy has no footer, which is exactly the
//      signal used to select uninstall mode.
//
//  The reader below is pure pointer arithmetic over a "read_at" callback, so
//  the identical code runs inside the Windows installer and inside the Linux
//  harness that checks the packing step (tools/installer/test_payload.cpp).
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

namespace mobinst {

static const uint32_t PACK_MAGIC    = 0x4C424F4Du;   // 'MOBL' little endian
static const uint32_t PACK_VERSION  = 1u;
static const uint32_t FOOTER_SIZE   = 24u;
static const uint32_t MAX_PATH_LEN  = 1024u;

// Reads exactly len bytes at absolute offset off. Returns false on short read.
typedef bool (*ReadAtFn)(void* ctx, uint64_t off, void* buf, uint32_t len);

// Identity of one payload entry, plus a cursor for streaming its bytes out.
struct PackedEntry {
    uint32_t index;         // 0-based position in the archive
    uint64_t entry_off;     // offset of path_len inside the payload blob
    uint64_t data_off;      // absolute file offset of the first data byte
    uint64_t data_len;
    uint32_t path_len;
};

struct PayloadReader {
    ReadAtFn read_at;
    void*    ctx;
    bool     valid;
    uint32_t count;
    uint64_t payload_off;
    uint64_t payload_end;    // payload_off + payload_size (start of the footer)

    PayloadReader() : read_at(0), ctx(0), valid(false), count(0),
                      payload_off(0), payload_end(0) {}

    // Locates the footer and validates it. Fails when the file is a plain
    // stub (the uninstaller copy) or when the footer is corrupt.
    bool open(uint64_t file_size) {
        valid = false;
        if (file_size < FOOTER_SIZE) return false;
        uint8_t foot[FOOTER_SIZE];
        if (!read_at(ctx, file_size - FOOTER_SIZE, foot, FOOTER_SIZE)) return false;
        uint32_t magic, version, n, reserved;
        uint64_t off;
        memcpy(&magic,   foot + 0, 4);
        memcpy(&version, foot + 4, 4);
        memcpy(&n,       foot + 8, 4);
        memcpy(&off,     foot + 12, 8);
        memcpy(&reserved,foot + 20, 4);
        (void)reserved;
        if (magic != PACK_MAGIC || version != PACK_VERSION) return false;
        if (off < 64 || off > file_size - FOOTER_SIZE) return false;
        if (n > 4096) return false;
        count       = n;
        payload_off = off;
        payload_end = file_size - FOOTER_SIZE;
        valid       = true;
        return true;
    }

    // Walks to entry `index`. The walk costs one small read per skipped entry,
    // which is irrelevant for the few dozen files an install touches.
    bool entry(uint32_t index, PackedEntry* out) const {
        if (!valid || index >= count || !out) return false;
        uint64_t cur = payload_off;
        for (uint32_t i = 0; i <= index; ++i) {
            uint32_t path_len = 0;
            if (cur + 4 > payload_end) return false;
            if (!read_at(ctx, cur, &path_len, 4)) return false;
            if (path_len == 0 || path_len > MAX_PATH_LEN) return false;
            uint64_t path_off = cur + 4;
            if (path_off + path_len + 8 > payload_end) return false;
            uint64_t data_len = 0;
            if (!read_at(ctx, path_off + path_len, &data_len, 8)) return false;
            uint64_t data_off = path_off + path_len + 8;
            if (data_off + data_len > payload_end) return false;
            if (i == index) {
                out->index     = index;
                out->entry_off = cur;
                out->path_len  = path_len;
                out->data_off  = data_off;
                out->data_len  = data_len;
                return true;
            }
            cur = data_off + data_len;
        }
        return false;
    }

    // Copies the (relative, backslash separated) path of an entry into buf.
    // buf must hold at least MAX_PATH_LEN bytes.
    bool entry_path(const PackedEntry& e, char* buf, uint32_t cap) const {
        if (e.path_len + 1 > cap) return false;
        return read_at(ctx, e.entry_off + 4, buf, e.path_len) &&
               (buf[e.path_len] = '\0', true);
    }
};

} // namespace mobinst
