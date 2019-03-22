#include <gtest/gtest.h>

#include <freelist.h>

#include <vector>
#include <future>

// Constants
constexpr size_t c_freeListSize = 10000000;

// Types
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

struct AlignmentNode
{
    AlignmentNode(unsigned val1, bool val2)
        : m_val1(val1)
        , m_val2(val2)
        , m_blank('A')
    {
    }

    AlignmentNode()
        : m_val1(0)
        , m_val2(false)
        , m_blank('A')
    {
    }

    unsigned    m_val1;
    bool        m_val2;
    char        m_blank;
    // Compiler will pad here to ensure correct alignment, which should look like this:
    // char     m_pad[2]
};

struct ExceptionNode
{
    ExceptionNode(unsigned val1, bool throwException)
        : m_val1(val1)
        , m_throwException(throwException)
    {
        if (throwException) {
            throw std::runtime_error("Test Exception");
        }
    }

    ExceptionNode()
        : m_val1(0)
        , m_throwException(false)
    {
    }

    unsigned    m_val1;
    bool        m_throwException;
};

// Tests
template< typename T >
void testAlignment(std::unique_ptr< T >& freeList)
{
    auto alignmentRequirements = alignof(AlignmentNode);
    auto sizeOfRequirements = sizeof(fl::FreeListAlloc< AlignmentNode >);
    auto paddingSize = sizeOfRequirements % alignmentRequirements;

    auto startVal = false;
    auto nodes = std::vector< typename T::ptr >(c_freeListSize);

    for (size_t i = 0 ; i < c_freeListSize ; ++i) {
        auto node = freeList->construct(i, startVal);

        startVal = !startVal;
        ASSERT_TRUE(node != nullptr);

        // Ensure node is correctly aligned
        ASSERT_EQ(reinterpret_cast< unsigned long >(node.get()) % alignmentRequirements, 0U);

        if (i) {
            ASSERT_EQ(reinterpret_cast< unsigned long >(node.get()), reinterpret_cast< unsigned long >(nodes[i - 1].get()) + sizeOfRequirements + paddingSize);
        }

        nodes[i] = std::move(node);
    }

    ASSERT_FALSE(freeList->construct(0, false));
    // The unique_ptr will destroy the whole Free List and all the memory

    // Cleanup
    for (size_t i = 0 ; i < c_freeListSize ; ++i) {
        nodes[i] = nullptr;
    }
}

TEST(FreeListTest, testStaticAlignment)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer< AlignmentNode, c_freeListSize > >();
    testAlignment(freeList);
}

TEST(FreeListTest, testDynamicAlignment)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer< AlignmentNode > >(c_freeListSize);
    testAlignment(freeList);
}

template< typename T >
void testExceptionSafety(std::unique_ptr< T >& freeList, unsigned size)
{
    auto startVal = false;
    auto numIterations = (size * 2) - 1;

    std::vector< typename T::ptr > nodes(numIterations);

    for (size_t i = 0 ; i < numIterations ; ++i) {
        try {
            auto node = freeList->construct(i, startVal);
            ASSERT_TRUE(node != nullptr);
            nodes[i] = std::move(node);
        }
        catch (std::runtime_error& ex) {
            ASSERT_EQ(i % 2, 1U); // Only on the odd event
        }

        startVal = !startVal;
    }

    ASSERT_FALSE(freeList->construct(0, false));

    for (size_t i = 0 ; i < numIterations ; ++i) {
        nodes[i] = nullptr;
    }
}

TEST(FreeListTest, testExceptionSafetyST)
{
    constexpr auto size = 100;
    auto freeList = std::make_unique< fl::FreeListStaticSingleProducerSingleConsumer< ExceptionNode, size > >();
    testExceptionSafety(freeList, size);
}

TEST(FreeListTest, testExceptionSafetyMT)
{
    constexpr auto size = 100;
    auto freeList = std::make_unique< fl::FreeListStaticMultipleProducerSingleConsumer< ExceptionNode, size > >();
    testExceptionSafety(freeList, size);
}

template< typename T >
void testMaxAllocations(std::unique_ptr< T >& freeList)
{
    std::vector< typename T::ptr > nodes(c_freeListSize);

    size_t offset = c_freeListSize + 500;

    // Verify max allocations
    for (size_t i = 0 ; i < c_freeListSize ; ++i) {
        auto node = freeList->construct(i, i + offset);
        ASSERT_TRUE(node != nullptr);
        nodes[i] = std::move(node);
    }
    ASSERT_FALSE(freeList->construct(0, 0));

    // Verify all nodes still good
    for (size_t i = 0 ; i < c_freeListSize ; ++i) {
        EXPECT_EQ(nodes[i]->m_val1, i);
        EXPECT_EQ(nodes[i]->m_val2, i + offset);
    }
}

TEST(FreeListTest, testMaxAllocationStaticSTST)
{
    auto freeList = std::make_unique< fl::FreeListStaticSingleProducerSingleConsumer< TestNode, c_freeListSize > >();
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationStaticSTMT)
{
    auto freeList = std::make_unique< fl::FreeListStaticSingleProducerMultipleConsumer< TestNode, c_freeListSize > >();
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationStaticMTST)
{
    auto freeList = std::make_unique< fl::FreeListStaticMultipleProducerSingleConsumer< TestNode, c_freeListSize > >();
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationStaticMTMT)
{
    auto freeList = std::make_unique< fl::FreeListStaticMultipleProducerMultipleConsumer< TestNode, c_freeListSize > >();
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationDynamicSTST)
{
    auto freeList = std::make_unique< fl::FreeListDynamicSingleProducerSingleConsumer< TestNode > >(c_freeListSize);
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationDynamicSTMT)
{
    auto freeList = std::make_unique< fl::FreeListDynamicSingleProducerMultipleConsumer< TestNode > >(c_freeListSize);
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationDynamicMTST)
{
    auto freeList = std::make_unique< fl::FreeListDynamicMultipleProducerSingleConsumer< TestNode > >(c_freeListSize);
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationDynamicMTMT)
{
    auto freeList = std::make_unique< fl::FreeListDynamicMultipleProducerMultipleConsumer< TestNode > >(c_freeListSize);
    testMaxAllocations(freeList);
}

template< typename T >
void testReallocations(std::unique_ptr< T >& freeList) {
    std::vector<typename T::ptr> nodes(c_freeListSize);

    size_t offset = c_freeListSize + 500;
    size_t numAllocs = 5;
    size_t numRuns = 5;

    // Verify num allocs
    for (size_t i = 0; i < numAllocs; ++i) {
        auto node = freeList->construct(i, i + offset);
        ASSERT_TRUE(node != nullptr);
        nodes[i] = std::move(node);
    }

    // Verify all nodes are still good
    for (size_t i = 0; i < numAllocs; ++i) {
        EXPECT_EQ(nodes[i]->m_val1, i);
        EXPECT_EQ(nodes[i]->m_val2, i + offset);
    }

    // Clean-up
    for (size_t i = 0; i < numAllocs; ++i) {
        nodes[i] = nullptr;
    }

    for (size_t run = 0 ; run < numRuns ; ++run) {
        // Verify max allocations
        for (size_t i = 0 ; i < c_freeListSize ; ++i) {
            auto node = freeList->construct(i, i + offset);
            ASSERT_TRUE(node != nullptr);
            nodes[i] = std::move(node);
        }
        ASSERT_FALSE(freeList->construct(0, 0));

        // Verify all nodes still good
        for (size_t i = 0 ; i < c_freeListSize ; ++i) {
            EXPECT_EQ(nodes[i]->m_val1, i);
            EXPECT_EQ(nodes[i]->m_val2, i + offset);
        }

        // Clean-up
        for (size_t i = 0; i < c_freeListSize; ++i) {
            nodes[i] = nullptr;
        }
    }
}

TEST(FreeListTest, testReallocationsStaticSTST)
{
    auto freeList = std::make_unique< fl::FreeListStaticSingleProducerSingleConsumer< TestNode, c_freeListSize > >();
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsStaticSTMT)
{
    auto freeList = std::make_unique< fl::FreeListStaticSingleProducerMultipleConsumer< TestNode, c_freeListSize > >();
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsStaticMTST)
{
    auto freeList = std::make_unique< fl::FreeListStaticMultipleProducerSingleConsumer< TestNode, c_freeListSize > >();
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsStaticMTMT)
{
    auto freeList = std::make_unique< fl::FreeListStaticMultipleProducerMultipleConsumer< TestNode, c_freeListSize > >();
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsDynamicSTST)
{
    auto freeList = std::make_unique< fl::FreeListDynamicSingleProducerSingleConsumer< TestNode > >(c_freeListSize);
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsDynamicSTMT)
{
    auto freeList = std::make_unique< fl::FreeListDynamicSingleProducerMultipleConsumer< TestNode > >(c_freeListSize);
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsDynamicMTST)
{
    auto freeList = std::make_unique< fl::FreeListDynamicMultipleProducerSingleConsumer< TestNode > >(c_freeListSize);
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsDynamicMTMT)
{
    auto freeList = std::make_unique< fl::FreeListDynamicMultipleProducerMultipleConsumer< TestNode > >(c_freeListSize);
    testReallocations(freeList);
}

template< typename T >
void allocatorTestThread(std::shared_ptr< T > freeList)
{
    std::vector< typename T::ptr > nodes(c_freeListSize);

    for (size_t i = 0 ; i < c_freeListSize ; ++i) {
        auto node = freeList->construct(i, i);

        if (node) {
            nodes[i] = std::move(node);
        }
        else {
            break;
        }
    }

    for (size_t i = 0 ; i < c_freeListSize ; ++i) {
        if (nodes[i] != nullptr) {
            nodes[i] = nullptr;
        }
        else {
            break;
        }
    }
}

template< typename T >
void testMultithreaded(std::shared_ptr< T > freeList)
{
    size_t numThreads = 4;
    std::future< void > fut[numThreads];

    for (size_t i = 0 ; i < numThreads ; ++i) {
        fut[i] = std::async(std::launch::async, allocatorTestThread< T >, freeList);
    }

    for (size_t i = 0 ; i < numThreads ; ++i) {
        fut[i].wait();
    }

    // If we get here the code didn't lock up
    ASSERT_TRUE(true);
}

TEST(FreeListTest, testMultithreadedStaticMTMT)
{
    auto freeList = std::make_shared< fl::FreeListStaticMultipleProducerMultipleConsumer< TestNode, c_freeListSize > >();
    testMultithreaded(freeList);
}

TEST(FreeListTest, testMultithreadedDynamicMTMT)
{
    auto freeList = std::make_shared< fl::FreeListDynamicMultipleProducerMultipleConsumer< TestNode > >(c_freeListSize);
    testMultithreaded(freeList);
}