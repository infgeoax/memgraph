#include <random>
#include <thread>

#include "benchmark/benchmark_api.h"

#include "data_structures/bloom/bloom_filter.hpp"
#include "logging/default.hpp"
#include "logging/streams/stdout.hpp"
#include "utils/command_line/arguments.hpp"
#include "utils/hashing/fnv64.hpp"
#include "utils/random/generator.h"

using utils::random::StringGenerator;
using StringHashFunction = std::function<uint64_t(const std::string &)>;

template <class Type, int Size>
static void TestBloom(benchmark::State &state, BloomFilter<Type, Size> *bloom,
                      const std::vector<Type> &elements)
{
    while (state.KeepRunning())
    {
        for (int start = 0; start < state.range(0); start++)
            if (start % 2)
                bloom->contains(elements[start]);
            else
                bloom->insert(elements[start]);
    }
    state.SetComplexityN(state.range(0));
}

auto BM_Bloom = [](benchmark::State &state, auto *bloom, const auto &elements) {
    TestBloom(state, bloom, elements);
};

int main(int argc, char **argv)
{
    logging::init_async();
    logging::log->pipe(std::make_unique<Stdout>());

    StringGenerator generator(4);

    auto elements = utils::random::generate_vector(generator, 1 << 16);

    StringHashFunction hash1              = fnv64<std::string>;
    StringHashFunction hash2              = fnv1a64<std::string>;
    std::vector<StringHashFunction> funcs = {hash1, hash2};

    BloomFilter<std::string, 128> bloom(funcs);

    benchmark::RegisterBenchmark("SimpleBloomFilter Benchmark Test", BM_Bloom,
                                 &bloom, elements)
        ->RangeMultiplier(2)
        ->Range(1, 1 << 16)
        ->Complexity(benchmark::oN);

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
}
