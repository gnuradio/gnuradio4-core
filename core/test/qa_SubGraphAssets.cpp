#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/ut.hpp>

#ifndef __EMSCRIPTEN__
#include <httplib.h>
#endif

#include <build_configure.hpp>

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>

namespace ut = boost::ut;

namespace {

const std::string kAssetsDir  = std::string(TESTS_SOURCE_PATH) + "/assets";
const std::string kServerBase = "http://127.0.0.1:" + std::to_string(HTTP_SERVER_PORT);
const std::string kCacheDir   = gr::detail::YamlDefinitionsLoader::assetsCacheDir() + "/asset_cache";
const bool        kSkipRemote = true;

// sets or clears one environment variable for the length of a case, and puts back the value it found
class EnvironmentVariable {
    std::string                _name;
    std::optional<std::string> _found;

public:
    EnvironmentVariable(std::string name, const std::optional<std::string>& value) : _name(std::move(name)) {
        if (const char* found = std::getenv(_name.c_str()); found != nullptr) {
            _found = found;
        }
        if (value.has_value()) {
            ::setenv(_name.c_str(), value->c_str(), 1);
        } else {
            ::unsetenv(_name.c_str());
        }
    }
    ~EnvironmentVariable() {
        if (_found.has_value()) {
            ::setenv(_name.c_str(), _found->c_str(), 1);
        } else {
            ::unsetenv(_name.c_str());
        }
    }
    EnvironmentVariable(const EnvironmentVariable&)            = delete;
    EnvironmentVariable& operator=(const EnvironmentVariable&) = delete;
};

// makes a directory the working directory for the length of a case, and puts back the one it found
class WorkingDirectory {
    std::filesystem::path _found = std::filesystem::current_path();

public:
    explicit WorkingDirectory(const std::filesystem::path& directory) { std::filesystem::current_path(directory); }
    ~WorkingDirectory() {
        std::error_code ignored;
        std::filesystem::current_path(_found, ignored);
    }
    WorkingDirectory(const WorkingDirectory&)            = delete;
    WorkingDirectory& operator=(const WorkingDirectory&) = delete;
};

gr::PluginLoader makeLoader(const std::vector<std::string>& paths) {
    static gr::BlockRegistry     registry;
    static gr::SchedulerRegistry schedulerRegistry;
    return gr::PluginLoader(registry, schedulerRegistry, paths);
}

[[maybe_unused]]
gr::PluginLoader makeLoaderWithPlugins(const std::vector<std::string>& assetPaths) {
    static gr::BlockRegistry     registry;
    static gr::SchedulerRegistry schedulerRegistry;

    std::vector<std::string> allPaths;
    const char*              pluginDir = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES");
    allPaths.emplace_back(pluginDir != nullptr ? pluginDir : "plugins");
    allPaths.insert(allPaths.end(), assetPaths.begin(), assetPaths.end());
    return gr::PluginLoader(registry, schedulerRegistry, allPaths);
}

std::filesystem::path cachePathFor(std::string_view uri) { return std::filesystem::path(kCacheDir) / gr::detail::uriToCacheFilename(uri); }

std::filesystem::path versionPathFor(std::string_view uri) { return cachePathFor(uri).replace_extension(".version"); }

const std::string kDeltaStamp = "2020-06-15-12:00:00";

void clearCache() { std::filesystem::remove_all(kCacheDir); }

} // namespace

bool hasOneSubgraphBlock(const gr::property_map& definition) {
    try {
        const auto blocks = definition.at("blocks").value_or(gr::Tensor<gr::pmt::Value>{});
        if (blocks.size() != 1uz) {
            return false;
        }
        const auto block = blocks[0].value_or(gr::property_map{});
        return block.at("id") == "SUBGRAPH";
    } catch (...) {
        return false;
    }
}

// exportedInputPorts()/exportedOutputPorts() return a nested map:
//   { blockUniqueName -> { internalPortName -> { "exportedName" -> name } } }
// This helper collects all exported port names from that structure.
std::vector<std::string> collectExportedNames(const gr::property_map& portsMap) {
    std::vector<std::string> names;
    for (const auto& [_blockName, portInfoVal] : portsMap) {
        const auto* portMap = portInfoVal.get_if<gr::property_map>();
        if (!portMap) {
            continue;
        }
        for (const auto& [_internalName, exportInfoVal] : *portMap) {
            const auto* exportMap = exportInfoVal.get_if<gr::property_map>();
            if (!exportMap) {
                continue;
            }
            auto it = exportMap->find("exportedName");
            if (it != exportMap->end()) {
                names.emplace_back(it->second.value_or(std::string_view{}));
            }
        }
    }
    return names;
}

const boost::ut::suite AssetsLoadingTests = [] {
    using namespace ut;
    using namespace ut::literals;
    using namespace std::string_literals;

    // ── local tests ──────────────────────────────────────────────────────────

#ifndef __EMSCRIPTEN__
    // Local files are not supported in WASM
    "happy path: two blocks loaded from root_a"_test = [] {
        auto loader = makeLoader({kAssetsDir + "/root_a"});

        const auto AlphaBlock = "MyAlphaBlock";
        const auto BetaBlock  = "MyBetaBlock";

        const auto& defs = loader.definitionForBlockName();
        expect(eq(defs.size(), 2_ul));
        expect(defs.contains(AlphaBlock));
        expect(defs.contains(BetaBlock));
        expect(eq(defs.at(AlphaBlock).metadata.block_type, "MyAlphaBlock"s));
        expect(eq(defs.at(AlphaBlock).metadata.plugin_name, "AlphaPlugin"s));
        expect(eq(defs.at(AlphaBlock).metadata.plugin_author, "Test Author"s));
        expect(eq(defs.at(AlphaBlock).metadata.plugin_license, "LGPL-3.0"s));
        expect(eq(defs.at(AlphaBlock).metadata.plugin_version, "2024-01-15"s));
        expect(eq(defs.at(BetaBlock).metadata.block_type, "MyBetaBlock"s));
        expect(defs.at(BetaBlock).metadata.plugin_name.empty());

        expect(hasOneSubgraphBlock(defs.at(AlphaBlock).definition));
        expect(hasOneSubgraphBlock(defs.at(BetaBlock).definition));
    };

    "missing index.yaml: map stays empty, no crash"_test = [] {
        auto loader = makeLoader({kAssetsDir + "/nonexistent_root"});
        expect(loader.definitionForBlockName().empty());
    };

    "malformed index.yaml: silently skipped"_test = [] {
        auto loader = makeLoader({kAssetsDir + "/root_malformed"});
        expect(loader.definitionForBlockName().empty());
    };

    "index.yaml without assets key: silently skipped"_test = [] {
        auto loader = makeLoader({kAssetsDir + "/root_no_files_key"});
        expect(loader.definitionForBlockName().empty());
    };

    "multiple URI roots: each contributes independent entries"_test = [] {
        auto loader = makeLoader({kAssetsDir + "/root_a", kAssetsDir + "/root_b"});

        const auto AlphaBlock = "MyAlphaBlock";
        const auto BetaBlock  = "MyBetaBlock";
        const auto GammaBlock = "MyGammaBlock";

        const auto& defs = loader.definitionForBlockName();
        expect(eq(defs.size(), 3_ul));
        expect(defs.contains(AlphaBlock));
        expect(defs.contains(BetaBlock));
        expect(defs.contains(GammaBlock));

        expect(hasOneSubgraphBlock(defs.at(GammaBlock).definition));
    };

    // instantiate a YAML-defined composite block from an asset definition.
    // The definition embeds a SUBGRAPH with two chained multiply blocks whose
    // exported ports are named 'in' and 'out'.
#ifndef GR_TEST_WITHOUT_BLOCK_REGISTRY // the asset's interior blocks are resolved by name through the registry
    "instantiate: YAML asset creates a composite block with exported ports"_test = [] {
        auto loader = makeLoaderWithPlugins({kAssetsDir + "/root_a"});

        auto block = loader.instantiate("MyAlphaBlock");
        expect(block != nullptr) << "instantiate must return a non-null block";
        if (!block) {
            return;
        }

        const auto inputNames  = collectExportedNames(block->exportedInputPorts());
        const auto outputNames = collectExportedNames(block->exportedOutputPorts());
        expect(eq(inputNames.size(), 1uz)) << "expected one exported input port";
        expect(eq(outputNames.size(), 1uz)) << "expected one exported output port";
        expect(std::ranges::find(inputNames, "in") != inputNames.end()) << "exported input port must be named 'in'";
        expect(std::ranges::find(outputNames, "out") != outputNames.end()) << "exported output port must be named 'out'";
    };
#endif

    // a definition on disk is read on every load, so an edit reaches the loader immediately; the
    // cache holds only assets that were expensive to fetch, and there are none of those here
    "local asset: an edit between loads is served without clearing the cache"_test = [] {
        const auto root = std::filesystem::temp_directory_path() / "gr4_qa_local_asset_edit";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        // the `modified` stamp deliberately never moves: nothing about invalidation may depend on it
        const auto writeAsset = [&root](std::string_view blockType) {
            std::ofstream index(root / "index.yaml");
            index << "assets:\n  - file: block_edited.yaml\n    created: \"2024-01-01-00:00:00\"\n    modified: \"2024-01-15-10:00:00\"\n    block_type: " << blockType << "\n";
            std::ofstream asset(root / "block_edited.yaml");
            asset << "definition_metadata:\n  block_type: " << blockType << "\n";
        };

        writeAsset("FirstEdition");
        {
            auto first = makeLoader({root.string()});
            expect(first.definitionForBlockName().contains("FirstEdition"));
        }

        writeAsset("SecondEdition");
        auto        second = makeLoader({root.string()});
        const auto& defs   = second.definitionForBlockName();
        expect(defs.contains("SecondEdition")) << "the edited definition must reach the loader";
        expect(!defs.contains("FirstEdition")) << "the previous definition must not be served from a cache";

        expect(!std::filesystem::exists(cachePathFor(root.string() + "/block_edited.yaml"))) << "a local asset must not be cached";

        std::filesystem::remove_all(root);
    };

    // the three skips an index can produce, in one root with a sound definition beside them
    "a skipped asset is reported and counted, and the sound one still loads"_test = [] {
        const auto root = std::filesystem::temp_directory_path() / "gr4_qa_asset_skips";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        const auto listAsset = [](std::ofstream& index, std::string_view file) { //
            index << "  - file: " << file << "\n    created: \"2024-01-01-00:00:00\"\n    modified: \"2024-01-15-10:00:00\"\n";
        };
        {
            std::ofstream index(root / "index.yaml");
            index << "assets:\n";
            listAsset(index, "sound.yaml");
            listAsset(index, "absent.yaml"); // deliberately never written
            listAsset(index, "malformed.yaml");
            listAsset(index, "untyped.yaml");
        }
        {
            std::ofstream sound(root / "sound.yaml");
            sound << "definition_metadata:\n  block_type: SoundBlock\n";
            std::ofstream malformed(root / "malformed.yaml");
            malformed << ": this is not valid yaml: [unclosed\n  - malformed\n";
            std::ofstream untyped(root / "untyped.yaml");
            untyped << "definition_metadata:\n  plugin_name: UntypedPlugin\n";
        }

        auto        loader = makeLoader({root.string()});
        const auto& defs   = loader.definitionForBlockName();
        expect(eq(defs.size(), 1uz)) << "only the sound definition may register";
        expect(defs.contains("SoundBlock")) << "the sound definition must still resolve";
        expect(eq(loader.nSkippedAssets(), 3uz)) << "the unreadable, the malformed and the untyped asset must each count as a skip";

        std::filesystem::remove_all(root);
    };

    // a definitions root holding `RefusingDefinition`, whose one block has a type no registry holds
    static const auto writeRefusingDefinitionRoot = [] {
        const auto root = std::filesystem::temp_directory_path() / "gr4_qa_refusing_definition";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::ofstream index(root / "index.yaml");
        index << "assets:\n  - file: refusing.yaml\n    created: \"2024-01-01-00:00:00\"\n    modified: \"2024-01-15-10:00:00\"\n    block_type: RefusingDefinition\n";
        std::ofstream asset(root / "refusing.yaml");
        asset << "definition_metadata:\n  block_type: RefusingDefinition\nblocks:\n  - id: qa::NoSuchBlockIsRegistered\n    parameters:\n      name: inner\n";
        return root;
    };

    "instantiateOrError: a definition that cannot be built refuses with its reason"_test = [] {
        const auto root   = writeRefusingDefinitionRoot();
        auto       loader = makeLoader({root.string()});

        const auto made = loader.instantiateOrError("RefusingDefinition");
        expect(!made.has_value()) << "a definition that cannot be built is a refusal, not a miss";
        if (!made.has_value()) {
            expect(made.error().message.contains("qa::NoSuchBlockIsRegistered")) << made.error().message;
        }

        std::filesystem::remove_all(root);
    };

    "instantiateOrError: a name nothing registers is a null block, and instantiate returns null for both"_test = [] {
        const auto root   = writeRefusingDefinitionRoot();
        auto       loader = makeLoader({root.string()});

        const auto missed = loader.instantiateOrError("NothingRegistersThisName");
        expect(missed.has_value() && *missed == nullptr) << "a miss is a null block, not a refusal";
        expect(loader.instantiate("NothingRegistersThisName") == nullptr);
        expect(loader.instantiate("RefusingDefinition") == nullptr);

        std::filesystem::remove_all(root);
    };

    "loadGrc: a block whose definition refuses is reported with the definition's reason"_test = [] {
        const auto root   = writeRefusingDefinitionRoot();
        auto       loader = makeLoader({root.string()});

        std::string message;
        try {
            [[maybe_unused]] const auto graph = gr::loadGrc(loader, "blocks:\n  - id: RefusingDefinition\n    parameters:\n      name: refused\n");
        } catch (const gr::exception& refusal) {
            message = refusal.message;
        }
        expect(message.contains("'refused'") && message.contains("'RefusingDefinition'")) << "the block and its type" << message;
        expect(message.contains("qa::NoSuchBlockIsRegistered")) << "the reason the definition gave" << message;

        std::filesystem::remove_all(root);
    };

    // The compiled path of the arms without the variable is a directory the process may make, so the case holds for a
    // build whose own compiled path the process cannot use.
    "the cache directory is GR_DATA_CACHE_DIR from the environment when set and not empty, else the compiled path"_test = [] {
        using Origin                         = gr::DataCacheDirectory::Origin;
        const std::filesystem::path root     = std::filesystem::temp_directory_path() / "gr4_qa_cache_override";
        const std::string           override = (root / "override").string();
        const std::string           compiled = (root / "compiled").string();
        {
            const EnvironmentVariable variable("GR_DATA_CACHE_DIR", override);
            expect(eq(gr::detail::YamlDefinitionsLoader::assetsCacheDir(), override)) << "the environment must override the compiled path";
            expect(eq(gr::dataCacheDirectory(compiled).path, override));
        }
        {
            const EnvironmentVariable    empty("GR_DATA_CACHE_DIR", std::string());
            const gr::DataCacheDirectory chosen = gr::dataCacheDirectory(compiled);
            expect(chosen.origin == Origin::Compiled) << "an empty variable counts as unset";
            expect(eq(chosen.path, compiled));
        }
        const EnvironmentVariable    variable("GR_DATA_CACHE_DIR", std::nullopt);
        const gr::DataCacheDirectory chosen = gr::dataCacheDirectory(compiled);
        expect(chosen.origin == Origin::Compiled) << "without the variable the compiled path must apply";
        expect(eq(chosen.path, compiled));
        expect(override != compiled) << "the two arms must name different directories";
    };

    // Each arm names a compiled directory in a tree of its own: one under a regular file, one under a directory the
    // process may not write to, one under a broken symbolic link, and one the process may make. The case makes the
    // asset_cache subdirectory of a chosen directory, as the loader does.
    "the user's cache directory takes over when the compiled one cannot be made, and GR_DATA_CACHE_DIR overrides both"_test = [] {
        using Origin              = gr::DataCacheDirectory::Origin;
        namespace fs              = std::filesystem;
        const fs::path  root      = fs::temp_directory_path() / "gr4_qa_cache_fallback";
        const fs::path  file      = root / "file";
        const fs::path  locked    = root / "locked";
        const fs::path  xdg       = root / "xdg";
        const fs::path  home      = root / "home";
        const fs::path  makeable  = root / "compiled" / "cache";
        const fs::path  underFile = file / "cache";
        const fs::path  underLock = locked / "cache";
        const fs::path  xdgCache  = xdg / "gnuradio4" / "cache";
        const fs::path  homeCache = home / ".cache" / "gnuradio4" / "cache";
        const fs::path  dangling  = root / "dangling";
        std::error_code ignored;
        fs::remove_all(root, ignored);
        fs::create_directories(locked);
        std::ofstream(file) << "a file where the compiled directory's parent would be\n";
        fs::create_symlink(root / "absent", dangling);

        const EnvironmentVariable variable("GR_DATA_CACHE_DIR", std::nullopt);
        const EnvironmentVariable xdgVariable("XDG_CACHE_HOME", xdg.string());
        const EnvironmentVariable homeVariable("HOME", home.string());

        const gr::DataCacheDirectory compiled = gr::dataCacheDirectory(makeable.string());
        expect(compiled.origin == Origin::Compiled) << "the instrument: a compiled directory that can be made is taken";
        expect(eq(compiled.path, makeable.string()));
        expect(compiled.whyNotCompiled.empty()) << compiled.whyNotCompiled;
        expect(!fs::exists(makeable)) << "the choice made a directory";
        std::error_code madeCompiled;
        fs::create_directories(fs::path(compiled.path) / "asset_cache", madeCompiled);
        expect(!madeCompiled) << madeCompiled.message();

        const gr::DataCacheDirectory fromXdg = gr::dataCacheDirectory(underFile.string());
        expect(fromXdg.origin == Origin::User);
        expect(eq(fromXdg.path, xdgCache.string())) << "the user's cache directory is under XDG_CACHE_HOME";
        expect(eq(fromXdg.compiled, underFile.string()));
        expect(eq(fromXdg.whyNotCompiled, std::format("{} is not a directory", file.string())));
        std::error_code madeXdg;
        fs::create_directories(fs::path(fromXdg.path) / "asset_cache", madeXdg);
        expect(!madeXdg) << madeXdg.message();

        const gr::DataCacheDirectory fromLink = gr::dataCacheDirectory((dangling / "cache").string());
        expect(fromLink.origin == Origin::User) << "a directory cannot be made under a broken symbolic link";
        expect(eq(fromLink.path, xdgCache.string()));
        expect(eq(fromLink.whyNotCompiled, std::format("{} is a broken symbolic link", dangling.string())));

        {
            const EnvironmentVariable    noXdg("XDG_CACHE_HOME", std::nullopt);
            const gr::DataCacheDirectory fromHome = gr::dataCacheDirectory(underFile.string());
            expect(fromHome.origin == Origin::User);
            expect(eq(fromHome.path, homeCache.string())) << "without XDG_CACHE_HOME the user's cache directory is under HOME";
        }

        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
        if (std::ofstream(locked / "probe")) {
            std::println("the process writes to a directory without write permission; the unwritable-directory arm has no subject here");
        } else {
            const gr::DataCacheDirectory fromLocked = gr::dataCacheDirectory(underLock.string());
            expect(fromLocked.origin == Origin::User);
            expect(eq(fromLocked.path, xdgCache.string()));
            expect(fromLocked.whyNotCompiled.starts_with(std::format("{}: ", locked.string()))) << fromLocked.whyNotCompiled;
        }
        fs::permissions(locked, fs::perms::owner_all, ignored);

        {
            const EnvironmentVariable    blockedXdg("XDG_CACHE_HOME", (file / "xdg").string());
            const gr::DataCacheDirectory none = gr::dataCacheDirectory(underFile.string());
            expect(none.origin == Origin::None) << "neither directory can be made";
            expect(none.path.empty());
            expect(eq(none.user, (file / "xdg" / "gnuradio4" / "cache").string()));
            expect(eq(none.whyNotUser, std::format("{} is not a directory", file.string())));
        }

        {
            const EnvironmentVariable    named("GR_DATA_CACHE_DIR", (file / "named").string());
            const gr::DataCacheDirectory fromEnvironment = gr::dataCacheDirectory(underFile.string());
            expect(fromEnvironment.origin == Origin::Environment) << "the variable is taken as given, whether or not it can be made";
            expect(eq(fromEnvironment.path, (file / "named").string()));
            expect(fromEnvironment.whyNotCompiled.empty()) << "the compiled directory was never considered";
        }

        fs::remove_all(root, ignored);
    };

    // The working directory is a tree of the case's own, and with HOME and XDG_CACHE_HOME unset a refused compiled path
    // selects no directory at all.
    "a relative compiled cache directory is taken under the working directory, whether it exists or not"_test = [] {
        using Origin         = gr::DataCacheDirectory::Origin;
        namespace fs         = std::filesystem;
        const fs::path  root = fs::temp_directory_path() / "gr4_qa_cache_relative";
        std::error_code ignored;
        fs::remove_all(root, ignored);
        fs::create_directories(root / "existing" / "cache");
        std::ofstream(root / "file") << "a file where the compiled directory's parent would be\n";

        {
            const WorkingDirectory    workingDirectory(root);
            const EnvironmentVariable variable("GR_DATA_CACHE_DIR", std::nullopt);
            const EnvironmentVariable xdgVariable("XDG_CACHE_HOME", std::nullopt);
            const EnvironmentVariable homeVariable("HOME", std::nullopt);

            for (const std::string relative : {"existing/cache", "missing/cache", "cache"}) {
                const gr::DataCacheDirectory chosen = gr::dataCacheDirectory(relative);
                expect(chosen.origin == Origin::Compiled) << relative << ": " << chosen.whyNotCompiled;
                expect(eq(chosen.path, relative));
                expect(chosen.whyNotCompiled.empty()) << chosen.whyNotCompiled;
            }
            expect(!fs::exists(root / "missing")) << "the choice made a directory";
            std::error_code made;
            fs::create_directories(fs::path("missing/cache") / "asset_cache", made);
            expect(!made) << made.message();
            expect(fs::is_directory(root / "missing" / "cache" / "asset_cache")) << "the relative directory is made under the working directory";

            const gr::DataCacheDirectory refused = gr::dataCacheDirectory("file/cache");
            expect(refused.origin == Origin::None) << "the instrument: a relative directory under a regular file cannot be made";
            expect(refused.path.empty());
            expect(eq(refused.whyNotCompiled, std::string("file is not a directory")));
        }
        fs::remove_all(root, ignored);
    };

    // ── remote tests (server started by CMake fixture) ────────────────────────

#endif

    "remote happy path: two blocks loaded via http from root_a"_test = [] {
        if (kSkipRemote) {
            return;
        }
        clearCache();
        auto loader = makeLoader({kServerBase + "/root_a"});

        const auto AlphaBlock = "MyAlphaBlock";
        const auto BetaBlock  = "MyBetaBlock";

        const auto& defs = loader.definitionForBlockName();
        expect(eq(defs.size(), 2_ul));
        expect(defs.contains(AlphaBlock));
        expect(defs.contains(BetaBlock));

        expect(hasOneSubgraphBlock(defs.at(AlphaBlock).definition));
        expect(hasOneSubgraphBlock(defs.at(BetaBlock).definition));
    };

    "remote missing index.yaml: map stays empty, no crash"_test = [] {
        if (kSkipRemote) {
            return;
        }
        clearCache();
        auto loader = makeLoader({kServerBase + "/nonexistent_root"});
        expect(loader.definitionForBlockName().empty());
    };

    "remote multiple URI roots: each contributes independent entries"_test = [] {
        if (kSkipRemote) {
            return;
        }
        clearCache();
        auto loader = makeLoader({kServerBase + "/root_a", kServerBase + "/root_b"});

        const auto AlphaBlock = "MyAlphaBlock";
        const auto BetaBlock  = "MyBetaBlock";
        const auto GammaBlock = "MyGammaBlock";

        const auto& defs = loader.definitionForBlockName();
        expect(eq(defs.size(), 3_ul));
        expect(defs.contains(AlphaBlock));
        expect(defs.contains(BetaBlock));
        expect(defs.contains(GammaBlock));
    };

    // ── cache tests ───────────────────────────────────────────────────────────

    "cache: loading remote asset creates a cache file"_test = [] {
        if (kSkipRemote) {
            return;
        }
        clearCache();
        const std::string blockUri = kServerBase + "/root_cache/block_delta.yaml";

        auto loader = makeLoader({kServerBase + "/root_cache"});

        const auto DeltaBlock = "MyDeltaBlock";

        expect(loader.definitionForBlockName().contains(DeltaBlock));
        expect(std::filesystem::exists(cachePathFor(blockUri)));
    };

    "cache: fresh cache is used instead of remote"_test = [] {
        if (kSkipRemote) {
            return;
        }
        clearCache();
        const std::string blockUri = kServerBase + "/root_cache/block_delta.yaml";

        // First load: populates cache.
        {
            auto loader = makeLoader({kServerBase + "/root_cache"});
            expect(std::filesystem::exists(cachePathFor(blockUri)));
        }

        // Overwrite the cache file with a distinguishable block type, leaving the sidecar stamp at
        // the value index.yaml carries, so the cached copy is the current version of that asset.
        {
            std::ofstream f(cachePathFor(blockUri));
            f << "definition_metadata:\n  block_type: CachedDeltaBlock\n";
        }
        {
            std::ofstream f(versionPathFor(blockUri));
            f << kDeltaStamp;
        }

        auto loader = makeLoader({kServerBase + "/root_cache"});

        const auto DeltaBlock       = "MyDeltaBlock";
        const auto CachedDeltaBlock = "CachedDeltaBlock";

        // Should have read from cache, not remote.
        expect(loader.definitionForBlockName().contains(CachedDeltaBlock));
        expect(!loader.definitionForBlockName().contains(DeltaBlock));
    };

    "cache: stale cache is refreshed from remote"_test = [] {
        if (kSkipRemote) {
            return;
        }
        clearCache();
        const std::string blockUri = kServerBase + "/root_cache/block_delta.yaml";

        // Pre-seed the cache with content stamped for a version index.yaml no longer names.
        std::filesystem::create_directories(kCacheDir);
        {
            std::ofstream f(cachePathFor(blockUri));
            f << "definition_metadata:\n  block_type: StaleBlock\n";
        }
        {
            std::ofstream f(versionPathFor(blockUri));
            f << "2019-01-01-00:00:00";
        }

        auto loader = makeLoader({kServerBase + "/root_cache"});

        const auto DeltaBlock = "MyDeltaBlock";
        const auto StaleBlock = "StaleBlock";

        // Stale cache should have been ignored; remote content loaded.
        expect(loader.definitionForBlockName().contains(DeltaBlock));
        expect(!loader.definitionForBlockName().contains(StaleBlock));
        // Cache should now be refreshed: the sidecar carries the stamp index.yaml names.
        const auto refreshed = gr::detail::readUriToString(versionPathFor(blockUri).string());
        expect(refreshed.has_value() && *refreshed == kDeltaStamp);
    };
};

int main() {
#ifndef __EMSCRIPTEN__
    httplib::Server httpServer;
    httpServer.set_mount_point("/", kAssetsDir);
    auto serverThread = std::thread([&httpServer] { httpServer.listen("127.0.0.1", HTTP_SERVER_PORT); });
    httpServer.wait_until_ready();
#else
    // Poll the pre-js HTTP server until a known asset responds. This replaces a
    // blind sleep; Node's http.createServer().listen(...) has no synchronous
    // ready signal, so probing is the only deterministic option here.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto content = gr::detail::readUriToString(kServerBase + "/root_a/index.yaml"); content && !content->empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
#endif

    const int result = boost::ut::cfg<boost::ut::override>.run();

#ifndef __EMSCRIPTEN__
    httpServer.stop();
    serverThread.join();
#endif
    return result;
}
