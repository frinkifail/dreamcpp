#include "CLI/CLI.hpp"
#include "spdlog/spdlog.h"
#include "toml++/impl/array.hpp"
#include "toml++/impl/parse_error.hpp"
#include "toml++/impl/parser.hpp"
#include "toml++/impl/table.hpp"
#include "toml++/toml.hpp"
#include <algorithm> // Added this for std::find
#include <cstdlib>
#include <curl/curl.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct Dependency {
    std::string name;
    std::string version = "latest"; // Default version
    bool system = false; // Is it a system library? (linked using -l<dep>)
};

struct DepIndex {
    std::string git;
    std::vector<std::string> aliases;
    std::optional<std::string> branch;
    bool header_only = false;
};

struct AppConfig {
    std::string name = "Dream++ Application";
    std::string version = "1.0.0";
    std::vector<std::string> includes = {};
    std::string standard = "c++20";
    std::string preferred_compiler = "clang++";
    std::vector<Dependency> deps = {};
};

template <typename Container>
std::string join(const Container &container, const std::string &delimiter) {
    std::string result;
    for (const auto &item : container) {
        result += delimiter + item;
    }
    return result;
}

template <typename Map, typename Key>
std::optional<typename Map::mapped_type> get_or_nullopt(const Map &map,
                                                        const Key &key) {
    auto it = map.find(key);
    if (it != map.end()) {
        return it->second;
    }
    return std::nullopt;
}

template <typename T>
void maybe_assign(const toml::table &tbl, const std::string &key, T &target) {
    if (auto val = tbl[key].value<T>()) {
        target = *val;
    }
}

size_t writeCallback(void *contents, size_t size, size_t nmemb,
                     std::string *userp) {
    userp->append((char *)contents, size * nmemb);
    return size * nmemb;
}

std::pair<long, std::string> fetchURL(const std::string &url) {
    CURL *curl = curl_easy_init();
    CURLcode res;
    std::string responseBody;
    long responseCode = 0;

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

        res = curl_easy_perform(curl);

        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
        } else {
            std::cerr << "curl_easy_perform() failed: "
                      << curl_easy_strerror(res) << std::endl;
        }

        curl_easy_cleanup(curl);
    }

    return {responseCode, responseBody};
}

struct ExecResult {
    std::string output;
    int exit_code;
};

ExecResult exec(const std::string &cmd) {
    std::array<char, 128> buffer;
    std::string result;

    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen() failed!");

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    int status = pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return {result, exit_code};
}

std::optional<AppConfig> parse_config_file(const std::string &fp,
                                           const std::string &project_name) {
    AppConfig config;
    try {
        toml::table tbl = toml::parse_file(fp);

        config.name = tbl["name"].value_or(project_name);
        maybe_assign(tbl, "version", config.version);
        maybe_assign(tbl, "standard", config.standard);
        maybe_assign(tbl, "preferred_compiler", config.preferred_compiler);

        if (auto arr = tbl["includes"].as_array()) {
            config.includes.clear(); // Clear defaults
            for (const auto &val : *arr) {
                if (auto str = val.value<std::string>())
                    config.includes.push_back(*str);
            }
        }

        if (auto arr = tbl["dependencies"].as_array()) {
            config.deps.clear(); // Clear defaults
            for (const auto &val : *arr) {
                if (auto tbldep = val.as_table()) {
                    Dependency dep;
                    dep.name = (*tbldep)["name"].value_or("unknown");
                    dep.version = (*tbldep)["version"].value_or("latest");
                    dep.system = (*tbldep)["system"].value_or(false);
                    if (!dep.name.empty() && dep.name != "unknown") {
                        config.deps.push_back(dep);
                    }
                }
            }
        }

        return config;
    } catch (const toml::parse_error &err) {
        spdlog::error("[📖] ❌ Couldn't parse config file '{}'.", fp);
        spdlog::error("[📖] ❌ TOML Parse Error: {}", err.description());
        return std::nullopt;
    } catch (const std::exception &e) {
        spdlog::error("[📖] ❌ Unexpected exception: {}", e.what());
        return std::nullopt;
    }
}

std::optional<std::map<std::string, DepIndex>>
parse_repository_index(const std::string &tomlstr) {
    try {
        toml::table tbl = toml::parse(tomlstr);
        std::map<std::string, DepIndex> depmap;

        for (const auto &[k, v] : tbl) {
            if (auto vtbl = v.as_table()) {
                DepIndex dep;
                dep.git = (*vtbl)["git"].value_or("");
                dep.header_only = (*vtbl)["header"].value_or(dep.header_only);

                // Handle aliases safely
                if (auto aliases_arr = (*vtbl)["aliases"].as_array()) {
                    for (const auto &alias : *aliases_arr) {
                        if (auto alias_str = alias.value<std::string>()) {
                            dep.aliases.push_back(*alias_str);
                        }
                    }
                }

                // Handle optional branch
                if (auto branch_val = (*vtbl)["branch"].value<std::string>()) {
                    dep.branch = *branch_val;
                }

                if (!dep.git.empty()) { // Only add if git URL exists
                    depmap[std::string(k)] = dep;
                }
            }
        }
        return depmap;
    } catch (const toml::parse_error &err) {
        spdlog::error("[📖] ❌ Couldn't parse repository index.");
        spdlog::error("[📖] ❌ TOML Parse Error: {}", err.description());
        return std::nullopt;
    } catch (const std::exception &e) {
        spdlog::error("[📖] ❌ Unexpected exception: {}", e.what());
        return std::nullopt;
    }
}

toml::table serialise_config(const AppConfig &config) {
    toml::table tbl;

    tbl.insert("name", config.name);
    tbl.insert("version", config.version);

    toml::array include_paths;
    for (const std::string &path : config.includes) {
        include_paths.push_back(path);
    }
    tbl.insert("includes", include_paths);

    toml::array deps;
    for (const auto &dep : config.deps) {
        toml::table tbldep;
        tbldep.insert("name", dep.name);
        tbldep.insert("version", dep.version);
        // Don't serialize git field - it's resolved dynamically
        deps.push_back(tbldep);
    }
    tbl.insert("dependencies", deps);

    tbl.insert("standard", config.standard);
    tbl.insert("preferred_compiler", config.preferred_compiler);

    return tbl;
}

bool sync_config(const toml::table &tbl, const std::string &fp) {
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
    } catch (const std::exception &e) {
        spdlog::error("[📝] ❌ Failed to sync config: {}", e.what());
        return false;
    }
}

namespace dependency {
bool validate_project_environment() {
    if (!std::filesystem::exists("dreamcpp.toml")) {
        spdlog::error("[🚀] ❌ This... isn't a 🌌++ project.");
        return false;
    }

    if (system("git --version > /dev/null 2>&1") != 0) { // Better git check
        spdlog::error("[🚀] ❌ You don't have git installed. :P");
        return false;
    }

    return true;
}

std::optional<std::string> search_local_indexes(const std::string &dep_name) {
    const std::vector<std::string> search_paths = {
        std::string(getenv("HOME") ? getenv("HOME") : "~") +
            "/.dreamcpp/index", // Expand ~ properly
        "../index"};

    for (const auto &path : search_paths) {
        if (std::filesystem::exists(path)) {
            // TODO: Implement actual search logic here
            // For now, just continue to remote search
            continue;
        }
    }

    return std::nullopt;
}

std::optional<DepIndex> search_remote_index(const std::string &dep_name) {
    const std::string index_url =
        "https://raw.githubusercontent.com/frinkifail/dreamcpp/refs/heads/main/"
        "index/dcpp%3Acore.toml";

    auto response = fetchURL(index_url);
    if (response.first != 200) {
        spdlog::warn("[🚀] ⚠️ Failed to fetch remote index (HTTP {})",
                     response.first);
        return std::nullopt;
    }

    auto index = parse_repository_index(response.second);
    if (!index.has_value()) {
        return std::nullopt;
    }

    // Direct lookup first
    auto dep = get_or_nullopt(index.value(), dep_name);
    if (dep.has_value()) {
        return dep;
    }

    // Search aliases
    for (const auto &[key, value] : index.value()) {
        auto it =
            std::find(value.aliases.begin(), value.aliases.end(), dep_name);
        if (it != value.aliases.end()) {
            return value;
        }
    }

    return std::nullopt;
}

std::optional<DepIndex> resolve_dependency_url(const std::string &dep_name) {
    // Try local indexes first
    // if (auto local_result = search_local_indexes(dep_name)) {
    //     return local_result;
    // }

    // Fall back to remote index
    if (auto remote_result = search_remote_index(dep_name)) {
        return remote_result;
    }

    return std::nullopt;
}

bool save_config(const AppConfig &config,
                 const std::string &config_path = "dreamcpp.toml") {
    // Actually implement this using the existing functions
    auto serialized = serialise_config(config);
    return sync_config(serialized, config_path);
}

bool add(const std::string &dep_name) {
    spdlog::info("[🚀] Adding dependency: {}", dep_name);

    if (!validate_project_environment()) {
        return false;
    }

    // Parse current config
    auto app = parse_config_file(
        "dreamcpp.toml", std::filesystem::current_path().filename().string());
    if (!app.has_value()) {
        return false;
    }

    // Check if dependency already exists
    for (const auto &dep : app->deps) {
        if (dep.name == dep_name) {
            spdlog::warn("[🚀] ⚠️  Dependency '{}' already exists", dep_name);
            return true;
        }
    }

    // Resolve the dependency URL to make sure it exists
    auto repo_url = resolve_dependency_url(dep_name);
    if (!repo_url.has_value()) {
        spdlog::error("[🚀] ❌ Dependency not found: {}", dep_name);
        spdlog::info(
            "[🚀] ❌ Checked: [~/.dreamcpp/index, ../index, github repo]");
        return false;
    }

    // Add to deps list (don't store the git URL)
    Dependency new_dep;
    new_dep.name = dep_name;
    new_dep.version = "latest"; // Default version
    // Don't set git field - it will be resolved dynamically

    app->deps.push_back(new_dep);

    // Save updated config
    if (!save_config(app.value())) {
        return false;
    }

    spdlog::info("[🚀] ✅ Added dependency '{}' to project", dep_name);
    spdlog::info("[🚀] 💡 Run 'dreamcpp sync' to install dependencies");
    return true;
}

bool clone_single_dependency(const std::string &dep_name) {
    std::string dep_path = std::format("build/deps/{}", dep_name);

    // Skip if already exists
    if (std::filesystem::exists(dep_path)) {
        spdlog::info("[🚀] ⏭️  Skipping '{}' (already exists)", dep_name);
        return true;
    }

    // Resolve the dependency URL dynamically
    auto repo_index = resolve_dependency_url(dep_name);
    if (!repo_index.has_value()) {
        spdlog::warn("[🚀] ⚠️ Failed to resolve dependency: {}", dep_name);
        return false;
    }

    spdlog::info("[🚀] 📦 Cloning '{}'...", dep_name);
    std::string clone_cmd =
        std::format("git clone {} {} 2>&1", repo_index->git, dep_path);

    auto result = exec(clone_cmd);
    if (result.exit_code != 0) {
        spdlog::error("[🚀] ❌ Failed to clone dependency: {}", dep_name);
        spdlog::error("[🚀] ❌ Git output: {}", result.output);
        return false;
    }

    if (repo_index->header_only) {
        try {
            std::filesystem::create_directory(std::format("build/includes/{}", dep_name));
            std::filesystem::rename(
                std::format("build/deps/{}/include/{}", dep_name, dep_name),
                std::format("build/includes/{}", dep_name)
            );
        } catch (const std::filesystem::filesystem_error& e) {
            spdlog::warn("[🚀] ⚠️ Couldn't move header-only include for '{}': {}", dep_name, e.what());
        }
    }

    spdlog::info("[🚀] ✅ Successfully cloned: {}", dep_name);
    return true;
}

bool sync() {
    spdlog::info("[🚀] Syncing project dependencies...");

    if (!validate_project_environment()) {
        return false;
    }

    // Parse current config
    auto app = parse_config_file(
        "dreamcpp.toml", std::filesystem::current_path().filename().string());
    if (!app.has_value()) {
        return false;
    }

    if (app->deps.empty()) {
        spdlog::info("[🚀] ✅ No dependencies to sync");
        return true;
    }

    // Create deps directory if it doesn't exist
    std::filesystem::create_directories("build/deps");

    // Create includes directory if it doesn't exist
    std::filesystem::create_directories("build/includes");

    // Sequential cloning (TODO: could be parallelized with std::async)
    bool all_success = true;
    for (const auto &dep : app->deps) {
        if (dep.system) {
            spdlog::info("[🚀] Skipping '{}', is a system library.", dep.name);
            continue;
        }
        if (!clone_single_dependency(dep.name)) {
            all_success = false;
        }
    }

    if (all_success) {
        spdlog::info("[🚀] ✅ All dependencies synced successfully");
    } else {
        spdlog::warn("[🚀] ⚠️ Some dependencies failed to sync");
    }

    return all_success;
}
} // namespace dependency

void build() {
    spdlog::info("[⚒️] Building this project...");
    if (!std::filesystem::exists("dreamcpp.toml")) {
        spdlog::error("[⚒️] ❌ This... isn't a 🌌++ project.");
        exit(1);
    }
    auto app_config = parse_config_file(
        "dreamcpp.toml",
        std::filesystem::current_path().filename().string());
    if (!app_config.has_value())
        exit(1);

    // Check if src directory exists and has files
    if (!std::filesystem::exists("src")) {
        spdlog::error("[⚒️] ❌ No src directory found");
        exit(1);
    }

    // construct build commands
    std::string include_flags = join(app_config->includes, " -I");
    if (!include_flags.empty()) {
        include_flags = " -I" + include_flags;
    }

    std::vector<std::string> link_syslibs;
    for (const auto &dep : app_config->deps) {
        // spdlog::info("Name: {}", dep.name);
        if (dep.system) {
            link_syslibs.push_back(dep.name);
        }
    }

    std::string link_flags = join(link_syslibs, " -l");
    // if (!link_flags.empty()) {
    //     link_flags = " -l" + link_flags;
    // }

    auto build_cmd = std::format(
        "{} src/*.cpp -o build/{} -std={} -Ibuild/includes -Lbuild/lib{}{}",
        app_config->preferred_compiler, app_config->name,
        app_config->standard, include_flags, link_flags);

    spdlog::info("[⚒️] Running: {}", build_cmd);
    auto out = exec(build_cmd);

    if (out.exit_code != 0) {
        spdlog::error("[⚒️] ❌ Failed to compile.");
        spdlog::error("[⚒️] ❌ {}", out.output);
        exit(1);
    } else {
        spdlog::info("[⚒️] ✅ Build successful!");
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

    auto new_cmd = app.add_subcommand("new", "Create a new 🛌++ project");
    std::string project_name;
    new_cmd->add_option("project_name", project_name, "Name of the new project")
        ->required();

    auto build_cmd = app.add_subcommand("build", "Builds a 💭++ project");
    auto run_cmd = app.add_subcommand("run", "Runs a 💤++ project");

    auto add_cmd =
        app.add_subcommand("add", "Adds a new dependency to a 🌧️++ project");
    std::string dep_name;
    add_cmd->add_option("dep_name", dep_name, "The name of the dependency.")
        ->required();

    auto sync_cmd =
        app.add_subcommand("sync", "Sync/install project dependencies");

    new_cmd->callback([&]() {
        spdlog::info("[🏗️] Creating new project '{}'", project_name);
        try {
            if (!std::filesystem::create_directory(project_name)) {
                spdlog::error(
                    "[🏗️] ❌ Something went wrong creating directories.");
                exit(1);
            }

            auto subdirs = {"src", "build", "build/includes", "build/lib"};
            for (const auto &dir : subdirs) {
                if (!std::filesystem::create_directory(
                        std::filesystem::path(project_name) / dir)) {
                    spdlog::warn(
                        "[🏗️] ⚠️ Something went wrong creating subdirectories.");
                }
            }

            AppConfig config;
            config.name = project_name;
            sync_config(serialise_config(config),
                        std::format("{}/{}", project_name, config_file_path));

            spdlog::info("[🏗️] ✅ Project '{}' created successfully!",
                         project_name);
        } catch (std::filesystem::filesystem_error &e) {
            spdlog::error(
                "[🏗️] ❌ Failed to create project. Filesystem error: {}",
                e.what());
            exit(1);
        }
    });

    build_cmd->callback([&]() {
        build();
    });

    run_cmd->callback([&]() {
        build();
        if (!std::filesystem::exists("dreamcpp.toml")) {
            spdlog::error("[⚒️] ❌ This... isn't a 🌌++ project.");
            exit(1);
        }
        auto app_config = parse_config_file(
            "dreamcpp.toml",
            std::filesystem::current_path().filename().string());
        if (!app_config.has_value())
            exit(1);
        auto runcmd = std::format("build/{}", app_config->name);
        spdlog::info("[⚒️] Running: {}", runcmd);
        system(runcmd.c_str());
    });

    add_cmd->callback([&]() {
        if (!dependency::add(dep_name)) {
            exit(1);
        }
    });

    sync_cmd->callback([&]() {
        if (!dependency::sync()) {
            exit(1);
        }
    });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    return 0;
}