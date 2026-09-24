// Containment over a trace with View: the call tree (per-event level/parent_id)
// and the folded flamegraph (name-path node frame), both from one buffered fold
// of one scan.
//
// usage: view_containment_example <trace.pfw.gz> [index_dir]

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/view.h>

#include <cstdio>
#include <string>

namespace views = dftracer::utils::trace::views;
namespace df = dftracer::utils::dataframe;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <trace.pfw.gz> [index_dir]\n", argv[0]);
        return 1;
    }
    const std::string file = argv[1];
    const std::string index_dir = argc > 2 ? argv[2] : "";

    views::View v = views::View::from_file(file, index_dir);
    views::ContainmentResult both = v.containment().collect().get();

    // Call tree: every event plus its nesting level and parent row index.
    const df::DataFrame& ct = both.call_tree;
    std::printf("call_tree: %lld events (columns: ",
                static_cast<long long>(ct.num_rows()));
    for (const std::string& n : ct.names) std::printf("%s ", n.c_str());
    std::printf(")\n");

    // Flamegraph: events folded by root-to-node name path.
    const df::DataFrame& fg = both.flamegraph;
    std::printf("flamegraph: %lld nodes\n",
                static_cast<long long>(fg.num_rows()));
    return 0;
}
