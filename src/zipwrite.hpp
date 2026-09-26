#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <zlib.h>

// Minimal ZIP writer for library archives. Streams files (STORE) so multi‑GB
// videos never sit fully in RAM; ZIP64 when sizes/offsets exceed 4 GiB.
namespace zipwrite {
namespace fs = std::filesystem;

namespace {

void put16(std::ostream& o, uint16_t v) {
    char b[2] = { (char)(v & 0xff), (char)((v >> 8) & 0xff) };
    o.write(b, 2);
}
void put32(std::ostream& o, uint32_t v) {
    char b[4] = {
        (char)(v & 0xff), (char)((v >> 8) & 0xff),
        (char)((v >> 16) & 0xff), (char)((v >> 24) & 0xff)
    };
    o.write(b, 4);
}
void put64(std::ostream& o, uint64_t v) {
    put32(o, (uint32_t)(v & 0xffffffffu));
    put32(o, (uint32_t)(v >> 32));
}

struct Entry {
    std::string name;
    uint32_t crc = 0;
    uint64_t size = 0;
    uint64_t localOffset = 0;
    bool zip64 = false;
};

} // anon

inline bool zipDirectory(const fs::path& folder, const fs::path& zipPath, std::string* err) {
    std::error_code ec;
    if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec)) {
        if (err) *err = "folder missing";
        return false;
    }
    fs::create_directories(zipPath.parent_path(), ec);
    std::ofstream out(zipPath.string(), std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) *err = "cannot create zip";
        return false;
    }

    std::vector<Entry> entries;
    auto folderAbs = fs::weakly_canonical(folder, ec);
    if (ec) folderAbs = fs::absolute(folder, ec);

    std::vector<uint8_t> buf(1 << 20); // 1 MiB stream buffer
    bool anyZip64 = false;

    for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;

        fs::path rel = fs::relative(it->path(), folderAbs, ec);
        if (ec) {
            rel = it->path().filename();
            ec.clear();
        }
        std::string name = rel.generic_string();
        if (name.empty() || name == ".") continue;

        auto fileSize = it->file_size(ec);
        if (ec) { ec.clear(); continue; }

        std::ifstream in(it->path().string(), std::ios::binary);
        if (!in) continue;

        const bool entryZip64 = fileSize >= 0xffffffffull || (uint64_t)out.tellp() >= 0xffffffffull;
        anyZip64 = anyZip64 || entryZip64;

        Entry e;
        e.name = name;
        e.size = (uint64_t)fileSize;
        e.localOffset = (uint64_t)out.tellp();
        e.zip64 = entryZip64;

        // Local file header (CRC patched after streaming).
        const auto crcPos = e.localOffset + 14;
        put32(out, 0x04034b50);
        put16(out, entryZip64 ? 45 : 20);
        put16(out, 0); // flags
        put16(out, 0); // STORE
        put16(out, 0); // time
        put16(out, 0); // date
        put32(out, 0); // crc placeholder
        if (entryZip64) {
            put32(out, 0xffffffffu);
            put32(out, 0xffffffffu);
        } else {
            put32(out, (uint32_t)fileSize);
            put32(out, (uint32_t)fileSize);
        }
        put16(out, (uint16_t)name.size());
        uint16_t extraLen = entryZip64 ? 20 : 0;
        put16(out, extraLen);
        out.write(name.data(), (std::streamsize)name.size());
        if (entryZip64) {
            put16(out, 0x0001);
            put16(out, 16);
            put64(out, fileSize);
            put64(out, fileSize);
        }

        uLong crc = crc32(0L, Z_NULL, 0);
        uint64_t written = 0;
        while (in && written < fileSize) {
            const size_t want = (size_t)std::min<uint64_t>(buf.size(), fileSize - written);
            in.read((char*)buf.data(), (std::streamsize)want);
            const auto got = (size_t)in.gcount();
            if (!got) break;
            crc = crc32(crc, buf.data(), (uInt)got);
            out.write((const char*)buf.data(), (std::streamsize)got);
            written += got;
        }
        if (written != fileSize) {
            out.close();
            fs::remove(zipPath, ec);
            if (err) *err = "read incomplete: " + name;
            return false;
        }
        e.crc = (uint32_t)crc;

        const auto resume = out.tellp();
        out.seekp((std::streamoff)crcPos);
        put32(out, e.crc);
        out.seekp(resume);

        entries.push_back(std::move(e));
    }

    if (entries.empty()) {
        out.close();
        fs::remove(zipPath, ec);
        if (err) *err = "nothing to archive";
        return false;
    }

    const uint64_t cdStart = (uint64_t)out.tellp();
    for (auto& e : entries) {
        const bool off64 = e.localOffset >= 0xffffffffull;
        const bool use64 = e.zip64 || off64;
        anyZip64 = anyZip64 || use64;

        put32(out, 0x02014b50);
        put16(out, use64 ? 45 : 20);
        put16(out, use64 ? 45 : 20);
        put16(out, 0);
        put16(out, 0); // STORE
        put16(out, 0);
        put16(out, 0);
        put32(out, e.crc);
        if (e.zip64) {
            put32(out, 0xffffffffu);
            put32(out, 0xffffffffu);
        } else {
            put32(out, (uint32_t)e.size);
            put32(out, (uint32_t)e.size);
        }
        put16(out, (uint16_t)e.name.size());
        uint16_t extraPayload = 0;
        if (e.zip64) extraPayload += 16; // uncomp + comp
        if (off64) extraPayload += 8;    // relative offset
        const uint16_t extraLen = extraPayload ? (uint16_t)(4 + extraPayload) : 0;
        put16(out, extraLen);
        put16(out, 0); // comment
        put16(out, 0); // disk
        put16(out, 0); // int attr
        put32(out, 0); // ext attr
        put32(out, off64 ? 0xffffffffu : (uint32_t)e.localOffset);
        out.write(e.name.data(), (std::streamsize)e.name.size());
        if (extraLen) {
            put16(out, 0x0001);
            put16(out, extraPayload);
            if (e.zip64) {
                put64(out, e.size);
                put64(out, e.size);
            }
            if (off64)
                put64(out, e.localOffset);
        }
    }
    const uint64_t cdSize = (uint64_t)out.tellp() - cdStart;
    const uint64_t entryCount = entries.size();
    anyZip64 = anyZip64 || cdStart >= 0xffffffffull || cdSize >= 0xffffffffull ||
               entryCount >= 0xffffu;

    if (anyZip64) {
        const uint64_t zip64EocdOffset = (uint64_t)out.tellp();
        put32(out, 0x06064b50);
        put64(out, 44); // size of zip64 EOCD remaining
        put16(out, 45);
        put16(out, 45);
        put32(out, 0);
        put32(out, 0);
        put64(out, entryCount);
        put64(out, entryCount);
        put64(out, cdSize);
        put64(out, cdStart);

        put32(out, 0x07064b50);
        put32(out, 0);
        put64(out, zip64EocdOffset);
        put32(out, 1);
    }

    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, entryCount >= 0xffffu ? 0xffff : (uint16_t)entryCount);
    put16(out, entryCount >= 0xffffu ? 0xffff : (uint16_t)entryCount);
    put32(out, cdSize >= 0xffffffffull ? 0xffffffffu : (uint32_t)cdSize);
    put32(out, cdStart >= 0xffffffffull ? 0xffffffffu : (uint32_t)cdStart);
    put16(out, 0);
    out.close();
    return true;
}

} // namespace zipwrite
