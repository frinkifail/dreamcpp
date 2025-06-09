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
    std::string name = "Dream++ Application";
    std::string version = "1.0.0";
    std::vector<std::string> includes;
    std::string standard = "c++20";
    std::string preferred_compiler = "clang++";
};

template<typename Container>
std::string join(const Container& container, const std::string& delimiter) {
    std::string result;
    for (const auto& item : container) {
        result += delimiter + item;
    }
    return result;
}

struct ExecResult {
    std::string output;
    int exit_code;
};

ExecResult exec(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) throw std::runtime_error("popen() failed!");

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    int status = pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return {result, exit_code};
}

template<typename T>
void maybe_assign(const toml::table& tbl, const std::string& key, T& target) {
    if (auto val = tbl[key].value<T>()) {
        target = *val;
    }
}

std::optional<AppConfig> parse_config_file(const std::string& fp, const std::string& project_name) {
    AppConfig config;
    try {
        toml::table tbl = toml::parse_file(fp);
        
        config.name = tbl["name"].value_or(project_name);
        maybe_assign(tbl, "version", config.version);
        maybe_assign(tbl, "standard", config.standard);
        maybe_assign(tbl, "preferred_compiler", config.preferred_compiler);

        if (auto arr = tbl["includes"].as_array()) {
            for (const auto& val : *arr) {
                if (auto str = val.value<std::string>())
                    config.includes.push_back(*str);
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
    for (const std::string& path : config.includes) {
        include_paths.push_back(path);
    }
    tbl.insert("includes", include_paths);

    tbl.insert("standard", config.standard);
    tbl.insert("preferred_compiler", config.preferred_compiler);

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
    #ifdef _WIN32
    spdlog::error("Windows isn't supported (for now).");
    exit(1);
    #endif
    CLI::App app{"✨ DreamCPP"};

    auto verbose = false;
    app.add_flag("-v,--verbose", verbose, "Enable verbose output");

    std::string config_file_path = "dreamcpp.toml";
    app.add_option("-c,--config", config_file_path,
                   "Path to configuration file")
        ->check(CLI::ExistingFile);
    
    auto new_cmd = app.add_subcommand("new", "Create a new ☁️++ project");
    std::string project_name;
    new_cmd->add_option("project_name", project_name, "Name of the new project"); // implicitly required

    auto build_cmd = app.add_subcommand("build", "Builds a 💭++ project");

    new_cmd->callback([&]() {
        spdlog::info("[🏗️] Creating new project '{}'", project_name);
        try {
            if (!std::filesystem::create_directory(project_name)) {
                spdlog::error("[🏗️] ❌ Something went wrong creating directories.");
                exit(1);
            }

            auto subdirs = {"src", "build"};
            for (const auto& dir : subdirs) {
                if (!std::filesystem::create_directory(std::filesystem::path(project_name) / dir)) {
                    spdlog::warn("[🏗️] ⚠️ Something went wrong creating subdirectories.");
                }
            }

            AppConfig config;
            config.name = project_name;
            sync_config(serialise_config(config), std::format("{}/{}", project_name, config_file_path));
        } catch (std::filesystem::filesystem_error& e) {
            spdlog::error("[🏗️] ❌ Failed to create project. Filesystem error: {}", e.what());
            exit(1);
        }
    });

    build_cmd->callback([&]() {
        spdlog::info("[⚒️] Building this project...");
        if (!std::filesystem::exists("dreamcpp.toml")) {
            spdlog::error("[⚒️] ❌ This... isn't a 🌌++ project."); // im running out of dream emoji ideas help
            exit(1);
        }
        auto app = parse_config_file("dreamcpp.toml", std::filesystem::current_path().filename().string());
        if (!app.has_value())
            exit(1);
        // construct build commands
        // NOTE: future do parallel
        auto out = exec(std::format("{} src/*.cpp -o build/{} -std={}{}", app->preferred_compiler, app->name, app->standard, join(app->includes, " -I")));
        if (out.exit_code != 0) {
            spdlog::warn("[⚒️] ⚠️ Failed to compile.");
            spdlog::warn("[⚒️] ⚠️ {}", out.output);
        }
    });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    return 0;
}
