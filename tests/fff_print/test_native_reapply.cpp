#include <catch2/catch_all.hpp>
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "test_data.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <set>

using namespace Slic3r;

// Explicit development probe: exercise the same importer/config assembly as
// desktop Orca without launching the GUI or modifying the production binary.
TEST_CASE("MakerScapes recipe desktop import probe", "[.recipe_import]")
{
    using json = nlohmann::json;
    namespace fs = boost::filesystem;
    const char *path = std::getenv("MS_RECIPE_IMPORT_MANIFEST");
    REQUIRE(path != nullptr);
    std::ifstream input(path);
    const json manifest = json::parse(input);
    REQUIRE(manifest.is_array());
    for (const auto &item : manifest) {
        const fs::path folder(item.at("folder").get<std::string>());
        json report = {{"imported", false}, {"substitutions", 0}, {"differences", json::object()}};
        try {
            const fs::path data = folder / "desktop";
            fs::create_directories(data);
            set_data_dir(data.string());
            PresetBundle bundle;
            const std::string user = (data / "user").string();
            fs::create_directories(user);
            bundle.printers.update_user_presets_directory(user, PRESET_PRINTER_NAME);
            bundle.prints.update_user_presets_directory(user, PRESET_PRINT_NAME);
            bundle.filaments.update_user_presets_directory(user, PRESET_FILAMENT_NAME);
            const std::vector<std::pair<std::string, std::string>> files = {
                {"machine", "machine.json"}, {"process", "process.json"}, {"filament", "filament1.json"}};
            PresetsConfigSubstitutions substitutions;
            bool imported = true;
            for (const auto &[role, filename] : files) {
                std::string file = (folder / filename).string();
                std::vector<std::string> result;
                int overwrite = 0;
                const bool ok = bundle.import_json_presets(substitutions, file,
                    [](const std::string &) { return 0; },
                    ForwardCompatibilitySubstitutionRule::Disable, overwrite, result);
                report["imports"][role] = {{"returned", ok}, {"saved", result.size()}};
                imported = imported && ok && result.size() == 1;
            }
            report["substitutions"] = substitutions.size();
            if (imported) {
                const auto name = [&](const char *role) { return item.at("names").at(role).get<std::string>(); };
                bundle.printers.select_preset_by_name(name("machine"), true);
                bundle.prints.select_preset_by_name(name("process"), true);
                bundle.filaments.select_preset_by_name(name("filament"), true);
                if (bundle.printers.get_edited_preset().name != name("machine") ||
                    bundle.prints.get_edited_preset().name != name("process") ||
                    bundle.filaments.get_edited_preset().name != name("filament"))
                    throw std::runtime_error("Imported preset not selectable by its exact name");
                bundle.filament_presets = {name("filament")};
                DynamicPrintConfig resolved = bundle.full_config();
                resolved.save((folder / "desktop-resolved.ini").string());
                DynamicPrintConfig reference;
                reference.load_from_ini((folder.parent_path() / "reference.ini").string(),
                                        ForwardCompatibilitySubstitutionRule::Disable);
                std::set<std::string> keys;
                for (const auto &key : reference.keys()) keys.insert(key);
                for (const auto &key : resolved.keys()) keys.insert(key);
                for (const auto &key : keys) {
                    const auto *before = reference.option(key);
                    const auto *after = resolved.option(key);
                    const json a = before ? json(before->serialize()) : json(nullptr);
                    const json b = after ? json(after->serialize()) : json(nullptr);
                    if (a != b) report["differences"][key] = {{"native", a}, {"desktop", b}};
                }
                report["imported"] = true;
            }
        } catch (const std::exception &error) {
            report["error"] = error.what();
        }
        std::ofstream output((folder / "report.json").string());
        output << report.dump(2) << '\n';
        REQUIRE(output.good());
    }
}

TEST_CASE("Default acceleration preserves geometry while layer height invalidates it",
          "[Print][native_reapply]")
{
    Print print;
    Test::init_and_process_print({Test::TestMesh::cube_20x20x20}, print, {
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"default_acceleration", 6000.0}
    });
    const Model model(print.model());
    DynamicPrintConfig config(print.full_print_config());
    const PrintObjectStep geometry[] = {
        posSlice, posPerimeters, posInfill, posSupportMaterial, posIroning
    };
    for (PrintObjectStep step : geometry)
        REQUIRE(print.is_step_done(step));

    for (double acceleration : {300.0, 6000.0}) {
        config.set_key_value("default_acceleration", new ConfigOptionFloat(acceleration));
        print.apply(model, config);
        for (PrintObjectStep step : geometry) {
            INFO("geometry step " << int(step) << ", acceleration " << acceleration);
            CHECK(print.is_step_done(step));
        }
    }

    config.set_key_value("layer_height", new ConfigOptionFloat(0.3));
    print.apply(model, config);
    CHECK_FALSE(print.is_step_done(posSlice));
    CHECK_FALSE(print.is_step_done(posPerimeters));
    CHECK_FALSE(print.is_step_done(posInfill));
}

TEST_CASE("Export preserves ordered support paths for the next native target",
          "[Print][native_reapply][support_export_immutable]")
{
    const bool organic = GENERATE(false, true);
    Print print;
    Test::init_and_process_print({Test::TestMesh::overhang}, print, {
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"enable_support", true},
        {"support_type", organic ? "tree(auto)" : "normal(auto)"},
        {"support_style", organic ? "organic" : "default"},
        {"support_threshold_angle", 45}, {"brim_width", 0.0}
    });
    const auto paths = [&]() {
        std::vector<Points> result;
        for (const PrintObject *object : print.objects())
            for (const SupportLayer *layer : object->support_layers())
                for (const Polyline &line : layer->support_fills.as_polylines())
                    result.push_back(line.points);
        return result;
    };
    const auto before = paths();
    REQUIRE_FALSE(before.empty());
    for (int pass = 0; pass < 2; ++pass) {
        INFO("organic " << organic << ", export " << pass);
        REQUIRE_FALSE(Test::gcode(print).empty());
        CHECK(paths() == before);
    }
}
