#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/factory.h>
#include <dftracer/utils/call_tree/internal/node.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/mpi/serializable.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace dftracer::utils::call_tree::internal;

TEST_CASE("CallTree - Basic construction and initialization") {
    CallTree tree;

    SUBCASE("Initialize tree") {
        tree.initialize();
        // Tree is initialized but empty - get returns nullptr for non-existent
        // keys
        CHECK(tree.get(1234, 5678) == nullptr);
        // Using operator[] creates the entry
        ProcessCallTree& pct = tree[ProcessKey(1234, 5678)];
        CHECK(pct.key.pid == 1234);
        CHECK(pct.key.tid == 5678);
        // Now get() should return the entry
        CHECK(tree.get(1234, 5678) != nullptr);
    }

    SUBCASE("Cleanup after initialize") {
        tree.initialize();
        tree.cleanup();
        // After cleanup, tree should still be usable but empty
    }
}

TEST_CASE("ProcessKey - Construction and comparison") {
    SUBCASE("Default construction") {
        ProcessKey key1;
        ProcessKey key2;
        CHECK(key1 == key2);
    }

    SUBCASE("Construction with pid and tid") {
        ProcessKey key1(1234, 5678);
        CHECK(key1.pid == 1234);
        CHECK(key1.tid == 5678);
    }

    SUBCASE("Comparison operators") {
        ProcessKey key1(100, 200);
        ProcessKey key2(100, 200);
        ProcessKey key3(100, 201);
        ProcessKey key4(101, 200);

        CHECK(key1 == key2);
        CHECK_FALSE(key1 == key3);
        CHECK_FALSE(key1 == key4);
        CHECK(key1 != key3);
        CHECK(key1 != key4);
    }

    SUBCASE("Hash consistency") {
        ProcessKey key1(100, 200, 300);
        ProcessKey key2(100, 200, 300);
        ProcessKey key3(100, 201, 300);

        // Same keys should hash the same
        std::hash<ProcessKey> hasher;
        CHECK(hasher(key1) == hasher(key2));
        // Different keys may hash differently (not guaranteed but likely)
        CHECK(hasher(key1) != hasher(key3));
    }
}

TEST_CASE("CallTreeFactory - Create nodes") {
    CallTreeFactory factory;
    factory.initialize();

    SUBCASE("Create simple node") {
        auto node =
            factory.create_node(1, "test_function", "function", 1000, 500, 0);

        CHECK(node != nullptr);
        CHECK(node->get_id() == 1);
        CHECK(node->get_name() == "test_function");
        CHECK(node->get_category() == "function");
        CHECK(node->get_start_time() == 1000);
        CHECK(node->get_duration() == 500);
        CHECK(node->get_level() == 0);
    }

    SUBCASE("Create node with arguments") {
        dftracer::utils::call_tree::internal::ArgsMap args;
        args.set_valid(true);
        args.insert("arg1", std::string("value1"));
        args.insert("arg2", std::string("value2"));

        auto node = factory.create_node(2, "test_func", "category", 2000, 1000,
                                        1, std::move(args));

        CHECK(node != nullptr);
        CHECK(node->get_args().raw().size() == 2);
        CHECK(node->get_args()["arg1"].get<std::string>() == "value1");
        CHECK(node->get_args()["arg2"].get<std::string>() == "value2");
    }

    SUBCASE("Multiple nodes with unique IDs") {
        auto node1 = factory.create_node(1, "func1", "cat1", 100, 50, 0);
        auto node2 = factory.create_node(2, "func2", "cat2", 200, 60, 1);

        CHECK(node1->get_id() != node2->get_id());
        CHECK(factory.get_node_count() == 2);
    }

    factory.cleanup();
}

TEST_CASE("CallNode - Parent-child relationships") {
    CallTreeFactory factory;
    factory.initialize();

    SUBCASE("Set parent ID") {
        auto node =
            factory.create_node(1, "child_func", "function", 1000, 500, 1);

        node->set_parent_id(100);
        CHECK(node->get_parent_id() == 100);
    }

    SUBCASE("Add children") {
        auto parent =
            factory.create_node(1, "parent_func", "function", 1000, 1000, 0);

        parent->add_child(2);
        parent->add_child(3);
        parent->add_child(4);

        const auto& children = parent->get_children();
        CHECK(children.size() == 3);
        CHECK(children[0] == 2);
        CHECK(children[1] == 3);
        CHECK(children[2] == 4);
    }

    factory.cleanup();
}

TEST_CASE("ProcessCallTree - Basic operations") {
    ProcessKey key(1234, 5678);
    ProcessCallTree graph;
    graph.key = key;

    SUBCASE("Process key is set correctly") {
        CHECK(graph.key.pid == 1234);
        CHECK(graph.key.tid == 5678);
    }

    SUBCASE("Start with empty root calls") { CHECK(graph.root_calls.empty()); }

    SUBCASE("Start with empty call map") { CHECK(graph.calls.empty()); }

    SUBCASE("Add root call") {
        graph.root_calls.push_back(1);
        CHECK(graph.root_calls.size() == 1);
        CHECK(graph.root_calls[0] == 1);
    }

    SUBCASE("Add call sequence") {
        graph.call_sequence.push_back(1);
        graph.call_sequence.push_back(2);
        graph.call_sequence.push_back(3);

        CHECK(graph.call_sequence.size() == 3);
    }
}

TEST_CASE("CallTree - Process graph operations") {
    CallTree tree;
    tree.initialize();

    SUBCASE("Access process graph creates it if not exists") {
        ProcessKey key(1000, 2000);
        auto& graph = tree[key];

        CHECK(graph.key == key);
    }

    SUBCASE("Multiple process graphs") {
        ProcessKey key1(1000, 2000);
        ProcessKey key2(1000, 3000);
        ProcessKey key3(2000, 2000);

        auto& graph1 = tree[key1];
        auto& graph2 = tree[key2];
        auto& graph3 = tree[key3];

        CHECK(graph1.key == key1);
        CHECK(graph2.key == key2);
        CHECK(graph3.key == key3);
    }

    SUBCASE("Get factory") {
        auto& factory = tree.get_factory();
        auto node = factory.create_node(1, "test", "cat", 100, 50, 0);
        CHECK(node != nullptr);
    }

    tree.cleanup();
}

TEST_CASE("CallTree - Integration test with nodes") {
    CallTree tree;
    tree.initialize();

    ProcessKey key(1234, 5678);
    auto& graph = tree[key];
    auto& factory = tree.get_factory();

    SUBCASE("Build simple call tree") {
        // Create root node
        auto root = factory.create_node(1, "main", "function", 0, 1000, 0);

        // Create child nodes
        auto child1 = factory.create_node(2, "func1", "function", 100, 200, 1);
        auto child2 = factory.create_node(3, "func2", "function", 400, 300, 1);

        // Set up relationships
        child1->set_parent_id(1);
        child2->set_parent_id(1);
        root->add_child(2);
        root->add_child(3);

        // Add to graph
        graph.calls[1] = root;
        graph.calls[2] = child1;
        graph.calls[3] = child2;
        graph.root_calls.push_back(1);
        graph.call_sequence = {1, 2, 3};

        // Verify structure
        CHECK(graph.root_calls.size() == 1);
        CHECK(graph.calls.size() == 3);
        CHECK(graph.call_sequence.size() == 3);
        CHECK(root->get_children().size() == 2);
    }

    tree.cleanup();
}

// ============================================================================
// Save / Load round-trips
// ============================================================================

namespace {

using dftracer::utils::CoroScope;
using dftracer::utils::make_task;
using dftracer::utils::Pipeline;
namespace coro = dftracer::utils::coro;
using dftracer::utils::call_tree::load_arrow;
using dftracer::utils::call_tree::load_binary;
using dftracer::utils::call_tree::save_arrow;
using dftracer::utils::call_tree::save_binary;

std::unique_ptr<CallTree> make_fixture() {
    auto tree = std::make_unique<CallTree>();
    tree->initialize();

    auto add_proc = [&](std::uint32_t pid, std::uint32_t tid,
                        std::uint32_t pkid) {
        ProcessKey key(pid, tid, pkid);
        dftracer::utils::utilities::composites::dft::ArgsMap a1;
        a1.set_valid(true);
        a1.insert("level", static_cast<std::uint64_t>(0));
        a1.insert("tid", static_cast<std::uint64_t>(tid));
        a1.insert("fhash", std::string("abc123"));
        auto root = tree->get_factory().create_node(1, "main", "function", 0,
                                                    1000, 0, std::move(a1));
        dftracer::utils::utilities::composites::dft::ArgsMap a2;
        a2.set_valid(true);
        a2.insert("level", static_cast<std::uint64_t>(1));
        a2.insert("tid", static_cast<std::uint64_t>(tid));
        auto child = tree->get_factory().create_node(
            2, "child", "function", 100, 500, 1, std::move(a2));
        child->set_parent_id(1);
        root->add_child(2);
        tree->add_call(key, root);
        tree->add_call(key, child);
        auto* pgraph = tree->get(key);
        pgraph->root_calls.push_back(1);
        pgraph->call_sequence = {1, 2};
    };
    add_proc(100, 200, 0);
    add_proc(101, 201, 0);
    return tree;
}

template <typename SaveFn, typename LoadFn>
std::unique_ptr<CallTree> roundtrip(const CallTree& src,
                                    const std::string& path, SaveFn save_fn,
                                    LoadFn load_fn, bool* save_ok_out,
                                    bool* load_ok_out) {
    struct Ctx {
        const CallTree* src;
        std::string path;
        std::unique_ptr<CallTree> loaded;
        bool save_ok = false;
        bool load_ok = false;
    };
    Ctx ctx{&src, path, nullptr, false, false};

    Pipeline pipeline;
    auto run = make_task(
        [&ctx, save_fn, load_fn](CoroScope& scope) -> coro::CoroTask<void> {
            ctx.save_ok = co_await save_fn(&scope, *ctx.src, ctx.path);
            if (ctx.save_ok) {
                ctx.loaded = co_await load_fn(&scope, ctx.path);
                ctx.load_ok = (ctx.loaded != nullptr);
            }
        },
        "save_load");
    pipeline.set_source(run);
    pipeline.set_destination(run);
    pipeline.execute();
    *save_ok_out = ctx.save_ok;
    *load_ok_out = ctx.load_ok;
    return std::move(ctx.loaded);
}

void check_structure_matches(const CallTree& src, const CallTree& loaded) {
    auto src_keys = const_cast<CallTree&>(src).keys();
    auto loaded_keys = const_cast<CallTree&>(loaded).keys();
    CHECK(src_keys.size() == loaded_keys.size());

    for (const auto& key : src_keys) {
        auto* sg = const_cast<CallTree&>(src).get(key);
        auto* lg = const_cast<CallTree&>(loaded).get(key);
        REQUIRE(sg != nullptr);
        REQUIRE(lg != nullptr);
        CHECK(sg->calls.size() == lg->calls.size());
        CHECK(sg->root_calls.size() == lg->root_calls.size());
        CHECK(sg->call_sequence.size() == lg->call_sequence.size());
        for (const auto& [id, sn] : sg->calls) {
            auto it = lg->calls.find(id);
            REQUIRE(it != lg->calls.end());
            const auto& ln = it->second;
            CHECK(sn->get_name() == ln->get_name());
            CHECK(sn->get_category() == ln->get_category());
            CHECK(sn->get_start_time() == ln->get_start_time());
            CHECK(sn->get_duration() == ln->get_duration());
            CHECK(sn->get_level() == ln->get_level());
            CHECK(sn->get_parent_id() == ln->get_parent_id());
            CHECK(sn->get_children().size() == ln->get_children().size());
            CHECK(sn->get_args().raw().size() == ln->get_args().raw().size());
        }
    }
}

}  // namespace

TEST_CASE("CallTree - custom binary save/load round-trip") {
    auto tmp = fs::temp_directory_path() /
               ("ct_binary_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto path = (tmp / "tree.bin").string();

    auto src = make_fixture();
    bool save_ok = false, load_ok = false;
    auto loaded =
        roundtrip(*src, path, save_binary, load_binary, &save_ok, &load_ok);
    REQUIRE(save_ok);
    REQUIRE(load_ok);
    REQUIRE(loaded != nullptr);
    check_structure_matches(*src, *loaded);

    fs::remove_all(tmp);
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
TEST_CASE("CallTree - arrow IPC save/load round-trip") {
    auto tmp = fs::temp_directory_path() /
               ("ct_arrow_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto path = (tmp / "tree.arrow").string();

    auto src = make_fixture();
    bool save_ok = false, load_ok = false;
    auto loaded =
        roundtrip(*src, path, save_arrow, load_arrow, &save_ok, &load_ok);
    REQUIRE(save_ok);
    REQUIRE(load_ok);
    REQUIRE(loaded != nullptr);
    check_structure_matches(*src, *loaded);

    fs::remove_all(tmp);
}
#endif
