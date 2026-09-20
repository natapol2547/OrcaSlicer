#include <catch2/catch_all.hpp>
#include "libslic3r/Print.hpp"
#include "test_data.hpp"

using namespace Slic3r;

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
