#include <gtest/gtest.h>

#include <freelist.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <boost/pool/object_pool.hpp>

// Constants
constexpr std::size_t c_perfFreeListSize = 100000;

// Types
struct Timer
{
    explicit Timer(std::string label)
        : m_label(std::move(label))
        , m_start(std::chrono::steady_clock::now())
    {
    }

    ~Timer()
    {
        auto end = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration<double>(end - m_start);
        std::cout << m_label << ": " << diff.count() << "s\n";
    }

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    std::string m_label;
    std::chrono::steady_clock::time_point m_start;
};

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
        // Fisher-Yates in-place shuffle
        std::random_device rd;
        std::mt19937 gen(rd());

        for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
            m_index[i] = i;
        }

        for (std::size_t i = c_perfFreeListSize - 1; i > 0; --i) {
            std::uniform_int_distribution<std::size_t> dist(0, i);
            std::size_t j = dist(gen);
            std::swap(m_index[i], m_index[j]);
        }
    }

    std::array<std::size_t, c_perfFreeListSize> m_index{};
};

// Globals
RandomIndex g_randomIndex;

// This method is used to randomise the internal linked list to stop sequential cache access
// artificially inflating performance compared to real world usage
template<typename T>
void randomiseFreeList(std::unique_ptr<T>& freeList)
{
    auto nodes = std::make_unique<std::array<typename T::ptr, c_perfFreeListSize>>();
    for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
        (*nodes)[i] = freeList->construct(i, i);
    }

    for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
        (*nodes)[g_randomIndex.m_index[i]] = nullptr;
    }
}

template<typename T>
void randomiseBoostObjectPool(std::unique_ptr<T>& boostPool)
{
    auto boostNodes = std::make_unique<std::array<TestNode*, c_perfFreeListSize>>();
    for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
        (*boostNodes)[i] = boostPool->construct(i, i);
    }

    for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
        boostPool->destroy((*boostNodes)[g_randomIndex.m_index[i]]);
    }
}

void randomiseNewDelete()
{
    // Allocate N objects, then free them in random order to fragment the
    // system allocator's free list — mirroring what randomiseFreeList does
    // for the free list's internal linked list.
    auto nodes = std::make_unique<std::array<std::unique_ptr<TestNode>, c_perfFreeListSize>>();
    for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
        (*nodes)[i] = std::make_unique<TestNode>(i, i);
    }

    for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
        (*nodes)[g_randomIndex.m_index[i]] = nullptr;
    }
}

template<typename T>
void testAgainstNewAndDelete(std::unique_ptr<T>& freeList)
{
    randomiseFreeList(freeList);

    auto nodes = std::make_unique<std::array<typename T::ptr, c_perfFreeListSize>>();
    auto newedNodes = std::make_unique<std::array<std::unique_ptr<TestNode>, c_perfFreeListSize>>();

    std::cout << "FreeList\n";

    {
        std::size_t offset = c_perfFreeListSize + 500;
        Timer t("  Allocate");
        for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
            (*nodes)[i] = freeList->construct(i, i + offset);
        }
    }

    {
        Timer t("  Free");
        for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
            (*nodes)[i] = nullptr;
        }
    }

    randomiseNewDelete();

    std::cout << "New / Delete (fragmented heap)\n";

    {
        std::size_t offset = c_perfFreeListSize + 500;
        Timer t("  Allocate");
        for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
            (*newedNodes)[i] = std::make_unique<TestNode>(i, i + offset);
        }
    }

    {
        Timer t("  Free");
        for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
            (*newedNodes)[i] = nullptr;
        }
    }
}

TEST(PerformanceTest, testAgainstNewAndDeleteStaticSTST)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer<TestNode, c_perfFreeListSize>>();
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteStaticSTMT)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerMultipleConsumer<TestNode, c_perfFreeListSize>>();
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteStaticMTST)
{
    auto freeList = std::make_unique<fl::FreeListStaticMultipleProducerSingleConsumer<TestNode, c_perfFreeListSize>>();
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteStaticMTMT)
{
    auto freeList = std::make_unique<fl::FreeListStaticMultipleProducerMultipleConsumer<TestNode, c_perfFreeListSize>>();
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteDynamicSTST)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer<TestNode>>(c_perfFreeListSize);
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteDynamicSTMT)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerMultipleConsumer<TestNode>>(c_perfFreeListSize);
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteDynamicMTST)
{
    auto freeList = std::make_unique<fl::FreeListDynamicMultipleProducerSingleConsumer<TestNode>>(c_perfFreeListSize);
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteDynamicMTMT)
{
    auto freeList = std::make_unique<fl::FreeListDynamicMultipleProducerMultipleConsumer<TestNode>>(c_perfFreeListSize);
    testAgainstNewAndDelete(freeList);
}

template<typename T>
void testAgainstBoostObjectPool(std::unique_ptr<T>& freeList)
{
    randomiseFreeList(freeList);

    auto boostPool = std::make_unique<boost::object_pool<TestNode>>();
    randomiseBoostObjectPool(boostPool);

    auto nodes = std::make_unique<std::array<typename T::ptr, c_perfFreeListSize>>();
    auto boostNodes = std::make_unique<std::array<TestNode*, c_perfFreeListSize>>();

    std::cout << "FreeList\n";

    {
        std::size_t offset = c_perfFreeListSize + 500;
        Timer t("  Allocate");
        for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
            (*nodes)[i] = freeList->construct(i, i + offset);
        }
    }

    {
        Timer t("  Free");
        for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
            (*nodes)[i] = nullptr;
        }
    }

    std::cout << "Boost Object Pool\n";

    {
        std::size_t offset = c_perfFreeListSize + 500;
        Timer t("  Allocate");
        for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
            (*boostNodes)[i] = boostPool->construct(i, i + offset);
        }
    }

    {
        Timer t("  Free (only 10k nodes due to slow performance) - O(N)");
        for (std::size_t i = 0; i < 10000; ++i) {
            boostPool->destroy((*boostNodes)[i]);
        }
    }
}

TEST(PerformanceTest, testAgainstBoostObjectPoolStaticSTST)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer<TestNode, c_perfFreeListSize>>();
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolStaticSTMT)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerMultipleConsumer<TestNode, c_perfFreeListSize>>();
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolStaticMTST)
{
    auto freeList = std::make_unique<fl::FreeListStaticMultipleProducerSingleConsumer<TestNode, c_perfFreeListSize>>();
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolStaticMTMT)
{
    auto freeList = std::make_unique<fl::FreeListStaticMultipleProducerMultipleConsumer<TestNode, c_perfFreeListSize>>();
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolDynamicSTST)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer<TestNode>>(c_perfFreeListSize);
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolDynamicSTMT)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerMultipleConsumer<TestNode>>(c_perfFreeListSize);
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolDynamicMTST)
{
    auto freeList = std::make_unique<fl::FreeListDynamicMultipleProducerSingleConsumer<TestNode>>(c_perfFreeListSize);
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolDynamicMTMT)
{
    auto freeList = std::make_unique<fl::FreeListDynamicMultipleProducerMultipleConsumer<TestNode>>(c_perfFreeListSize);
    testAgainstBoostObjectPool(freeList);
}

// --- Multithreaded contention benchmarks ---

template<typename T>
void testMultithreadedContention(std::shared_ptr<T> freeList, std::size_t numThreads)
{
    std::cout << numThreads << " threads contending\n";

    std::vector<std::future<void>> futures;
    futures.reserve(numThreads);

    {
        Timer t("  Total");
        for (std::size_t t = 0; t < numThreads; ++t) {
            futures.push_back(std::async(std::launch::async, [&freeList]() {
                std::vector<typename T::ptr> nodes;
                nodes.reserve(c_perfFreeListSize);

                // Allocate as many as we can
                for (std::size_t i = 0; i < c_perfFreeListSize; ++i) {
                    auto node = freeList->construct(static_cast<unsigned>(i), static_cast<unsigned>(i));
                    if (!node) break;
                    nodes.push_back(std::move(node));
                }

                // Free all
                nodes.clear();
            }));
        }

        for (auto& f : futures) {
            f.wait();
        }
    }
}

TEST(PerformanceTest, testContention2ThreadsStaticMTMT)
{
    auto freeList = std::make_shared<fl::FreeListStaticMultipleProducerMultipleConsumer<TestNode, c_perfFreeListSize>>();
    testMultithreadedContention(freeList, 2);
}

TEST(PerformanceTest, testContention4ThreadsStaticMTMT)
{
    auto freeList = std::make_shared<fl::FreeListStaticMultipleProducerMultipleConsumer<TestNode, c_perfFreeListSize>>();
    testMultithreadedContention(freeList, 4);
}

TEST(PerformanceTest, testContention8ThreadsStaticMTMT)
{
    auto freeList = std::make_shared<fl::FreeListStaticMultipleProducerMultipleConsumer<TestNode, c_perfFreeListSize>>();
    testMultithreadedContention(freeList, 8);
}

TEST(PerformanceTest, testContention2ThreadsDynamicMTMT)
{
    auto freeList = std::make_shared<fl::FreeListDynamicMultipleProducerMultipleConsumer<TestNode>>(c_perfFreeListSize);
    testMultithreadedContention(freeList, 2);
}

TEST(PerformanceTest, testContention4ThreadsDynamicMTMT)
{
    auto freeList = std::make_shared<fl::FreeListDynamicMultipleProducerMultipleConsumer<TestNode>>(c_perfFreeListSize);
    testMultithreadedContention(freeList, 4);
}

TEST(PerformanceTest, testContention8ThreadsDynamicMTMT)
{
    auto freeList = std::make_shared<fl::FreeListDynamicMultipleProducerMultipleConsumer<TestNode>>(c_perfFreeListSize);
    testMultithreadedContention(freeList, 8);
}
