// A libFuzzer-free driver for the harnesses in this directory.
//
// Two jobs, both of which libFuzzer cannot do:
//
//   * Replay a saved input under gdb, valgrind, or a plain profiler. libFuzzer
//     installs its own signal handlers and its own main, which gets in the way of
//     every one of those. Reproducing a crash should not require the fuzzer.
//   * Answer "is this memory growth ours or libFuzzer's?" — the question that
//     prompted this file. libFuzzer's RSS includes its corpus, its feature
//     bookkeeping, and its mutator. A harness driven directly, on a fixed input,
//     with no mutation, isolates the harness.
//
// Usage:
//   standalone_<target> <file>...            run each file once
//   standalone_<target> -n <count> <file>... run each file <count> times
//
// With -n, RSS is sampled from /proc/self/statm at the start, at each tenth of
// the run, and at the end, so a leak in the harness shows as a rising line and
// allocator behaviour shows as a flat one.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

// Resident set size in KiB, or 0 if it cannot be read. Field two of
// /proc/self/statm is the resident page count.
long ResidentKib() {
    std::ifstream statm("/proc/self/statm");
    long total_pages = 0;
    long resident_pages = 0;
    if (!(statm >> total_pages >> resident_pages)) {
        return 0;
    }
    return resident_pages * 4;
}

std::vector<uint8_t> ReadFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    long repeat = 1;
    int first = 1;
    if (argc >= 3 && std::strcmp(argv[1], "-n") == 0) {
        repeat = std::strtol(argv[2], nullptr, 10);
        first = 3;
    }
    if (first >= argc) {
        std::fprintf(stderr, "usage: %s [-n count] <file>...\n", argv[0]);
        return 2;
    }

    std::vector<std::vector<uint8_t>> inputs;
    for (int index = first; index < argc; ++index) {
        inputs.push_back(ReadFile(argv[index]));
    }

    const long report_every = repeat < 10 ? 1 : repeat / 10;
    std::printf("inputs=%zu repeat=%ld rss_start=%ldKiB\n", inputs.size(), repeat, ResidentKib());
    for (long iteration = 0; iteration < repeat; ++iteration) {
        for (const std::vector<uint8_t>& input : inputs) {
            LLVMFuzzerTestOneInput(input.data(), input.size());
        }
        if ((iteration + 1) % report_every == 0) {
            std::printf("  iteration=%-10ld rss=%ldKiB\n", iteration + 1, ResidentKib());
            std::fflush(stdout);
        }
    }
    std::printf("rss_end=%ldKiB\n", ResidentKib());
    return 0;
}
