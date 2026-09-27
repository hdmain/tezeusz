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

// Extract a ZIP produced by zipDirectory (STORE only; ZIP64 OK). Streams large files.
inline bool unzipToDirectory(const fs::path& zipPath, const fs::path& destDir, std::string* err) {
    std::ifstream in(zipPath.string(), std::ios::binary);
    if (!in) {
        if (err) *err = "cannot open zip";
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto fileSize = (uint64_t)in.tellg();
    if (fileSize < 22) {
        if (err) *err = "zip too small";
        return false;
    }

    auto readAt = [&](uint64_t off, char* buf, size_t n) -> bool {
        in.clear();
        in.seekg((std::streamoff)off);
        in.read(buf, (std::streamsize)n);
        return (size_t)in.gcount() == n;
    };
    auto u16 = [](const char* p) -> uint16_t {
        return (uint16_t)((uint8_t)p[0] | ((uint16_t)(uint8_t)p[1] << 8));
    };
    auto u32 = [](const char* p) -> uint32_t {
        return (uint32_t)(uint8_t)p[0] | ((uint32_t)(uint8_t)p[1] << 8) |
               ((uint32_t)(uint8_t)p[2] << 16) | ((uint32_t)(uint8_t)p[3] << 24);
    };
    auto u64 = [&](const char* p) -> uint64_t {
        return (uint64_t)u32(p) | ((uint64_t)u32(p + 4) << 32);
    };

    // Find EOCD (last 64 KiB search window).
    const uint64_t scan = std::min<uint64_t>(fileSize, 65536 + 22);
    std::vector<char> tail((size_t)scan);
    if (!readAt(fileSize - scan, tail.data(), (size_t)scan)) {
        if (err) *err = "read eocd failed";
        return false;
    }
    int64_t eocdRel = -1;
    for (int64_t i = (int64_t)scan - 22; i >= 0; --i) {
        if (u32(&tail[(size_t)i]) == 0x06054b50u) { eocdRel = i; break; }
    }
    if (eocdRel < 0) {
        if (err) *err = "eocd not found";
        return false;
    }
    const char* eocd = &tail[(size_t)eocdRel];
    uint64_t cdSize = u32(eocd + 12);
    uint64_t cdOffset = u32(eocd + 16);
    uint64_t entryCount = u16(eocd + 10);

    if (cdOffset == 0xffffffffu || cdSize == 0xffffffffu || entryCount == 0xffffu) {
        // ZIP64 end locator sits before EOCD
        if (eocdRel < 20) {
            if (err) *err = "zip64 locator missing";
            return false;
        }
        const char* loc = eocd - 20;
        if (u32(loc) != 0x07064b50u) {
            if (err) *err = "zip64 locator missing";
            return false;
        }
        const uint64_t zip64EocdOff = u64(loc + 8);
        char z64[56];
        if (!readAt(zip64EocdOff, z64, 56) || u32(z64) != 0x06064b50u) {
            if (err) *err = "zip64 eocd missing";
            return false;
        }
        entryCount = u64(z64 + 32);
        cdSize = u64(z64 + 40);
        cdOffset = u64(z64 + 48);
    }

    std::vector<char> cd((size_t)cdSize);
    if (!readAt(cdOffset, cd.data(), (size_t)cdSize)) {
        if (err) *err = "central directory read failed";
        return false;
    }

    std::error_code ec;
    fs::create_directories(destDir, ec);
    std::vector<uint8_t> buf(1 << 20);
    size_t pos = 0;
    size_t extracted = 0;

    for (uint64_t n = 0; n < entryCount; ++n) {
        if (pos + 46 > cd.size() || u32(&cd[pos]) != 0x02014b50u) {
            if (err) *err = "bad central directory entry";
            return false;
        }
        const uint16_t method = u16(&cd[pos + 10]);
        uint64_t compSize = u32(&cd[pos + 20]);
        uint64_t uncompSize = u32(&cd[pos + 24]);
        const uint16_t nameLen = u16(&cd[pos + 28]);
        const uint16_t extraLen = u16(&cd[pos + 30]);
        const uint16_t commentLen = u16(&cd[pos + 32]);
        uint64_t localOff = u32(&cd[pos + 42]);
        if (pos + 46 + nameLen > cd.size()) {
            if (err) *err = "bad name length";
            return false;
        }
        std::string name(&cd[pos + 46], &cd[pos + 46] + nameLen);

        // ZIP64 extra in central directory
        const char* extra = &cd[pos + 46 + nameLen];
        size_t ex = 0;
        while (ex + 4 <= extraLen) {
            uint16_t id = u16(extra + ex);
            uint16_t sz = u16(extra + ex + 2);
            if (ex + 4 + sz > extraLen) break;
            if (id == 0x0001) {
                size_t o = 0;
                if (uncompSize == 0xffffffffu && o + 8 <= sz) { uncompSize = u64(extra + ex + 4 + o); o += 8; }
                if (compSize == 0xffffffffu && o + 8 <= sz) { compSize = u64(extra + ex + 4 + o); o += 8; }
                if (o + 8 <= sz && localOff == 0xffffffffu) localOff = u64(extra + ex + 4 + o);
            }
            ex += 4 + sz;
        }

        pos += 46 + nameLen + extraLen + commentLen;

        if (name.empty() || name.back() == '/') continue; // directory
        if (name.find("..") != std::string::npos) {
            if (err) *err = "unsafe path in zip";
            return false;
        }
        if (method != 0) {
            if (err) *err = "unsupported compression (need STORE)";
            return false;
        }

        char local[30];
        if (!readAt(localOff, local, 30) || u32(local) != 0x04034b50u) {
            if (err) *err = "bad local header: " + name;
            return false;
        }
        const uint16_t lName = u16(local + 26);
        const uint16_t lExtra = u16(local + 28);
        uint64_t dataOff = localOff + 30 + lName + lExtra;

        fs::path outPath = destDir / fs::path(name);
        fs::create_directories(outPath.parent_path(), ec);
        std::ofstream out(outPath.string(), std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) *err = "cannot write: " + name;
            return false;
        }
        uint64_t left = uncompSize;
        in.clear();
        in.seekg((std::streamoff)dataOff);
        while (left) {
            const size_t want = (size_t)std::min<uint64_t>(buf.size(), left);
            in.read((char*)buf.data(), (std::streamsize)want);
            const auto got = (size_t)in.gcount();
            if (!got) {
                out.close();
                if (err) *err = "truncated read: " + name;
                return false;
            }
            out.write((const char*)buf.data(), (std::streamsize)got);
            left -= got;
        }
        out.close();
        ++extracted;
    }

    if (!extracted) {
        if (err) *err = "zip empty";
        return false;
    }
    return true;
}

} // namespace zipwrite
