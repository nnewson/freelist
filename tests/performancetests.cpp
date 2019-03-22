#include <gtest/gtest.h>

#include <freelist.h>

#include <chrono>
#include <iostream>
#include <random>

#include <boost/pool/object_pool.hpp>

// Constants
constexpr size_t c_perfFreeListSize = 100000;

// Types
struct Timer
{
    Timer()
        : m_start(std::chrono::steady_clock::now())
    {}

    ~Timer()
    {
        std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        std::chrono::duration< double > diff = std::chrono::duration_cast< std::chrono::duration< double > >(end - m_start);
        std::cout << "Timer: " << diff.count() << '\n';
    }

    std::chrono::steady_clock::time_point   m_start;
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

    unsigned    m_val1;
    unsigned    m_val2;
};

struct RandomIndex
{
    RandomIndex()
    {
        // Fisher-Yates in-place shuffle
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution< size_t > dist(0, c_perfFreeListSize - 1);

        for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
            size_t j = dist(gen) % (i + 1);

            if (j != i) {
                m_index[i] = m_index[j];
            }
            m_index[j] = i;
        }
    }

    std::array< size_t, c_perfFreeListSize >  m_index;
};

// Globals
RandomIndex g_randomIndex;

// This methods is used to randomise the internal linked list to stop sequential cache access
// artificially inflating performance compared to real world usage
template< typename T >
void randomiseFreeList(std::unique_ptr< T >& freeList)
{
    auto nodes = std::make_unique< std::array< typename T::ptr, c_perfFreeListSize > >();
    for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
        (*nodes)[i] = freeList->construct(i, i);
    }

    // We now have a list of unique integers from 0 to size-1 which are randomised
    for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
        // Free the unique random entry to ensure the internal list is randomised
        (*nodes)[g_randomIndex.m_index[i]] = nullptr;
    }
}

template< typename T >
void randomiseBoostObjectPool(std::unique_ptr< T >& boostPool)
{
    auto boostNodes = std::make_unique< std::array< TestNode*, c_perfFreeListSize > >();
    for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
        (*boostNodes)[i] = boostPool->construct(i, i);
    }

    // We now have a list of unique integers from 0 to size-1 which are randomised
    for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
        // Free the unique random entry to ensure the internal list is randomised
        boostPool->destroy((*boostNodes)[g_randomIndex.m_index[i]]);;
    }
}

template< typename T >
void testAgainstNewAndDelete(std::unique_ptr< T >& freeList)
{
    randomiseFreeList(freeList);

    auto nodes = std::make_unique< std::array< typename T::ptr, c_perfFreeListSize > >();
    auto newedNodes = std::make_unique< std::array< std::unique_ptr< TestNode >, c_perfFreeListSize > >();

    std::cout << "FreeList" << "\n";

    {
        std::cout << "Allocate" << "\n";
        size_t offset = c_perfFreeListSize + 500;

        Timer t;
        for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
            (*nodes)[i] = freeList->construct(i, i + offset);
        }
    }

    {
        std::cout << "Free" << "\n";

        Timer t;
        for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
            (*nodes)[i] = nullptr;
        }
    }

    std::cout << "\n" << "New / Delete" << "\n";

    {
        std::cout << "Allocate" << "\n";
        size_t offset = c_perfFreeListSize + 500;

        Timer t;
        for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
            (*newedNodes)[i] = std::make_unique< TestNode >(i, i + offset);
        }
    }

    {
        std::cout << "Free" << "\n";

        Timer t;
        for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
            (*newedNodes)[i] = nullptr;
        }
    }
}

TEST(PerformanceTest, testAgainstNewAndDeleteStaticSTST)
{
    auto freeList = std::make_unique< fl::FreeListStaticSingleProducerSingleConsumer< TestNode, c_perfFreeListSize > >();
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteStaticSTMT)
{
    auto freeList = std::make_unique< fl::FreeListStaticSingleProducerMultipleConsumer< TestNode, c_perfFreeListSize > >();
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteStaticMTST)
{
    auto freeList = std::make_unique< fl::FreeListStaticMultipleProducerSingleConsumer< TestNode, c_perfFreeListSize > >();
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteStaticMTMT)
{
    auto freeList = std::make_unique< fl::FreeListStaticMultipleProducerMultipleConsumer< TestNode, c_perfFreeListSize > >();
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteDynamicSTST)
{
    auto freeList = std::make_unique< fl::FreeListDynamicSingleProducerSingleConsumer< TestNode > >(c_perfFreeListSize);
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteDynamicSTMT)
{
    auto freeList = std::make_unique< fl::FreeListDynamicSingleProducerMultipleConsumer< TestNode > >(c_perfFreeListSize);
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteDynamicMTST)
{
    auto freeList = std::make_unique< fl::FreeListDynamicMultipleProducerSingleConsumer< TestNode > >(c_perfFreeListSize);
    testAgainstNewAndDelete(freeList);
}

TEST(PerformanceTest, testAgainstNewAndDeleteDynamicMTMT)
{
    auto freeList = std::make_unique< fl::FreeListDynamicMultipleProducerMultipleConsumer< TestNode > >(c_perfFreeListSize);
    testAgainstNewAndDelete(freeList);
}

template< typename T >
void testAgainstBoostObjectPool(std::unique_ptr< T >& freeList)
{
    randomiseFreeList(freeList);

    auto boostPool = std::make_unique< boost::object_pool< TestNode > >();
    randomiseBoostObjectPool(boostPool);

    auto nodes = std::make_unique< std::array< typename T::ptr, c_perfFreeListSize > >();
    auto boostNodes = std::make_unique< std::array< TestNode*, c_perfFreeListSize > >();

    std::cout << "FreeList" << "\n";

    {
        std::cout << "Allocate" << "\n";
        size_t offset = c_perfFreeListSize + 500;

        Timer t;
        for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
            (*nodes)[i] = freeList->construct(i, i + offset);
        }
    }

    {
        std::cout << "Free" << "\n";

        Timer t;
        for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
            (*nodes)[i] = nullptr;
        }
    }

    std::cout << "\n" << "Boost Object Pool" << "\n";

    {
        std::cout << "Allocate" << "\n";
        size_t offset = c_perfFreeListSize + 500;

        Timer t;
        for (size_t i = 0 ; i < c_perfFreeListSize ; ++i) {
            (*boostNodes)[i] = boostPool->construct(i, i + offset);
        }
    }

    {
        std::cout << "Free (only 10k nodes due to slow performance) - this is O(N)" << "\n";

        Timer t;
        for (size_t i = 0 ; i < 10000 ; ++i) {
            boostPool->destroy((*boostNodes)[i]);
        }
    }
}

TEST(PerformanceTest, testAgainstBoostObjectPoolStaticSTST)
{
    auto freeList = std::make_unique< fl::FreeListStaticSingleProducerSingleConsumer< TestNode, c_perfFreeListSize > >();
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolStaticSTMT)
{
    auto freeList = std::make_unique< fl::FreeListStaticSingleProducerMultipleConsumer< TestNode, c_perfFreeListSize > >();
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolStaticMTST)
{
    auto freeList = std::make_unique< fl::FreeListStaticMultipleProducerSingleConsumer< TestNode, c_perfFreeListSize > >();
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolStaticMTMT)
{
    auto freeList = std::make_unique< fl::FreeListStaticMultipleProducerMultipleConsumer< TestNode, c_perfFreeListSize > >();
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolDynamicSTST)
{
    auto freeList = std::make_unique< fl::FreeListDynamicSingleProducerSingleConsumer< TestNode > >(c_perfFreeListSize);
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolDynamicSTMT)
{
    auto freeList = std::make_unique< fl::FreeListDynamicSingleProducerMultipleConsumer< TestNode > >(c_perfFreeListSize);
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolDynamicMTST)
{
    auto freeList = std::make_unique< fl::FreeListDynamicMultipleProducerSingleConsumer< TestNode > >(c_perfFreeListSize);
    testAgainstBoostObjectPool(freeList);
}

TEST(PerformanceTest, testAgainstBoostObjectPoolDynamicMTMT)
{
    auto freeList = std::make_unique< fl::FreeListDynamicMultipleProducerMultipleConsumer< TestNode > >(c_perfFreeListSize);
    testAgainstBoostObjectPool(freeList);
}