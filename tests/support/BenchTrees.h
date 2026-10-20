#pragma once

#include "TempDir.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace arraw::test {

/// @brief Shape of a generated folder tree for the listing benchmarks.
struct BenchShape {
    /// @brief Name to report.
    std::string name;
    /// @brief Folders below the root that hold shots, as `parents` folders of `children` each;
    /// 0 parents for shots in the root itself.
    int parents = 0;
    /// @brief Folders in each parent.
    int children = 0;
    /// @brief Shots in each folder that holds any.
    int shots = 0;
    /// @brief Whether each shot has a sidecar too.
    bool sidecars = false;
};

/// @brief Gives the three shapes of the stall target (review 2026-10-20, B2).
///
/// 200 day folders of 250 RAW+JPEG+XMP; one flat folder of 50 000 RAW+JPEG; and 20 000 folders
/// (100 of 200) of two RAW+JPEG shots, 40 000 in all, as the review measured.
inline std::vector<BenchShape> benchShapes() {
    return {{"200 x 250", 10, 20, 250, true},
            {"1 x 50 000", 0, 0, 50'000, false},
            {"20 000 x 2", 100, 200, 2, false}};
}

/// @brief Writes a tree of a shape below a root, as empty files.
inline void generateBenchTree(const std::filesystem::path& root, const BenchShape& shape) {
    const auto fill = [&](const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        for (int shot = 1; shot <= shape.shots; ++shot) {
            const std::string stem = "DSC" + std::to_string(100000 + shot);
            for (const char* extension : {".ARW", ".JPG", ".ARW.xmp"}) {
                if (std::string(extension) == ".ARW.xmp" && !shape.sidecars) {
                    continue;
                }
                std::ofstream(folder / (stem + extension));
            }
        }
    };
    if (shape.parents == 0) {
        fill(root);
        return;
    }
    for (int parent = 0; parent < shape.parents; ++parent) {
        const std::filesystem::path parentFolder = root / std::to_string(2000 + parent);
        for (int child = 1; child <= shape.children; ++child) {
            fill(parentFolder / ("day " + std::to_string(child)));
        }
    }
}

/// @brief Gives the trees to measure: ARRAW_BENCH_TREE alone if set, else one generated tree of
/// each of ::arraw::test::benchShapes, kept in @p generated.
/// @return Name and root of each tree.
inline std::vector<std::pair<std::string, std::filesystem::path>>
benchTrees(std::vector<std::unique_ptr<TempDir>>& generated) {
    if (const char* tree = std::getenv("ARRAW_BENCH_TREE"); tree != nullptr && *tree != '\0') {
        return {{"ARRAW_BENCH_TREE", tree}};
    }
    std::vector<std::pair<std::string, std::filesystem::path>> trees;
    const std::vector<BenchShape> shapes = benchShapes();
    for (const BenchShape& shape : shapes) {
        generated.push_back(std::make_unique<TempDir>());
        generateBenchTree(generated.back()->path(), shape);
        trees.emplace_back(shape.name, generated.back()->path());
    }
    return trees;
}

} // namespace arraw::test
