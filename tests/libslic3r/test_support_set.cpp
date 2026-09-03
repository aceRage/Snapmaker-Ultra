// Ultra (support sets) - Stage 1 gate.
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md, "Stage 1 - Support sets", Gate.
//
// Every helper under test is wx-free and lives in libslic3r/SupportSet.{hpp,cpp}, so these tests
// belong in the libslic3r suite.

#include <catch2/catch.hpp>

#include <algorithm>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SupportSet.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;

namespace {

// A config carrying just the two vectors resolve_interface_filament() reads.
DynamicPrintConfig filaments(const std::vector<std::string> &types, const std::vector<unsigned char> &soluble)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_type", new ConfigOptionStrings(types));
    cfg.set_key_value("filament_soluble", new ConfigOptionBools(soluble));
    return cfg;
}

bool has_key(const std::vector<std::string> &v, const std::string &k)
{
    return std::find(v.begin(), v.end(), k) != v.end();
}

} // namespace

SCENARIO("Support set: the allowed key list", "[SupportSet]")
{
    const std::vector<std::string> &keys = support_set_keys();

    THEN("it is non-empty, sorted and free of duplicates")
    {
        REQUIRE(! keys.empty());
        REQUIRE(std::is_sorted(keys.begin(), keys.end()));
        REQUIRE(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
    }

    THEN("every key is a PrintObjectConfig member with the Support category")
    {
        PrintObjectConfig proto;
        const t_config_option_keys object_keys = proto.keys();
        for (const std::string &key : keys) {
            INFO(key);
            REQUIRE(std::find(object_keys.begin(), object_keys.end(), key) != object_keys.end());
            const ConfigOptionDef *def = print_config_def.get(key);
            REQUIRE(def != nullptr);
            REQUIRE(def->category == "Support");
        }
    }

    THEN("it carries the interface keys a set is for")
    {
        REQUIRE(has_key(keys, "support_interface_top_layers"));
        REQUIRE(has_key(keys, "support_interface_bottom_layers"));
        REQUIRE(has_key(keys, "support_interface_spacing"));
        REQUIRE(has_key(keys, "support_bottom_interface_spacing"));
        REQUIRE(has_key(keys, "support_interface_pattern"));
        REQUIRE(has_key(keys, "support_interface_loop_pattern"));
        REQUIRE(has_key(keys, "support_top_z_distance"));
        REQUIRE(has_key(keys, "support_style"));
        REQUIRE(has_key(keys, "support_threshold_angle"));
    }

    THEN("it carries none of the excluded keys")
    {
        for (const std::string &key : support_set_excluded_keys()) {
            INFO(key);
            REQUIRE(! has_key(keys, key));
        }
        // Named explicitly because these are the ones that make a set portable (or not).
        REQUIRE(! has_key(keys, "support_filament"));
        REQUIRE(! has_key(keys, "support_interface_filament"));
        REQUIRE(! has_key(keys, "enforce_support_layers"));
        REQUIRE(! has_key(keys, "independent_support_layer_height"));
        for (const std::string &key : keys) {
            INFO(key);
            REQUIRE(key.rfind("raft_", 0) != 0);
            REQUIRE(key.rfind("brim_", 0) != 0);
        }
    }
}

SCENARIO("Support set: capture from a config", "[SupportSet]")
{
    GIVEN("a full print config with an edited support block")
    {
        DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
        cfg.set_key_value("support_interface_top_layers", new ConfigOptionInt(4));
        cfg.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
        cfg.set_key_value("raft_layers", new ConfigOptionInt(3));
        cfg.set_key_value("support_filament", new ConfigOptionInt(2));
        cfg.set_key_value("support_interface_filament", new ConfigOptionInt(0));

        WHEN("it is captured as a support set")
        {
            SupportSet set = support_set_from_config(cfg, "Dense interface");

            THEN("the set carries exactly the allowed keys")
            {
                REQUIRE(set.name == "Dense interface");
                REQUIRE(set.values.size() == support_set_keys().size());
                for (const std::string &key : support_set_keys())
                    REQUIRE(set.values.count(key) == 1);
            }
            THEN("the edited values came across")
            {
                REQUIRE(set.values.at("support_interface_top_layers") == "4");
                REQUIRE(set.values.at("support_interface_spacing") == "0");
            }
            THEN("filament slots and raft keys did not")
            {
                REQUIRE(set.values.count("support_filament") == 0);
                REQUIRE(set.values.count("support_interface_filament") == 0);
                REQUIRE(set.values.count("raft_layers") == 0);
                REQUIRE(set.values.count("brim_width") == 0);
            }
            THEN("the interface filament was reduced to a type")
            {
                REQUIRE(set.interface_filament_type == "same");
            }
            THEN("the metadata is filled in")
            {
                REQUIRE(! set.created.empty());
                REQUIRE(! set.app_version.empty());
            }
        }
    }
}

SCENARIO("Support set: apply writes only its own keys", "[SupportSet]")
{
    GIVEN("a set with two values")
    {
        SupportSet set;
        set.name                    = "Two values";
        set.interface_filament_type = "same";
        set.values["support_interface_top_layers"] = "5";
        set.values["support_interface_spacing"]    = "0.75";

        DynamicPrintConfig full = filaments({ "PLA", "PETG" }, { 0, 0 });

        WHEN("it is applied to an empty delta")
        {
            DynamicPrintConfig delta;
            std::string        warning;
            support_set_apply_to(set, delta, full, &warning);

            THEN("exactly the set's keys plus the resolved filament slot are present")
            {
                t_config_option_keys keys = delta.keys();
                std::sort(keys.begin(), keys.end());
                REQUIRE(keys == t_config_option_keys{ "support_interface_filament",
                                                      "support_interface_spacing",
                                                      "support_interface_top_layers" });
                REQUIRE(delta.opt_int("support_interface_top_layers") == 5);
                REQUIRE(delta.opt_float("support_interface_spacing") == Approx(0.75));
                REQUIRE(delta.opt_int("support_interface_filament") == 0);
                REQUIRE(warning.empty());
            }
        }

        WHEN("the set carries a key that is not allowed")
        {
            set.values["raft_layers"]   = "9";
            set.values["nonsense_key"]  = "1";
            DynamicPrintConfig delta;
            support_set_apply_to(set, delta, full, nullptr);

            THEN("the forbidden keys are skipped and the rest still applies")
            {
                REQUIRE(! delta.has("raft_layers"));
                REQUIRE(! delta.has("nonsense_key"));
                REQUIRE(delta.opt_int("support_interface_top_layers") == 5);
            }
        }
    }
}

SCENARIO("Support set: interface filament resolution", "[SupportSet]")
{
    std::string warning;

    GIVEN("PLA, PETG, PVA(soluble)")
    {
        DynamicPrintConfig cfg = filaments({ "PLA", "PETG", "PVA" }, { 0, 0, 1 });

        THEN("\"same\" and the empty type mean slot 0")
        {
            REQUIRE(resolve_interface_filament("same", cfg, &warning) == 0);
            REQUIRE(warning.empty());
            REQUIRE(resolve_interface_filament("", cfg, &warning) == 0);
            REQUIRE(warning.empty());
        }
        THEN("\"soluble\" finds the soluble slot")
        {
            REQUIRE(resolve_interface_filament("soluble", cfg, &warning) == 3);
            REQUIRE(warning.empty());
        }
        THEN("an explicit type matches exactly, case-insensitively")
        {
            REQUIRE(resolve_interface_filament("PETG", cfg, &warning) == 2);
            REQUIRE(resolve_interface_filament("petg", cfg, &warning) == 2);
            REQUIRE(warning.empty());
        }
        THEN("an unloaded type falls back to slot 0 with a warning")
        {
            REQUIRE(resolve_interface_filament("ABS", cfg, &warning) == 0);
            REQUIRE(! warning.empty());
        }
    }

    GIVEN("PLA, BVOH but nothing flagged soluble")
    {
        DynamicPrintConfig cfg = filaments({ "PLA", "BVOH" }, { 0, 0 });
        THEN("\"soluble\" falls back to the PVA/BVOH type match")
        {
            REQUIRE(resolve_interface_filament("soluble", cfg, &warning) == 2);
            REQUIRE(warning.empty());
        }
    }

    GIVEN("no soluble filament at all")
    {
        DynamicPrintConfig cfg = filaments({ "PLA", "PETG" }, { 0, 0 });
        THEN("\"soluble\" is slot 0 with a warning")
        {
            REQUIRE(resolve_interface_filament("soluble", cfg, &warning) == 0);
            REQUIRE(! warning.empty());
        }
    }

    GIVEN("PETG and PLA-CF, but no plain PLA")
    {
        DynamicPrintConfig cfg = filaments({ "PETG", "PLA-CF" }, { 0, 0 });
        THEN("\"PLA\" matches the PLA-CF prefix")
        {
            REQUIRE(resolve_interface_filament("PLA", cfg, &warning) == 2);
            REQUIRE(warning.empty());
        }
    }

    GIVEN("plain PLA in a later slot than PLA-CF")
    {
        DynamicPrintConfig cfg = filaments({ "PLA-CF", "PLA" }, { 0, 0 });
        THEN("the exact match wins over the prefix match")
        {
            REQUIRE(resolve_interface_filament("PLA", cfg, &warning) == 2);
        }
    }

    GIVEN("two soluble filaments")
    {
        DynamicPrintConfig cfg = filaments({ "PLA", "PVA", "BVOH" }, { 0, 1, 1 });
        THEN("ties break to the lowest slot")
        {
            REQUIRE(resolve_interface_filament("soluble", cfg, &warning) == 2);
        }
    }

    GIVEN("filament_soluble shorter than filament_type")
    {
        DynamicPrintConfig cfg = filaments({ "PLA", "PVA" }, { 0 });
        THEN("the missing entries are read as not soluble and the type match still fires")
        {
            REQUIRE(resolve_interface_filament("soluble", cfg, &warning) == 2);
        }
    }

    GIVEN("a config with neither vector")
    {
        DynamicPrintConfig cfg;
        THEN("every explicit type resolves to slot 0 with a warning")
        {
            REQUIRE(resolve_interface_filament("PLA", cfg, &warning) == 0);
            REQUIRE(! warning.empty());
            REQUIRE(resolve_interface_filament("same", cfg, &warning) == 0);
        }
    }
}

SCENARIO("Support set: the reverse filament map", "[SupportSet]")
{
    GIVEN("PLA, PETG, PVA(soluble)")
    {
        DynamicPrintConfig cfg = filaments({ "PLA", "PETG", "PVA" }, { 0, 0, 1 });

        THEN("slot 0 is \"same\"")
        {
            cfg.set_key_value("support_interface_filament", new ConfigOptionInt(0));
            REQUIRE(support_set_interface_filament_type(cfg) == "same");
        }
        THEN("a soluble slot is \"soluble\"")
        {
            cfg.set_key_value("support_interface_filament", new ConfigOptionInt(3));
            REQUIRE(support_set_interface_filament_type(cfg) == "soluble");
        }
        THEN("any other slot is its filament type")
        {
            cfg.set_key_value("support_interface_filament", new ConfigOptionInt(2));
            REQUIRE(support_set_interface_filament_type(cfg) == "PETG");
        }
        THEN("an out-of-range slot degrades to \"same\"")
        {
            cfg.set_key_value("support_interface_filament", new ConfigOptionInt(9));
            REQUIRE(support_set_interface_filament_type(cfg) == "same");
        }
    }

    GIVEN("a set captured on one printer and applied on another")
    {
        // Snapmaker-ish ordering: PVA in slot 2.
        DynamicPrintConfig source = DynamicPrintConfig::full_print_config();
        source.set_key_value("filament_type", new ConfigOptionStrings({ "PLA", "PVA", "PETG" }));
        source.set_key_value("filament_soluble", new ConfigOptionBools(std::vector<unsigned char>{ 0, 1, 0 }));
        source.set_key_value("support_interface_filament", new ConfigOptionInt(2));
        SupportSet set = support_set_from_config(source, "Soluble interface");
        REQUIRE(set.interface_filament_type == "soluble");

        // Bambu-ish ordering: PVA in slot 4.
        DynamicPrintConfig target = filaments({ "PLA", "PETG", "ABS", "PVA" }, { 0, 0, 0, 1 });
        DynamicPrintConfig delta;
        std::string        warning;
        support_set_apply_to(set, delta, target, &warning);

        THEN("the interface filament lands on the right slot for the target printer")
        {
            REQUIRE(delta.opt_int("support_interface_filament") == 4);
            REQUIRE(warning.empty());
        }
    }
}

SCENARIO("Support set: JSON round trip", "[SupportSet]")
{
    GIVEN("a set with every field filled in")
    {
        SupportSet set;
        set.name                    = "Soluble interface";
        set.description             = "PVA interface, zero gap, 3 dense layers";
        set.created                 = "2026-09-02T14:05:11Z";
        set.app_version             = "2.1.0";
        set.interface_filament_type = "soluble";
        set.values["support_interface_top_layers"]    = "3";
        set.values["support_interface_bottom_layers"] = "3";
        set.values["support_interface_spacing"]       = "0";
        set.values["support_top_z_distance"]          = "0";
        set.values["support_style"]                   = "grid";

        WHEN("it goes through JSON and back")
        {
            SupportSet  back;
            std::string err;
            REQUIRE(support_set_from_json_string(support_set_to_json_string(set), back, &err));
            THEN("every field survives")
            {
                INFO(err);
                REQUIRE(back.name == set.name);
                REQUIRE(back.description == set.description);
                REQUIRE(back.created == set.created);
                REQUIRE(back.app_version == set.app_version);
                REQUIRE(back.interface_filament_type == set.interface_filament_type);
                REQUIRE(back.values == set.values);
            }
        }
    }

    GIVEN("JSON with an unknown and a forbidden key")
    {
        const std::string text = R"({
            "version": 1,
            "type": "support_set",
            "name": "Mixed",
            "interface_filament_type": "same",
            "values": {
                "support_interface_top_layers": "2",
                "support_interface_filament": "3",
                "raft_layers": "4",
                "not_a_setting": "7"
            }
        })";

        WHEN("it is loaded")
        {
            SupportSet               set;
            std::string              err;
            std::vector<std::string> dropped;
            REQUIRE(support_set_from_json_string(text, set, &err, &dropped));

            THEN("the good key stays, the rest are dropped and reported")
            {
                REQUIRE(set.values.size() == 1);
                REQUIRE(set.values.at("support_interface_top_layers") == "2");
                std::sort(dropped.begin(), dropped.end());
                REQUIRE(dropped == std::vector<std::string>{ "not_a_setting", "raft_layers", "support_interface_filament" });
            }
        }
    }

    GIVEN("malformed or foreign JSON")
    {
        SupportSet  set;
        std::string err;
        THEN("a parse error is reported, not thrown")
        {
            REQUIRE(! support_set_from_json_string("{ not json", set, &err));
            REQUIRE(! err.empty());
        }
        THEN("a file with no name is rejected")
        {
            REQUIRE(! support_set_from_json_string(R"({"type":"support_set","values":{}})", set, &err));
        }
        THEN("a file of another type is rejected")
        {
            REQUIRE(! support_set_from_json_string(R"({"type":"process","name":"x"})", set, &err));
        }
        THEN("a file from a newer version is rejected")
        {
            REQUIRE(! support_set_from_json_string(R"({"type":"support_set","name":"x","version":99})", set, &err));
        }
        THEN("a minimal file loads with defaults")
        {
            REQUIRE(support_set_from_json_string(R"({"name":"Bare"})", set, &err));
            REQUIRE(set.name == "Bare");
            REQUIRE(set.interface_filament_type == "same");
            REQUIRE(set.values.empty());
        }
    }
}

SCENARIO("Support set: file name sanitising", "[SupportSet]")
{
    THEN("plain names survive")
    {
        REQUIRE(sanitize_support_set_filename("Soluble interface") == "Soluble interface");
        REQUIRE(sanitize_support_set_filename("PVA_3-layers.v2") == "PVA_3-layers.v2");
    }
    THEN("path separators and other specials become underscores")
    {
        REQUIRE(sanitize_support_set_filename("a/b\\c") == "a_b_c");
        REQUIRE(sanitize_support_set_filename("q?:*\"<>|r") == "q_______r");
    }
    THEN("leading and trailing whitespace is trimmed")
    {
        REQUIRE(sanitize_support_set_filename("  padded  ") == "padded");
    }
    THEN("a name that collapses to nothing still yields a file stem")
    {
        REQUIRE(sanitize_support_set_filename("") == "support_set");
        REQUIRE(sanitize_support_set_filename("   ") == "support_set");
        REQUIRE(sanitize_support_set_filename("..") == "support_set");
    }
    THEN("non-ASCII is replaced byte by byte, never dropped to nothing")
    {
        const std::string sanitized = sanitize_support_set_filename("\xC3\xA9t\xC3\xA9");
        REQUIRE(sanitized.find('t') != std::string::npos);
        REQUIRE(sanitized.find("\xC3") == std::string::npos);
    }
}

SCENARIO("Support set: the on-disk store", "[SupportSet]")
{
    // The store is the only thing here that touches data_dir(); point it at a scratch directory
    // and put it back afterwards so the rest of the suite is unaffected.
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("snorca_support_set_%%%%%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    SupportSetStore::set_preset_folder("default");

    SupportSetStore &store = SupportSetStore::instance();
    store.reload();
    REQUIRE(store.list().empty());

    SupportSet set;
    set.name                    = "Soluble PVA";
    set.description             = "zero gap";
    set.interface_filament_type = "soluble";
    set.values["support_interface_top_layers"] = "3";

    WHEN("a set is saved")
    {
        std::string err;
        REQUIRE(store.save(set, &err));
        INFO(err);

        THEN("it lands in <datadir>/user/default/support_set and reads back")
        {
            REQUIRE(boost::filesystem::exists(scratch / "user" / "default" / "support_set" / "Soluble PVA.json"));
            REQUIRE(store.list().size() == 1);
            const SupportSet *found = store.find("Soluble PVA");
            REQUIRE(found != nullptr);
            REQUIRE(found->description == "zero gap");
            REQUIRE(found->interface_filament_type == "soluble");
            REQUIRE(found->values.at("support_interface_top_layers") == "3");
            REQUIRE(! found->read_only);
        }

        THEN("saving again overwrites in place rather than making a second file")
        {
            SupportSet updated = set;
            updated.values["support_interface_top_layers"] = "6";
            REQUIRE(store.save(updated, &err));
            REQUIRE(store.list().size() == 1);
            REQUIRE(store.find("Soluble PVA")->values.at("support_interface_top_layers") == "6");
        }

        THEN("names that sanitise to the same stem each get their own file")
        {
            SupportSet slash = set;
            slash.name = "Soluble/PVA";
            REQUIRE(store.save(slash, &err));
            SupportSet colon = set;
            colon.name = "Soluble:PVA";
            REQUIRE(store.save(colon, &err));

            REQUIRE(store.list().size() == 3);
            const boost::filesystem::path dir = scratch / "user" / "default" / "support_set";
            REQUIRE(boost::filesystem::exists(dir / "Soluble PVA.json"));
            REQUIRE(boost::filesystem::exists(dir / "Soluble_PVA.json"));
            // The collision suffix goes on the file name only; the set names are untouched.
            REQUIRE(boost::filesystem::exists(dir / "Soluble_PVA (2).json"));
            REQUIRE(store.find("Soluble PVA") != nullptr);
            REQUIRE(store.find("Soluble/PVA") != nullptr);
            REQUIRE(store.find("Soluble:PVA") != nullptr);
        }

        THEN("rename moves the file and keeps the values")
        {
            REQUIRE(store.rename("Soluble PVA", "PVA interface", &err));
            REQUIRE(store.list().size() == 1);
            REQUIRE(store.find("Soluble PVA") == nullptr);
            REQUIRE(store.find("PVA interface") != nullptr);
            REQUIRE(store.find("PVA interface")->values.at("support_interface_top_layers") == "3");
            REQUIRE(! boost::filesystem::exists(scratch / "user" / "default" / "support_set" / "Soluble PVA.json"));
        }

        THEN("rename onto an existing name is refused")
        {
            SupportSet other = set;
            other.name = "Other";
            REQUIRE(store.save(other, &err));
            REQUIRE(! store.rename("Soluble PVA", "Other", &err));
            REQUIRE(! err.empty());
            REQUIRE(store.list().size() == 2);
        }

        THEN("delete removes the file")
        {
            REQUIRE(store.remove("Soluble PVA", &err));
            REQUIRE(store.list().empty());
            REQUIRE(! boost::filesystem::exists(scratch / "user" / "default" / "support_set" / "Soluble PVA.json"));
        }

        THEN("a set with no name is refused")
        {
            SupportSet nameless = set;
            nameless.name.clear();
            REQUIRE(! store.save(nameless, &err));
        }
    }

    WHEN("the account folder is not \"default\"")
    {
        std::string err;
        REQUIRE(store.save(set, &err));            // written into user/default/support_set
        SupportSetStore::set_preset_folder("1234567890");
        store.reload();

        THEN("the default folder is still readable, but read-only")
        {
            REQUIRE(store.list().size() == 1);
            const SupportSet *found = store.find("Soluble PVA");
            REQUIRE(found != nullptr);
            REQUIRE(found->read_only);
            REQUIRE(! store.remove("Soluble PVA", &err));
            REQUIRE(! store.rename("Soluble PVA", "Nope", &err));
        }

        THEN("saving over it shadows it in the account's own folder")
        {
            SupportSet shadow = set;
            shadow.values["support_interface_top_layers"] = "8";
            REQUIRE(store.save(shadow, &err));
            REQUIRE(store.list().size() == 1);
            REQUIRE(store.find("Soluble PVA")->values.at("support_interface_top_layers") == "8");
            REQUIRE(! store.find("Soluble PVA")->read_only);
            REQUIRE(boost::filesystem::exists(scratch / "user" / "1234567890" / "support_set" / "Soluble PVA.json"));
            // The shared copy is untouched.
            REQUIRE(boost::filesystem::exists(scratch / "user" / "default" / "support_set" / "Soluble PVA.json"));
        }
    }

    WHEN("the directory holds a file that is not a support set")
    {
        std::string err;
        REQUIRE(store.save(set, &err));
        const boost::filesystem::path dir = scratch / "user" / "default" / "support_set";
        {
            boost::filesystem::ofstream ofs(dir / "junk.json");
            ofs << "{ this is not json";
        }
        {
            boost::filesystem::ofstream ofs(dir / "readme.txt");
            ofs << "ignore me";
        }
        store.reload();

        THEN("it is skipped and the good set still loads")
        {
            REQUIRE(store.list().size() == 1);
            REQUIRE(store.find("Soluble PVA") != nullptr);
        }
    }

    // Teardown - runs for each Catch2 leaf section.
    SupportSetStore::set_preset_folder(std::string());
    set_data_dir(saved_data_dir);
    boost::system::error_code ec;
    boost::filesystem::remove_all(scratch, ec);
}
