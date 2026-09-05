#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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
