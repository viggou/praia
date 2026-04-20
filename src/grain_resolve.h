#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// Check if a directory is a grain with a grain.yaml.
// Returns the resolved entry file path, or empty string if not a grain directory.
inline std::string resolveGrainDir(const fs::path& dir) {
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
                    if (fs::exists(entry)) return fs::canonical(entry).string();
                }
            }
        }
    }

    // Fallback: index.praia
    auto indexFile = dir / "index.praia";
    if (fs::exists(indexFile)) return fs::canonical(indexFile).string();

    return "";
}

// Try to resolve a grain name in a given base directory.
// Checks <base>/<name>.praia first, then <base>/<name>/ as a directory grain.
inline std::string tryResolveGrain(const fs::path& base, const std::string& name) {
    // Single file
    auto singleFile = base / (name + ".praia");
    if (fs::exists(singleFile)) return fs::canonical(singleFile).string();

    // Directory grain
    auto grainDir = base / name;
    return resolveGrainDir(grainDir);
}
