#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;

namespace {
// Loads `json` as this app's config file from a scratch data directory.
void load_config_text(AppConfig &config, const std::string &json)
{
    const std::string old_dir = data_dir();
    const auto        dir     = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("appconfig-test-%%%%%%%%");
    boost::filesystem::create_directories(dir);
    set_data_dir(dir.string());
    {
        boost::nowide::ofstream f((dir / (SLIC3R_APP_KEY ".conf")).string());
        // The trailer save() writes; the Windows loader expects a line after the closing brace.
        f << json << "\n# MD5 checksum 0\n";
    }
    const std::string error = config.load();
    set_data_dir(old_dir);
    boost::system::error_code ec;
    boost::filesystem::remove_all(dir, ec);
    REQUIRE(error.empty());
}
} // namespace

TEST_CASE("Stealth mode is turned off once for a config that has it on", "[AppConfig]") {
    SECTION("a config from a build that forced it on, including one whose broken first migration only set its flag") {
        AppConfig config;
        load_config_text(config, R"({"app": {"stealth_mode": true, "unlayered_stealth_off_migrated": true}})");
        REQUIRE_FALSE(config.get_stealth_mode());
        REQUIRE(config.get_bool("unlayered_stealth_off_migrated_v2"));
    }
    SECTION("turned back on after the migration, it stays on") {
        AppConfig config;
        load_config_text(config, R"({"app": {"stealth_mode": true, "unlayered_stealth_off_migrated_v2": true}})");
        REQUIRE(config.get_stealth_mode());
    }
}

TEST_CASE("AppConfig network version helpers", "[AppConfig]") {
    AppConfig config;

    SECTION("skipped versions starts empty") {
        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.empty());
    }

    SECTION("add and check skipped version") {
        config.add_skipped_network_version("02.01.01.52");
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
        REQUIRE_FALSE(config.is_network_version_skipped("02.03.00.62"));
    }

    SECTION("multiple skipped versions") {
        config.add_skipped_network_version("02.01.01.52");
        config.add_skipped_network_version("02.00.02.50");

        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.size() == 2);
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
        REQUIRE(config.is_network_version_skipped("02.00.02.50"));
    }

    SECTION("clear skipped versions") {
        config.add_skipped_network_version("02.01.01.52");
        config.clear_skipped_network_versions();
        REQUIRE_FALSE(config.is_network_version_skipped("02.01.01.52"));
    }

    SECTION("duplicate add is idempotent") {
        config.add_skipped_network_version("02.01.01.52");
        config.add_skipped_network_version("02.01.01.52");

        auto skipped = config.get_skipped_network_versions();
        REQUIRE(skipped.size() == 1);
        REQUIRE(config.is_network_version_skipped("02.01.01.52"));
    }
}
