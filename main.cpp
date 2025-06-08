#include "CLI/CLI.hpp"
#include "spdlog/spdlog.h"
#include "toml++/impl/array.hpp"
#include "toml++/impl/parse_error.hpp"
#include "toml++/impl/parser.hpp"
#include "toml++/impl/table.hpp"
#include "toml++/toml.hpp"
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

struct AppConfig {
    std::string name;
    std::string version;
    std::vector<std::string> includes;
    
    AppConfig() : name("Dream++ Application"), version("1.0.0"), includes() {}
};

std::optional<AppConfig> parse_config_file(const std::string& fp, const std::string& project_name) {
    AppConfig config;
    try {
        toml::table tbl = toml::parse_file(fp);
        
        config.name = tbl["name"].value_or(project_name);
        config.version = tbl["version"].value_or(config.version);

        if (auto include_paths = tbl["includes"].as_array()) {
            for (auto& path : *include_paths) {
                if (path.is_string()) {
                    config.includes.push_back(*path.value<std::string>());
                }
            }
        }

        return config;
    } catch (const toml::parse_error& err) {
        spdlog::error("[📖] ❌ Couldn't parse config file '{}'.", fp);
        spdlog::error("[📖] ❌ TOML Parse Error: {}", err.description());
        return std::nullopt;
    } catch (const std::exception& e) {
        spdlog::error("[📖] ❌ Unexpected exception: {}", e.what());
        return std::nullopt;
    }
}

toml::table serialise_config(const AppConfig& config) {
    toml::table tbl;

    tbl.insert("name", config.name);
    tbl.insert("version", config.version);

    toml::array include_paths;
    for (std::string path : config.includes) {
        include_paths.push_back(path);
    }
    tbl.insert("includes", include_paths);

    return tbl;
}

bool sync_config(const toml::table& tbl, const std::string& fp) {
    try {
        std::ofstream file(fp);
        if (!file.is_open()) {
            spdlog::error("[📝] ❌ Failed to sync config '{}'.", fp);
            return false;
        }
        file << tbl;
        file.close();
        spdlog::info("[📝] ✅ Synced config '{}'!", fp);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[📝] ❌ Failed to sync config: {}", e.what());
        return false;
    }
}

int main(int argc, char **argv) {
    CLI::App app{"✨ DreamCPP"};

    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Enable verbose output");

    std::string config_file_path = "dreamcpp.toml";
    app.add_option("-c,--config", config_file_path,
                   "Path to configuration file")
        ->check(CLI::ExistingFile);
    
    CLI::App* new_cmd = app.add_subcommand("new", "Create a new ☁️++ project");
    std::string project_name;
    new_cmd->add_option("project_name", project_name, "Name of the new project"); // implicitly required

    new_cmd->callback([&]() {
        spdlog::info("[🏗️] Creating new project '{}'", project_name);
        try {
            if (!std::filesystem::create_directory(project_name)) {
                spdlog::error("[🏗️] ❌ Something went wrong creating directories.");
                exit(1);
            }
            AppConfig config;
            config.name = project_name;
            sync_config(serialise_config(config), std::format("{}/{}", project_name, config_file_path));
        } catch (std::filesystem::filesystem_error& e) {
            spdlog::error("[🏗️] ❌ Failed to create project. Filesystem error: {}", e.what());
            exit(1);
        }
    });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // if (command == "build") {
    //     if (auto cfg = parse_config_file(config_file_path, )) {

    //     }
    // } else {

    // }

    return 0;
}
