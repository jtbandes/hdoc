// Copyright 2019-2021 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include "frontend/Frontend.hpp"

#include "argparse.hpp"
#include "process.hpp"
#include "spdlog/spdlog.h"
#include "toml.hpp"
#include "version.hpp"

// These files are generated by meson at build-time using `xxd -i`
extern uint8_t  ___site_content_oss_md[];   ///< Contents of the OSS attribution file
extern uint64_t ___site_content_oss_md_len; ///< Length of the OSS attribution file

/// @brief Parse the CLI and configuration file
hdoc::frontend::Frontend::Frontend(int argc, char** argv, hdoc::types::Config* cfg) {
  cfg->hdocVersion = HDOC_VERSION;
  argparse::ArgumentParser program("hdoc", cfg->hdocVersion);
  program.add_argument("-v", "--verbose")
      .help("Whether to use verbose output")
      .default_value(false)
      .implicit_value(true);

  program.add_argument("--oss").help("Show open source notices").default_value(false).implicit_value(true);

  // Parse command line arguments
  try {
    program.parse_args(argc, argv);
  } catch (const std::runtime_error& err) {
    spdlog::error("Error found while parsing command line arguments: {}", err.what());
    return;
  }

  // Display open source attribution by dumping the contents of the OSS attribution file and exit
  if (program.get<bool>("--oss") == true) {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Displaying OSS attribution.\n{}", ___site_content_oss_md);
    std::exit(0);
  }

  // Toggle verbosity depending on state of command line switch
  if (program.get<bool>("--verbose") == true) {
    spdlog::set_level(spdlog::level::info);
  } else {
    spdlog::set_level(spdlog::level::warn);
  }

  // Check that the current directory contains a .hdoc.toml file
  cfg->rootDir = std::filesystem::current_path();
  if (!std::filesystem::is_regular_file(cfg->rootDir / ".hdoc.toml")) {
    spdlog::error("Current directory doesn't contain an .hdoc.toml file.");
    return;
  }

  // Parse configuration file
  toml::table toml;
  try {
    toml = toml::parse_file((cfg->rootDir / ".hdoc.toml").string());
  } catch (const toml::parse_error& err) {
    spdlog::error("Error in configuration file: {} ({}:{}:{})",
                  err.description(),
                  *err.source().path,
                  err.source().begin.line,
                  err.source().begin.column);
    return;
  }

  // Check that buildDir is a directory and contains a compile_commands.json file
  cfg->compileCommandsJSON = std::filesystem::path(toml["paths"]["compile_commands"].value_or(""));
  if (std::filesystem::is_regular_file(cfg->compileCommandsJSON) == false) {
    spdlog::error("{} is not a valid file.", cfg->compileCommandsJSON.string());
    return;
  }

  // Check if the output directory is specified. Print a warning if it's specified for client versions of hdoc,
  // and throw an error if it's specified for full versions of hdoc because we need to know where to save the docs.
  std::optional<std::string_view> output_dir = toml["paths"]["output_dir"].value<std::string_view>();
  if (output_dir != std::nullopt && cfg->binaryType == hdoc::types::BinaryType::Client) {
    spdlog::warn(
        "'output_dir' specified in .hdoc.toml but you are running a version of hdoc downloaded from hdoc.io. "
        "Your documentation will be uploaded to docs.hdoc.io instead of being saved locally.");
  } else if (output_dir == std::nullopt && cfg->binaryType == hdoc::types::BinaryType::Full) {
    spdlog::error(
        "No 'output_dir' specified in .hdoc.toml. It is required so that documentation can be saved locally.");
    return;
  }

  // Get other arguments from the .hdoc.toml file.
  cfg->outputDir      = std::filesystem::path(toml["paths"]["output_dir"].value_or(""));
  cfg->projectName    = toml["project"]["name"].value_or("");
  cfg->projectVersion = toml["project"]["version"].value_or("");
  cfg->gitRepoURL     = toml["project"]["git_repo_url"].value_or("");
  if (cfg->projectName == "") {
    spdlog::error("Project name in .hdoc.toml is empty, not a string, or invalid.");
    return;
  }
  if (cfg->projectVersion == "") {
    spdlog::error("Project version in .hdoc.toml is empty, not a string, or invalid.");
    return;
  }
  if (cfg->gitRepoURL != "" && cfg->gitRepoURL.back() != '/') {
    spdlog::error("Git repo URL is missing the mandatory trailing slash: {}", cfg->gitRepoURL);
    return;
  }

  // If numThreads is not an integer, return an error
  if (toml["project"]["num_threads"].type() != toml::node_type::integer &&
      toml["project"]["num_threads"].type() != toml::node_type::none) {
    spdlog::error("Number of threads in .hdoc.toml is not an integer.");
    return;
  }
  // If numThreads wasn't defined, use the default value of 0 (index with all available threads)
  if (toml["project"]["num_threads"].type() == toml::node_type::none) {
    cfg->numThreads = 0;
  }
  // Otherwise it must be defined and be an integer, so check if it's valid and set if so
  else {
    int64_t rawNumThreads = toml["project"]["num_threads"].as_integer()->get();
    if (rawNumThreads < 0) {
      spdlog::error("Number of threads must be a positive integer greater than or equal to 0.");
      return;
    }
    cfg->numThreads = rawNumThreads;
  }

  // Determine the compiler's builtin include paths and add them to the list
  cfg->useSystemIncludes = toml["includes"]["use_system_includes"].value_or(true);
  if (cfg->useSystemIncludes == true) {
    std::string compilerIncludes;
    // Get compiler builtin include paths on stdout (only lines with leading space are the include paths)
    // then remove that leading space
    TinyProcessLib::Process proc(
        "c++ -Wp,-v -x c++ - -fsyntax-only < /dev/null 2>&1 | grep '^\\ ' | tr -d ' '",
        "",
        [&](const char* bytes, size_t n) { compilerIncludes += std::string(bytes, n); },
        nullptr);
    // Split compilerIncludes into individual strings instead of a big newline'd string
    if (proc.get_exit_status() == 0 && compilerIncludes != "") {
      size_t last = 0;
      size_t next = 0;
      while ((next = compilerIncludes.find('\n', last)) != std::string::npos) {
        cfg->includePaths.push_back(compilerIncludes.substr(last, next - last));
        last = next + 1;
      }
    } else {
      spdlog::warn("Failed to determine the compiler's default include paths. Continuing without them.");
    }
  }

  // Get additional include paths from toml config file
  if (const auto& includes = toml["includes"]["paths"].as_array()) {
    for (const auto& inc : *includes) {
      std::string s = inc.value_or(std::string(""));
      if (s == "") {
        spdlog::warn("An include path from .hdoc.toml was malformed, ignoring it.");
        continue;
      }
      cfg->includePaths.push_back(s);
    }
  }

  // Get substrings of paths that should be ignored
  if (const auto& ignores = toml["ignore"]["paths"].as_array()) {
    for (const auto& i : *ignores) {
      std::string s = i.value_or(std::string(""));
      if (s == "") {
        spdlog::warn("An ignore directive from .hdoc.toml was malformed, ignoring it.");
        continue;
      }
      cfg->ignorePaths.push_back(s);
    }
  }

  // Collect paths to markdown files
  cfg->homepage = std::filesystem::path(toml["pages"]["homepage"].value_or(""));
  if (const auto& mdPaths = toml["pages"]["paths"].as_array()) {
    for (const auto& md : *mdPaths) {
      std::string s = md.value_or(std::string(""));
      if (s == "") {
        spdlog::warn("A path to a markdown file in .hdoc.toml was malformed, ignoring it.");
        continue;
      }
      std::filesystem::path mdPath(s);
      if (std::filesystem::exists(mdPath) == false || std::filesystem::is_regular_file(mdPath) == false) {
        spdlog::warn("A path to a markdown file in .hdoc.toml either doesn't exist or isn't a file, ignoring it.");
        continue;
      }
      cfg->mdPaths.push_back(mdPath);
    }
  }

  // Get the current timestamp
  const auto        time_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::stringstream ss;
  ss << std::put_time(std::gmtime(&time_t), "%FT%T UTC");
  cfg->timestamp = ss.str();

  cfg->initialized = true;

  // Dump state of the Config object
  spdlog::info("hdoc version: {}", cfg->hdocVersion);
  spdlog::info("Timestamp: {}", cfg->timestamp);
  spdlog::info("Root directory: {}", cfg->rootDir.string());
  if (cfg->binaryType != hdoc::types::BinaryType::Client) {
    spdlog::info("Output directory: {}", cfg->outputDir.string());
  }
  spdlog::info("Project name: {}", cfg->projectName);
  spdlog::info("Project version: {}", cfg->projectVersion);
  spdlog::info("Indexing using {} threads",
               cfg->numThreads == 0 ? std::string("all") : std::to_string(cfg->numThreads));
}
