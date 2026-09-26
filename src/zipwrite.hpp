#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <zlib.h>

// Minimal ZIP writer (DEFLATE) for archiving a library title folder.
namespace zipwrite {
namespace fs = std::filesystem;

namespace {

uint32_t crc32Bytes(const uint8_t* data, size_t n) {
    return (uint32_t)::crc32(0L, data, (uInt)n);
}

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

bool deflateRaw(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
    if (!out) return false;
    out->clear();
    if (in.empty()) return true;
    z_stream zs{};
    // negative windowBits = raw DEFLATE (ZIP)
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    zs.next_in = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    char buf[32768];
    int ret = Z_OK;
    do {
        zs.next_out = (Bytef*)buf;
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, zs.avail_in ? Z_NO_FLUSH : Z_FINISH);
        if (ret == Z_STREAM_ERROR) { deflateEnd(&zs); return false; }
        size_t have = sizeof(buf) - zs.avail_out;
        out->insert(out->end(), buf, buf + have);
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return true;
}

struct Entry {
    std::string name; // relative, forward slashes
    uint32_t crc = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint16_t method = 0; // 0=store 8=deflate
    uint32_t localOffset = 0;
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

        std::ifstream in(it->path().string(), std::ios::binary);
        if (!in) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        uint32_t crc = crc32Bytes(data.data(), data.size());
        std::vector<uint8_t> comp;
        uint16_t method = 8;
        if (!deflateRaw(data, &comp) || comp.size() >= data.size()) {
            comp = data;
            method = 0;
        }

        Entry e;
        e.name = name;
        e.crc = crc;
        e.compSize = (uint32_t)comp.size();
        e.uncompSize = (uint32_t)data.size();
        e.method = method;
        e.localOffset = (uint32_t)out.tellp();

        // local file header
        put32(out, 0x04034b50);
        put16(out, 20); // version needed
        put16(out, 0);  // flags
        put16(out, method);
        put16(out, 0);  // time
        put16(out, 0);  // date
        put32(out, crc);
        put32(out, e.compSize);
        put32(out, e.uncompSize);
        put16(out, (uint16_t)name.size());
        put16(out, 0); // extra
        out.write(name.data(), (std::streamsize)name.size());
        if (!comp.empty())
            out.write((const char*)comp.data(), (std::streamsize)comp.size());
        entries.push_back(std::move(e));
    }

    if (entries.empty()) {
        out.close();
        fs::remove(zipPath, ec);
        if (err) *err = "nothing to archive";
        return false;
    }

    uint32_t cdStart = (uint32_t)out.tellp();
    for (auto& e : entries) {
        put32(out, 0x02014b50);
        put16(out, 20); // version made by
        put16(out, 20); // version needed
        put16(out, 0);
        put16(out, e.method);
        put16(out, 0);
        put16(out, 0);
        put32(out, e.crc);
        put32(out, e.compSize);
        put32(out, e.uncompSize);
        put16(out, (uint16_t)e.name.size());
        put16(out, 0); // extra
        put16(out, 0); // comment
        put16(out, 0); // disk
        put16(out, 0); // int attr
        put32(out, 0); // ext attr
        put32(out, e.localOffset);
        out.write(e.name.data(), (std::streamsize)e.name.size());
    }
    uint32_t cdSize = (uint32_t)out.tellp() - cdStart;
    // end of central directory
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, (uint16_t)entries.size());
    put16(out, (uint16_t)entries.size());
    put32(out, cdSize);
    put32(out, cdStart);
    put16(out, 0); // comment
    out.close();
    return true;
}

} // namespace zipwrite
