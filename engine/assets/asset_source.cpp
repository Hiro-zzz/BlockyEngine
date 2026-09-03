#include "engine/assets/asset_source.hpp"

#include "engine/core/file.hpp"

#include <algorithm>
#include <cstdlib>

namespace blocky {
namespace {

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

std::string joinPath(const std::string& root, const std::string& relative) {
    if (root.empty()) return relative;
    if (root.back() == '/' || root.back() == '\\') return root + relative;
    return root + "/" + relative;
}

// Compare version folder names numerically, so 1.9 sorts below 1.20.
int compareVersions(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        int left = 0, right = 0;
        bool leftNumeric = false, rightNumeric = false;

        while (i < a.size() && a[i] >= '0' && a[i] <= '9') {
            left = left * 10 + (a[i++] - '0');
            leftNumeric = true;
        }
        while (j < b.size() && b[j] >= '0' && b[j] <= '9') {
            right = right * 10 + (b[j++] - '0');
            rightNumeric = true;
        }

        if (leftNumeric || rightNumeric) {
            if (left != right) return left < right ? -1 : 1;
        }
        // Skip one separator on each side and continue.
        if (i < a.size()) ++i;
        if (j < b.size()) ++j;
        if (!leftNumeric && !rightNumeric) break;
    }
    return 0;
}

void collectVersionJars(const std::string& versionsRoot, std::vector<std::string>& versions,
                        std::vector<std::string>& jars) {
    std::vector<DirEntry> entries;
    if (!listDirectory(versionsRoot, entries)) return;

    for (const DirEntry& entry : entries) {
        if (!entry.isDirectory) continue;
        // A version folder holds <name>.jar alongside <name>.json.
        std::string jar = joinPath(joinPath(versionsRoot, entry.name), entry.name + ".jar");
        if (!fileExists(jar)) continue;

        // Modded profiles reuse a vanilla jar and add loader classes; the
        // vanilla one is the cleaner source of textures.
        std::string lowered = entry.name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (lowered.find("optifine") != std::string::npos ||
            lowered.find("forge") != std::string::npos ||
            lowered.find("fabric") != std::string::npos) {
            continue;
        }

        versions.push_back(entry.name);
        jars.push_back(jar);
    }
}

} // namespace

void AssetSource::close() {
    kind_ = Kind::None;
    root_.clear();
    description_.clear();
    archive_.reset();
}

bool AssetSource::openDirectory(const std::string& utf8Path, std::string* error) {
    close();
    if (!isDirectory(utf8Path)) {
        setError(error, "asset source: not a directory: " + utf8Path);
        return false;
    }
    kind_ = Kind::Directory;
    root_ = utf8Path;
    description_ = "directory " + utf8Path;
    return true;
}

bool AssetSource::openArchive(const std::string& utf8Path, std::string* error) {
    close();
    auto archive = std::make_unique<ZipArchive>();
    if (!archive->openFile(utf8Path, error)) return false;

    description_ = "archive " + utf8Path + " (" + std::to_string(archive->entryCount()) + " entries)";
    archive_ = std::move(archive);
    kind_ = Kind::Archive;
    return true;
}

bool AssetSource::open(const std::string& utf8Path, std::string* error) {
    if (isDirectory(utf8Path)) return openDirectory(utf8Path, error);
    return openArchive(utf8Path, error);
}

bool AssetSource::exists(const std::string& path) const {
    switch (kind_) {
        case Kind::Directory: return fileExists(joinPath(root_, path));
        case Kind::Archive:   return archive_->contains(path);
        default:              return false;
    }
}

bool AssetSource::read(const std::string& path, std::vector<uint8_t>& out, std::string* error) const {
    switch (kind_) {
        case Kind::Directory: return readFileBytes(joinPath(root_, path), out, error);
        case Kind::Archive:   return archive_->read(path, out, error);
        default:
            setError(error, "asset source: nothing is open");
            return false;
    }
}

std::vector<std::string> AssetSource::list(const std::string& prefix) const {
    std::vector<std::string> result;

    if (kind_ == Kind::Archive) {
        for (const std::string& name : archive_->names()) {
            if (name.rfind(prefix, 0) == 0) result.push_back(name);
        }
        return result;
    }
    if (kind_ != Kind::Directory) return result;

    // Walk the directory tree under the prefix, returning prefix-relative
    // paths so both source kinds answer in the same currency.
    std::vector<std::string> pending{prefix};
    while (!pending.empty()) {
        std::string relative = pending.back();
        pending.pop_back();

        std::vector<DirEntry> entries;
        if (!listDirectory(joinPath(root_, relative), entries)) continue;

        for (const DirEntry& entry : entries) {
            std::string child = relative.empty() ? entry.name : relative + "/" + entry.name;
            if (entry.isDirectory) {
                pending.push_back(child);
            } else {
                result.push_back(child);
            }
        }
    }
    return result;
}

std::string AssetSource::findClientJar() {
    const char* appData = std::getenv("APPDATA");
    if (!appData) return {};
    std::string base(appData);

    const std::string candidates[] = {
        base + "/.minecraft/versions",
        base + "/.tlauncher/legacy/Minecraft/game/versions",
        base + "/PrismLauncher/instances",
    };

    std::vector<std::string> versions, jars;
    for (const std::string& root : candidates) {
        collectVersionJars(root, versions, jars);
    }
    if (jars.empty()) return {};

    size_t best = 0;
    for (size_t i = 1; i < jars.size(); ++i) {
        if (compareVersions(versions[i], versions[best]) > 0) best = i;
    }
    return jars[best];
}

} // namespace blocky
