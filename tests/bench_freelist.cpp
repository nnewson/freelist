#include <benchmark/benchmark.h>

#include <freelist.h>

#include <array>
#include <cstddef>
#include <memory>
#include <random>

#include <boost/pool/object_pool.hpp>

// ---------------------------------------------------------------------------
// Constants & helpers
// ---------------------------------------------------------------------------

constexpr std::size_t c_poolSize = 100000;

struct TestNode
{
    TestNode(unsigned val1, unsigned val2)
        : m_val1(val1)
        , m_val2(val2)
    {
    }

    TestNode()
        : m_val1(0)
        , m_val2(0)
    {
    }

    unsigned m_val1;
    unsigned m_val2;
};

struct RandomIndex
{
    RandomIndex()
    {
        for (std::size_t i = 0; i < c_poolSize; ++i) {
            m_index[i] = i;
        }

        std::random_device rd;
        std::mt19937 gen(rd());

        for (std::size_t i = c_poolSize - 1; i > 0; --i) {
            std::uniform_int_distribution<std::size_t> dist(0, i);
            std::swap(m_index[i], m_index[dist(gen)]);
        }
    }

    std::array<std::size_t, c_poolSize> m_index{};
};

static RandomIndex g_randomIndex;

// Randomise the internal linked list to prevent sequential cache access
template<typename T>
void randomiseFreeList(std::unique_ptr<T>& freeList)
{
    auto nodes = std::make_unique<std::array<typename T::ptr, c_poolSize>>();
    for (std::size_t i = 0; i < c_poolSize; ++i) {
        (*nodes)[i] = freeList->construct(static_cast<unsigned>(i), static_cast<unsigned>(i));
    }

    for (std::size_t i = 0; i < c_poolSize; ++i) {
        (*nodes)[g_randomIndex.m_index[i]] = nullptr;
    }
}

template<typename T>
void randomiseBoostObjectPool(std::unique_ptr<T>& boostPool)
{
    auto nodes = std::make_unique<std::array<TestNode*, c_poolSize>>();
    for (std::size_t i = 0; i < c_poolSize; ++i) {
        (*nodes)[i] = boostPool->construct(static_cast<unsigned>(i), static_cast<unsigned>(i));
    }

    for (std::size_t i = 0; i < c_poolSize; ++i) {
        boostPool->destroy((*nodes)[g_randomIndex.m_index[i]]);
    }
}

void randomiseNewDelete()
{
    auto nodes = std::make_unique<std::array<std::unique_ptr<TestNode>, c_poolSize>>();
    for (std::size_t i = 0; i < c_poolSize; ++i) {
        (*nodes)[i] = std::make_unique<TestNode>(static_cast<unsigned>(i), static_cast<unsigned>(i));
    }

    for (std::size_t i = 0; i < c_poolSize; ++i) {
        (*nodes)[g_randomIndex.m_index[i]] = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Single allocate + free  (steady-state per-operation latency)
// ---------------------------------------------------------------------------

// Each iteration: pop one node, construct, then the unique_ptr dtor pushes it
// back.  The free list never exhausts, giving us true steady-state numbers.

// Type aliases to avoid preprocessor comma issues with template arguments
using StaticSTST = fl::FreeListStaticSingleProducerSingleConsumer<TestNode, c_poolSize>;
using StaticSTMT = fl::FreeListStaticSingleProducerMultipleConsumer<TestNode, c_poolSize>;
using StaticMTST = fl::FreeListStaticMultipleProducerSingleConsumer<TestNode, c_poolSize>;
using StaticMTMT = fl::FreeListStaticMultipleProducerMultipleConsumer<TestNode, c_poolSize>;
using DynamicSTST = fl::FreeListDynamicSingleProducerSingleConsumer<TestNode>;
using DynamicSTMT = fl::FreeListDynamicSingleProducerMultipleConsumer<TestNode>;
using DynamicMTST = fl::FreeListDynamicMultipleProducerSingleConsumer<TestNode>;
using DynamicMTMT = fl::FreeListDynamicMultipleProducerMultipleConsumer<TestNode>;

#define FL_BENCH_ALLOC_FREE(Name, Type, ...)                                    \
    static void Name(benchmark::State& state)                                   \
    {                                                                           \
        auto fl = std::make_unique<Type>(__VA_ARGS__);                          \
        randomiseFreeList(fl);                                                  \
        for (auto _ : state) {                                                  \
            auto node = fl->construct(1u, 2u);                                  \
            benchmark::DoNotOptimize(node.get());                               \
        }                                                                       \
        state.SetItemsProcessed(state.iterations());                            \
    }                                                                           \
    BENCHMARK(Name)->Unit(benchmark::kNanosecond)

// Static variants
FL_BENCH_ALLOC_FREE(BM_AllocFree_StaticSTST, StaticSTST);
FL_BENCH_ALLOC_FREE(BM_AllocFree_StaticSTMT, StaticSTMT);
FL_BENCH_ALLOC_FREE(BM_AllocFree_StaticMTST, StaticMTST);
FL_BENCH_ALLOC_FREE(BM_AllocFree_StaticMTMT, StaticMTMT);

// Dynamic variants
FL_BENCH_ALLOC_FREE(BM_AllocFree_DynamicSTST, DynamicSTST, c_poolSize);
FL_BENCH_ALLOC_FREE(BM_AllocFree_DynamicSTMT, DynamicSTMT, c_poolSize);
FL_BENCH_ALLOC_FREE(BM_AllocFree_DynamicMTST, DynamicMTST, c_poolSize);
FL_BENCH_ALLOC_FREE(BM_AllocFree_DynamicMTMT, DynamicMTMT, c_poolSize);

#undef FL_BENCH_ALLOC_FREE

// ---------------------------------------------------------------------------
// Baselines: new/delete and boost::object_pool
// ---------------------------------------------------------------------------

static void BM_AllocFree_NewDelete(benchmark::State& state)
{
    randomiseNewDelete();
    for (auto _ : state) {
        auto node = std::make_unique<TestNode>(1u, 2u);
        benchmark::DoNotOptimize(node.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AllocFree_NewDelete)->Unit(benchmark::kNanosecond);

static void BM_AllocFree_BoostPool(benchmark::State& state)
{
    auto pool = std::make_unique<boost::object_pool<TestNode>>();
    randomiseBoostObjectPool(pool);
    for (auto _ : state) {
        auto* node = pool->construct(1u, 2u);
        benchmark::DoNotOptimize(node);
        pool->destroy(node);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AllocFree_BoostPool)->Unit(benchmark::kNanosecond);

// ---------------------------------------------------------------------------
// Batch allocate N, then free N  (bulk throughput)
// ---------------------------------------------------------------------------

#define FL_BENCH_BATCH(Name, Type, ...)                                         \
    static void Name(benchmark::State& state)                                   \
    {                                                                           \
        auto fl = std::make_unique<Type>(__VA_ARGS__);                          \
        randomiseFreeList(fl);                                                  \
        auto nodes =                                                            \
            std::make_unique<std::array<typename Type::ptr, c_poolSize>>();      \
        for (auto _ : state) {                                                  \
            for (std::size_t i = 0; i < c_poolSize; ++i) {                      \
                (*nodes)[i] = fl->construct(                                    \
                    static_cast<unsigned>(i), static_cast<unsigned>(i));         \
            }                                                                   \
            for (std::size_t i = 0; i < c_poolSize; ++i) {                      \
                (*nodes)[i] = nullptr;                                          \
            }                                                                   \
        }                                                                       \
        state.SetItemsProcessed(                                                \
            state.iterations() * static_cast<int64_t>(c_poolSize));             \
    }                                                                           \
    BENCHMARK(Name)->Unit(benchmark::kMillisecond)

FL_BENCH_BATCH(BM_Batch_StaticSTST, StaticSTST);
FL_BENCH_BATCH(BM_Batch_StaticMTMT, StaticMTMT);
FL_BENCH_BATCH(BM_Batch_DynamicSTST, DynamicSTST, c_poolSize);
FL_BENCH_BATCH(BM_Batch_DynamicMTMT, DynamicMTMT, c_poolSize);

#undef FL_BENCH_BATCH

static void BM_Batch_NewDelete(benchmark::State& state)
{
    auto nodes =
        std::make_unique<std::array<std::unique_ptr<TestNode>, c_poolSize>>();
    for (auto _ : state) {
        for (std::size_t i = 0; i < c_poolSize; ++i) {
            (*nodes)[i] = std::make_unique<TestNode>(
                static_cast<unsigned>(i), static_cast<unsigned>(i));
        }
        for (std::size_t i = 0; i < c_poolSize; ++i) {
            (*nodes)[i] = nullptr;
        }
    }
    state.SetItemsProcessed(
        state.iterations() * static_cast<int64_t>(c_poolSize));
}
BENCHMARK(BM_Batch_NewDelete)->Unit(benchmark::kMillisecond);

static void BM_Batch_BoostPool(benchmark::State& state)
{
    auto pool = std::make_unique<boost::object_pool<TestNode>>();
    auto nodes = std::make_unique<std::array<TestNode*, c_poolSize>>();
    for (auto _ : state) {
        for (std::size_t i = 0; i < c_poolSize; ++i) {
            (*nodes)[i] = pool->construct(
                static_cast<unsigned>(i), static_cast<unsigned>(i));
        }
        for (std::size_t i = 0; i < c_poolSize; ++i) {
            pool->destroy((*nodes)[i]);
        }
    }
    state.SetItemsProcessed(
        state.iterations() * static_cast<int64_t>(c_poolSize));
}
BENCHMARK(BM_Batch_BoostPool)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
// Multi-threaded contention  (MTMT variants with increasing thread counts)
// ---------------------------------------------------------------------------
// Each thread does steady-state allocate+free on a shared free list.
// UseRealTime() so wall-clock is reported, not CPU-time × threads.

static void BM_Contention_StaticMTMT(benchmark::State& state)
{
    static auto fl = std::make_shared<StaticMTMT>();

    for (auto _ : state) {
        auto node = fl->construct(1u, 2u);
        benchmark::DoNotOptimize(node.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Contention_StaticMTMT)
    ->Unit(benchmark::kNanosecond)
    ->UseRealTime()
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);

static void BM_Contention_DynamicMTMT(benchmark::State& state)
{
    static auto fl = std::make_shared<DynamicMTMT>(c_poolSize);

    for (auto _ : state) {
        auto node = fl->construct(1u, 2u);
        benchmark::DoNotOptimize(node.get());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Contention_DynamicMTMT)
    ->Unit(benchmark::kNanosecond)
    ->UseRealTime()
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);
