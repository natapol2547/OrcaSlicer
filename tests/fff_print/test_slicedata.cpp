#include <catch2/catch_all.hpp>
#include "libslic3r/Print.hpp"
#include "test_data.hpp"
#include <boost/filesystem.hpp>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace Slic3r;
using nlohmann::json;

namespace {
json* first_path(json& value)
{
    if (value.is_object() && value.contains("polyline"))
        return &value["polyline"];
    if (value.is_structured())
        for (auto& child : value)
            if (auto* path = first_path(child))
                return path;
    return nullptr;
}

struct CacheDirectory {
    boost::filesystem::path path = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orca-slicedata-%%%%-%%%%");
    CacheDirectory() { boost::filesystem::create_directory(path); }
    ~CacheDirectory() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
};
}

TEST_CASE("Sliced-data preserves XYZ and arc fits", "[Print][slicedata]")
{
    CacheDirectory directory;
    Print print;
    Test::init_and_process_print({Test::TestMesh::cube_20x20x20}, print, {
        {"layer_height", 0.25}, {"initial_layer_print_height", 0.25}
    });
    const auto input = directory.path / "input";
    REQUIRE(print.export_cached_data(input.string(), false) == 0);
    boost::filesystem::path object_file;
    for (const auto& entry : boost::filesystem::directory_iterator(input))
        if (entry.path().filename().string().find("obj_") == 0)
            object_file = entry.path();
    REQUIRE_FALSE(object_file.empty());
    json original;
    std::ifstream(object_file.string()) >> original;
    REQUIRE(first_path(original) != nullptr);
    REQUIRE(first_path(original)->at("version") == 1);

    const json straight = {{"start_index", 0}, {"end_index", 1}, {"path_type", 1}};
    const json arc = {{"is_arc", true}, {"length", 1570796.3267948966},
        {"angle_radians", 1.5707963267948966}, {"polar_start_theta", 0.0},
        {"polar_end_theta", 1.5707963267948966}, {"start_point", {1000000, 0}},
        {"end_point", {0, 1000000}}, {"direction", 1}, {"radius", 1000000.0},
        {"center", {0, 0}}};
    const json xyz = {{"version", 1}, {"points_xyz", {{1000000, 0, 17}, {0, 1000000, -19}}},
        {"arc_fitting", json::array()}};

    std::vector<json> cases = {
        xyz,
        {{"version", 1}, {"points_xyz", json::array()}, {"arc_fitting", json::array()}}
    };
    json linear = xyz;
    linear["arc_fitting"] = {straight};
    cases.push_back(linear);
    for (int direction : {1, 2}) {
        json fitted = xyz;
        json arc_data = arc;
        arc_data["direction"] = direction;
        if (direction == 2) {
            arc_data["end_point"] = {0, -1000000};
            arc_data["polar_end_theta"] = -1.5707963267948966;
            fitted["points_xyz"][1] = {0, -1000000, -19};
        }
        fitted["arc_fitting"] = {{{"start_index", 0}, {"end_index", 1},
            {"path_type", direction == 2 ? 2 : 3}, {"arc_data", arc_data}}};
        cases.push_back(fitted);
    }
    json legacy = {{"points", {1000000, 0, 0, 1000000}},
                   {"arc_fitting", cases[3]["arc_fitting"]}};
    cases.push_back(legacy);

    for (size_t i = 0; i < cases.size(); ++i) {
        INFO("path case " << i);
        json source = original;
        *first_path(source) = cases[i];
        { std::ofstream file(object_file.string()); file << source.dump(); }
        REQUIRE(print.load_cached_data(input.string()) == 0);
        const auto output = directory.path / ("output-" + std::to_string(i));
        REQUIRE(print.export_cached_data(output.string(), false) == 0);
        json roundtrip;
        std::ifstream((output / object_file.filename()).string()) >> roundtrip;
        json expected = cases[i];
        if (expected.contains("points")) {
            expected.erase("points");
            expected["version"] = 1;
            expected["points_xyz"] = {{1000000, 0, 0}, {0, 1000000, 0}};
        }
        REQUIRE(first_path(roundtrip) != nullptr);
        CHECK(*first_path(roundtrip) == expected);
    }

    json unknown = xyz;
    unknown["version"] = 999;
    json malformed = xyz;
    malformed["points_xyz"] = {{1, 2}};
    for (const json& bad : {unknown, malformed, json::array({{1, 2, 0}})}) {
        json source = original;
        *first_path(source) = bad;
        { std::ofstream file(object_file.string()); file << source.dump(); }
        CHECK(print.load_cached_data(input.string()) != 0);
    }
}
