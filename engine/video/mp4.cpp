#include "engine/video/mp4.hpp"

#include "engine/core/file.hpp"

#include <cstring>

namespace blocky {
namespace {

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

void putU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void putTag(std::vector<uint8_t>& out, const char tag[4]) {
    out.insert(out.end(), tag, tag + 4);
}

// A box is a size followed by a tag, and the size includes both. It is not
// known until the contents are written, so it goes down as a placeholder and
// is patched when the box closes.
size_t openBox(std::vector<uint8_t>& out, const char tag[4]) {
    const size_t start = out.size();
    putU32(out, 0);
    putTag(out, tag);
    return start;
}

void closeBox(std::vector<uint8_t>& out, size_t start) {
    const uint32_t size = uint32_t(out.size() - start);
    out[start + 0] = uint8_t(size >> 24);
    out[start + 1] = uint8_t(size >> 16);
    out[start + 2] = uint8_t(size >> 8);
    out[start + 3] = uint8_t(size);
}

// A full box carries a version byte and three flag bytes before its contents.
size_t openFullBox(std::vector<uint8_t>& out, const char tag[4], uint8_t version, uint32_t flags) {
    const size_t start = openBox(out, tag);
    putU32(out, (uint32_t(version) << 24) | (flags & 0x00FFFFFFu));
    return start;
}

// The identity display matrix, in 16.16 fixed point except the last which is
// 2.30. Every video track carries one and almost none of them mean anything.
void putUnityMatrix(std::vector<uint8_t>& out) {
    putU32(out, 0x00010000); putU32(out, 0); putU32(out, 0);
    putU32(out, 0); putU32(out, 0x00010000); putU32(out, 0);
    putU32(out, 0); putU32(out, 0); putU32(out, 0x40000000);
}

std::vector<uint8_t> buildFtyp() {
    std::vector<uint8_t> out;
    const size_t box = openBox(out, "ftyp");
    putTag(out, "isom");
    putU32(out, 512);
    putTag(out, "isom");
    putTag(out, "iso2");
    putTag(out, "avc1");
    putTag(out, "mp41");
    closeBox(out, box);
    return out;
}

} // namespace

bool Mp4Writer::begin(int width, int height, uint32_t timescale,
                      const std::vector<uint8_t>& avcc, std::string* error) {
    if (width <= 0 || height <= 0) {
        setError(error, "mp4: refusing to start a track with no size");
        return false;
    }
    if (timescale == 0) {
        setError(error, "mp4: timescale of zero");
        return false;
    }
    if (avcc.empty()) {
        setError(error, "mp4: no decoder configuration -- a player cannot start without it");
        return false;
    }

    mdat_.clear();
    sizes_.clear();
    durations_.clear();
    offsets_.clear();
    avcc_ = avcc;
    width_ = width;
    height_ = height;
    timescale_ = timescale;
    started_ = true;
    return true;
}

void Mp4Writer::addSample(const std::vector<uint8_t>& sample, uint32_t duration,
                          bool syncSample) {
    if (!started_ || sample.empty()) return;
    offsets_.push_back(uint32_t(mdat_.size()));
    sizes_.push_back(uint32_t(sample.size()));
    durations_.push_back(duration);
    sync_.push_back(syncSample ? 1 : 0);
    mdat_.insert(mdat_.end(), sample.begin(), sample.end());
}

bool Mp4Writer::save(const std::string& path, std::string* error) {
    if (!started_ || sizes_.empty()) {
        setError(error, "mp4: refusing to write a track with no samples");
        return false;
    }

    const std::vector<uint8_t> ftyp = buildFtyp();

    // A 32-bit box size cannot describe more than four gigabytes, and the
    // sample offsets in stco are 32-bit too. Both have 64-bit forms; neither
    // is worth writing until something needs them, so the limit is a refusal
    // rather than a silent truncation.
    const uint64_t mdatStart = uint64_t(ftyp.size());
    const uint64_t mdatSize = 8ull + uint64_t(mdat_.size());
    if (mdatStart + mdatSize > 0xFFFFFFFFull) {
        setError(error, "mp4: the movie is over four gigabytes, which needs the 64-bit boxes");
        return false;
    }

    uint64_t total = 0;
    for (uint32_t d : durations_) total += d;

    // ---- moov
    std::vector<uint8_t> moov;
    const size_t moovBox = openBox(moov, "moov");

    {
        const size_t mvhd = openFullBox(moov, "mvhd", 0, 0);
        putU32(moov, 0);                    // creation time
        putU32(moov, 0);                    // modification time
        putU32(moov, timescale_);
        putU32(moov, uint32_t(total));
        putU32(moov, 0x00010000);           // rate 1.0
        putU16(moov, 0x0100);               // volume 1.0
        putU16(moov, 0);                    // reserved
        putU32(moov, 0); putU32(moov, 0);   // reserved
        putUnityMatrix(moov);
        for (int i = 0; i < 6; ++i) putU32(moov, 0);   // pre_defined
        putU32(moov, 2);                    // next track id
        closeBox(moov, mvhd);
    }

    const size_t trak = openBox(moov, "trak");
    {
        // enabled | in movie | in preview
        const size_t tkhd = openFullBox(moov, "tkhd", 0, 0x000007);
        putU32(moov, 0);                    // creation
        putU32(moov, 0);                    // modification
        putU32(moov, 1);                    // track id
        putU32(moov, 0);                    // reserved
        putU32(moov, uint32_t(total));
        putU32(moov, 0); putU32(moov, 0);   // reserved
        putU16(moov, 0);                    // layer
        putU16(moov, 0);                    // alternate group
        putU16(moov, 0);                    // volume: zero for video
        putU16(moov, 0);                    // reserved
        putUnityMatrix(moov);
        putU32(moov, uint32_t(width_) << 16);
        putU32(moov, uint32_t(height_) << 16);
        closeBox(moov, tkhd);
    }

    const size_t mdia = openBox(moov, "mdia");
    {
        const size_t mdhd = openFullBox(moov, "mdhd", 0, 0);
        putU32(moov, 0);
        putU32(moov, 0);
        putU32(moov, timescale_);
        putU32(moov, uint32_t(total));
        putU16(moov, 0x55C4);               // language: 'und'
        putU16(moov, 0);                    // pre_defined
        closeBox(moov, mdhd);

        const size_t hdlr = openFullBox(moov, "hdlr", 0, 0);
        putU32(moov, 0);                    // pre_defined
        putTag(moov, "vide");
        putU32(moov, 0); putU32(moov, 0); putU32(moov, 0);   // reserved
        const char name[] = "BlockyEngine video";
        moov.insert(moov.end(), name, name + sizeof(name));  // includes the terminator
        closeBox(moov, hdlr);
    }

    const size_t minf = openBox(moov, "minf");
    {
        const size_t vmhd = openFullBox(moov, "vmhd", 0, 1);
        putU16(moov, 0);                                     // graphics mode: copy
        putU16(moov, 0); putU16(moov, 0); putU16(moov, 0);   // opcolour
        closeBox(moov, vmhd);

        const size_t dinf = openBox(moov, "dinf");
        const size_t dref = openFullBox(moov, "dref", 0, 0);
        putU32(moov, 1);                                     // one entry
        const size_t url = openFullBox(moov, "url ", 0, 1);  // flag 1: data is in this file
        closeBox(moov, url);
        closeBox(moov, dref);
        closeBox(moov, dinf);
    }

    const size_t stbl = openBox(moov, "stbl");
    {
        const size_t stsd = openFullBox(moov, "stsd", 0, 0);
        putU32(moov, 1);                    // one sample description

        const size_t avc1 = openBox(moov, "avc1");
        for (int i = 0; i < 6; ++i) moov.push_back(0);   // reserved
        putU16(moov, 1);                    // data reference index
        putU16(moov, 0);                    // pre_defined
        putU16(moov, 0);                    // reserved
        putU32(moov, 0); putU32(moov, 0); putU32(moov, 0);   // pre_defined
        putU16(moov, uint16_t(width_));
        putU16(moov, uint16_t(height_));
        putU32(moov, 0x00480000);           // 72 dpi horizontal
        putU32(moov, 0x00480000);           // 72 dpi vertical
        putU32(moov, 0);                    // reserved
        putU16(moov, 1);                    // frames per sample
        // compressorname: one length byte then 31 bytes of space
        moov.push_back(0);
        for (int i = 0; i < 31; ++i) moov.push_back(0);
        putU16(moov, 0x0018);               // depth: colour, no alpha
        putU16(moov, 0xFFFF);               // pre_defined: -1

        const size_t avcC = openBox(moov, "avcC");
        moov.insert(moov.end(), avcc_.begin(), avcc_.end());
        closeBox(moov, avcC);

        closeBox(moov, avc1);
        closeBox(moov, stsd);
    }

    {
        // Time to sample, run-length encoded. This is where a held frame
        // becomes one sample that lasts twice as long.
        std::vector<std::pair<uint32_t, uint32_t>> runs;   // count, delta
        for (uint32_t d : durations_) {
            if (!runs.empty() && runs.back().second == d) ++runs.back().first;
            else runs.push_back({1u, d});
        }

        const size_t stts = openFullBox(moov, "stts", 0, 0);
        putU32(moov, uint32_t(runs.size()));
        for (const auto& run : runs) {
            putU32(moov, run.first);
            putU32(moov, run.second);
        }
        closeBox(moov, stts);
    }

    {
        // Every sample is its own chunk, which makes the offset table a
        // direct list and removes a whole class of arithmetic mistake.
        const size_t stsc = openFullBox(moov, "stsc", 0, 0);
        putU32(moov, 1);
        putU32(moov, 1);   // first chunk
        putU32(moov, 1);   // samples per chunk
        putU32(moov, 1);   // sample description index
        closeBox(moov, stsc);

        const size_t stsz = openFullBox(moov, "stsz", 0, 0);
        putU32(moov, 0);   // sizes vary, so they are listed
        putU32(moov, uint32_t(sizes_.size()));
        for (uint32_t size : sizes_) putU32(moov, size);
        closeBox(moov, stsz);

        const size_t stco = openFullBox(moov, "stco", 0, 0);
        putU32(moov, uint32_t(offsets_.size()));
        for (uint32_t offset : offsets_) {
            putU32(moov, uint32_t(mdatStart + 8ull + uint64_t(offset)));
        }
        closeBox(moov, stco);

        // The samples a player may seek to. An all-intra track lists every
        // one of them, which is why this box used to be a formality; with P
        // frames it lists the IDRs and nothing else.
        //
        // Sample numbers here are one based. Off by one and a scrub lands the
        // decoder in the middle of a run of P frames, predicting from a
        // picture it never saw -- which looks like a corrupt file rather than
        // a wrong table.
        const size_t stss = openFullBox(moov, "stss", 0, 0);
        uint32_t syncCount = 0;
        for (uint8_t s : sync_) syncCount += s ? 1u : 0u;
        putU32(moov, syncCount);
        for (size_t i = 0; i < sync_.size(); ++i) {
            if (sync_[i]) putU32(moov, uint32_t(i + 1));
        }
        closeBox(moov, stss);
    }

    closeBox(moov, stbl);
    closeBox(moov, minf);
    closeBox(moov, mdia);
    closeBox(moov, trak);
    closeBox(moov, moovBox);

    // ---- the file: header, payload, then the table that points into it
    std::vector<uint8_t> file;
    file.reserve(ftyp.size() + size_t(mdatSize) + moov.size());
    file.insert(file.end(), ftyp.begin(), ftyp.end());
    putU32(file, uint32_t(mdatSize));
    putTag(file, "mdat");
    file.insert(file.end(), mdat_.begin(), mdat_.end());
    file.insert(file.end(), moov.begin(), moov.end());

    if (!createParentDirectories(path)) {
        setError(error, "mp4: cannot create directory for " + path);
        return false;
    }
    return writeFileBytes(path, file.data(), file.size(), error);
}

} // namespace blocky
