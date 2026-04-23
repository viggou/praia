#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// Directory where the praia binary lives — set once from main().
// Used to find bundled stdlib grains in development mode: <bindir>/grains/
inline std::string g_praiaInstallDir;

// Compile-time LIBDIR from make install (e.g. "/usr/local/lib/praia").
// When set, grains are found at PRAIA_LIBDIR/grains/.
// When empty, falls back to g_praiaInstallDir-relative resolution.
#ifdef PRAIA_LIBDIR
inline const char* g_praiaLibDir = PRAIA_LIBDIR;
#else
inline const char* g_praiaLibDir = nullptr;
#endif

// Verify that a resolved path stays within an expected base directory.
// Prevents symlink escapes (e.g. ext_grains/evil -> /etc/passwd).
inline std::string containedCanonical(const fs::path& file, const fs::path& base) {
    auto resolved = fs::canonical(file).string();
    auto baseStr = fs::canonical(base).string();
    // Ensure resolved path starts with base + separator (or equals base)
    if (resolved.rfind(baseStr, 0) != 0)
        return ""; // escaped containment
    // Must be exactly base or base/...
    if (resolved.size() > baseStr.size() && resolved[baseStr.size()] != '/')
        return ""; // e.g. base="/foo/bar", resolved="/foo/barBaz"
    return resolved;
}

// Check if a directory is a grain with a grain.yaml.
// Returns the resolved entry file path, or empty string if not a grain directory.
inline std::string resolveGrainDir(const fs::path& dir, const fs::path& base) {
    if (!fs::is_directory(dir)) return "";

    // Look for grain.yaml to find the main entry file
    auto manifest = dir / "grain.yaml";
    if (fs::exists(manifest)) {
        std::ifstream f(manifest);
        std::string line;
        while (std::getline(f, line)) {
            // Simple YAML parse: look for "main: <filename>"
            if (line.rfind("main:", 0) == 0) {
                std::string mainFile = line.substr(5);
                // Trim whitespace
                size_t start = mainFile.find_first_not_of(" \t");
                if (start != std::string::npos) mainFile = mainFile.substr(start);
                size_t end = mainFile.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) mainFile = mainFile.substr(0, end + 1);
                if (!mainFile.empty()) {
                    auto entry = dir / mainFile;
                    if (fs::exists(entry)) return containedCanonical(entry, base);
                }
            }
        }
    }

    // Fallback: main.praia, then index.praia
    auto mainFile = dir / "main.praia";
    if (fs::exists(mainFile)) return containedCanonical(mainFile, base);

    auto indexFile = dir / "index.praia";
    if (fs::exists(indexFile)) return containedCanonical(indexFile, base);

    return "";
}

// Try to resolve a grain name in a given base directory.
// Checks <base>/<name>.praia first, then <base>/<name>/ as a directory grain.
inline std::string tryResolveGrain(const fs::path& base, const std::string& name) {
    // Single file
    auto singleFile = base / (name + ".praia");
    if (fs::exists(singleFile)) return containedCanonical(singleFile, base);

    // Directory grain
    auto grainDir = base / name;
    return resolveGrainDir(grainDir, base);
}
