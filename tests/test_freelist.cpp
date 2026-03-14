#include <gtest/gtest.h>

#include <freelist.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <vector>

// Constants
constexpr std::size_t c_freeListSize = 10000000;

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

    unsigned m_val1;
    unsigned m_val2;
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

    unsigned m_val1;
    bool     m_val2;
    char     m_blank;
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

    unsigned m_val1;
    bool     m_throwException;
};

// Move-only type to verify forwarding references work correctly
struct MoveOnlyNode
{
    MoveOnlyNode(std::string name, unsigned val)
        : m_name(std::move(name))
        , m_val(val)
    {
    }

    MoveOnlyNode(const MoveOnlyNode&) = delete;
    MoveOnlyNode& operator=(const MoveOnlyNode&) = delete;
    MoveOnlyNode(MoveOnlyNode&&) = default;
    MoveOnlyNode& operator=(MoveOnlyNode&&) = default;

    std::string m_name;
    unsigned    m_val;
};

// High-alignment type to verify deleter offsetof fix
struct alignas(32) HighAlignNode
{
    HighAlignNode(unsigned val1, unsigned val2)
        : m_val1(val1)
        , m_val2(val2)
    {
    }

    unsigned m_val1;
    unsigned m_val2;
};

// Type that tracks construction/destruction for lifetime verification
struct LifetimeNode
{
    static std::atomic<int> s_liveCount;

    LifetimeNode(unsigned val1, unsigned val2)
        : m_val1(val1)
        , m_val2(val2)
    {
        s_liveCount.fetch_add(1, std::memory_order_relaxed);
    }

    ~LifetimeNode()
    {
        s_liveCount.fetch_sub(1, std::memory_order_relaxed);
    }

    LifetimeNode(const LifetimeNode&) = delete;
    LifetimeNode& operator=(const LifetimeNode&) = delete;
    LifetimeNode(LifetimeNode&&) = delete;
    LifetimeNode& operator=(LifetimeNode&&) = delete;

    unsigned m_val1;
    unsigned m_val2;
};

std::atomic<int> LifetimeNode::s_liveCount{0};

// Tests
template<typename T>
void testAlignment(std::unique_ptr<T>& freeList)
{
    auto alignmentRequirements = alignof(AlignmentNode);
    auto sizeOfRequirements = sizeof(fl::FreeListAlloc<AlignmentNode>);
    auto paddingSize = sizeOfRequirements % alignmentRequirements;

    auto startVal = false;
    auto nodes = std::vector<typename T::ptr>(c_freeListSize);

    for (std::size_t i = 0; i < c_freeListSize; ++i) {
        auto node = freeList->construct(i, startVal);

        startVal = !startVal;
        ASSERT_TRUE(node != nullptr);

        // Ensure node is correctly aligned
        ASSERT_EQ(reinterpret_cast<std::uintptr_t>(node.get()) % alignmentRequirements, 0U);

        if (i) {
            ASSERT_EQ(reinterpret_cast<std::uintptr_t>(node.get()),
                      reinterpret_cast<std::uintptr_t>(nodes[i - 1].get()) + sizeOfRequirements + paddingSize);
        }

        nodes[i] = std::move(node);
    }

    ASSERT_FALSE(freeList->construct(0, false));

    // Cleanup
    for (std::size_t i = 0; i < c_freeListSize; ++i) {
        nodes[i] = nullptr;
    }
}

TEST(FreeListTest, testStaticAlignment)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer<AlignmentNode, c_freeListSize>>();
    testAlignment(freeList);
}

TEST(FreeListTest, testDynamicAlignment)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer<AlignmentNode>>(c_freeListSize);
    testAlignment(freeList);
}

template<typename T>
void testExceptionSafety(std::unique_ptr<T>& freeList, unsigned size)
{
    auto startVal = false;
    auto numIterations = (size * 2) - 1;

    std::vector<typename T::ptr> nodes(numIterations);

    for (std::size_t i = 0; i < numIterations; ++i) {
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

    for (std::size_t i = 0; i < numIterations; ++i) {
        nodes[i] = nullptr;
    }
}

TEST(FreeListTest, testExceptionSafetyST)
{
    constexpr auto size = 100;
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer<ExceptionNode, size>>();
    testExceptionSafety(freeList, size);
}

TEST(FreeListTest, testExceptionSafetyMT)
{
    constexpr auto size = 100;
    auto freeList = std::make_unique<fl::FreeListStaticMultipleProducerSingleConsumer<ExceptionNode, size>>();
    testExceptionSafety(freeList, size);
}

template<typename T>
void testMaxAllocations(std::unique_ptr<T>& freeList)
{
    std::vector<typename T::ptr> nodes(c_freeListSize);

    std::size_t offset = c_freeListSize + 500;

    // Verify max allocations
    for (std::size_t i = 0; i < c_freeListSize; ++i) {
        auto node = freeList->construct(i, i + offset);
        ASSERT_TRUE(node != nullptr);
        nodes[i] = std::move(node);
    }
    ASSERT_FALSE(freeList->construct(0, 0));

    // Verify all nodes still good
    for (std::size_t i = 0; i < c_freeListSize; ++i) {
        EXPECT_EQ(nodes[i]->m_val1, i);
        EXPECT_EQ(nodes[i]->m_val2, i + offset);
    }
}

TEST(FreeListTest, testMaxAllocationStaticSTST)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer<TestNode, c_freeListSize>>();
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationStaticSTMT)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerMultipleConsumer<TestNode, c_freeListSize>>();
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationStaticMTST)
{
    auto freeList = std::make_unique<fl::FreeListStaticMultipleProducerSingleConsumer<TestNode, c_freeListSize>>();
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationStaticMTMT)
{
    auto freeList = std::make_unique<fl::FreeListStaticMultipleProducerMultipleConsumer<TestNode, c_freeListSize>>();
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationDynamicSTST)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer<TestNode>>(c_freeListSize);
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationDynamicSTMT)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerMultipleConsumer<TestNode>>(c_freeListSize);
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationDynamicMTST)
{
    auto freeList = std::make_unique<fl::FreeListDynamicMultipleProducerSingleConsumer<TestNode>>(c_freeListSize);
    testMaxAllocations(freeList);
}

TEST(FreeListTest, testMaxAllocationDynamicMTMT)
{
    auto freeList = std::make_unique<fl::FreeListDynamicMultipleProducerMultipleConsumer<TestNode>>(c_freeListSize);
    testMaxAllocations(freeList);
}

template<typename T>
void testReallocations(std::unique_ptr<T>& freeList)
{
    std::vector<typename T::ptr> nodes(c_freeListSize);

    std::size_t offset = c_freeListSize + 500;
    std::size_t numAllocs = 5;
    std::size_t numRuns = 5;

    // Verify num allocs
    for (std::size_t i = 0; i < numAllocs; ++i) {
        auto node = freeList->construct(i, i + offset);
        ASSERT_TRUE(node != nullptr);
        nodes[i] = std::move(node);
    }

    // Verify all nodes are still good
    for (std::size_t i = 0; i < numAllocs; ++i) {
        EXPECT_EQ(nodes[i]->m_val1, i);
        EXPECT_EQ(nodes[i]->m_val2, i + offset);
    }

    // Clean-up
    for (std::size_t i = 0; i < numAllocs; ++i) {
        nodes[i] = nullptr;
    }

    for (std::size_t run = 0; run < numRuns; ++run) {
        // Verify max allocations
        for (std::size_t i = 0; i < c_freeListSize; ++i) {
            auto node = freeList->construct(i, i + offset);
            ASSERT_TRUE(node != nullptr);
            nodes[i] = std::move(node);
        }
        ASSERT_FALSE(freeList->construct(0, 0));

        // Verify all nodes still good
        for (std::size_t i = 0; i < c_freeListSize; ++i) {
            EXPECT_EQ(nodes[i]->m_val1, i);
            EXPECT_EQ(nodes[i]->m_val2, i + offset);
        }

        // Clean-up
        for (std::size_t i = 0; i < c_freeListSize; ++i) {
            nodes[i] = nullptr;
        }
    }
}

TEST(FreeListTest, testReallocationsStaticSTST)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer<TestNode, c_freeListSize>>();
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsStaticSTMT)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerMultipleConsumer<TestNode, c_freeListSize>>();
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsStaticMTST)
{
    auto freeList = std::make_unique<fl::FreeListStaticMultipleProducerSingleConsumer<TestNode, c_freeListSize>>();
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsStaticMTMT)
{
    auto freeList = std::make_unique<fl::FreeListStaticMultipleProducerMultipleConsumer<TestNode, c_freeListSize>>();
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsDynamicSTST)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer<TestNode>>(c_freeListSize);
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsDynamicSTMT)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerMultipleConsumer<TestNode>>(c_freeListSize);
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsDynamicMTST)
{
    auto freeList = std::make_unique<fl::FreeListDynamicMultipleProducerSingleConsumer<TestNode>>(c_freeListSize);
    testReallocations(freeList);
}

TEST(FreeListTest, testReallocationsDynamicMTMT)
{
    auto freeList = std::make_unique<fl::FreeListDynamicMultipleProducerMultipleConsumer<TestNode>>(c_freeListSize);
    testReallocations(freeList);
}

template<typename T>
void allocatorTestThread(std::shared_ptr<T> freeList)
{
    std::vector<typename T::ptr> nodes(c_freeListSize);

    for (std::size_t i = 0; i < c_freeListSize; ++i) {
        auto node = freeList->construct(i, i);

        if (node) {
            nodes[i] = std::move(node);
        }
        else {
            break;
        }
    }

    for (std::size_t i = 0; i < c_freeListSize; ++i) {
        if (nodes[i] != nullptr) {
            nodes[i] = nullptr;
        }
        else {
            break;
        }
    }
}

template<typename T>
void testMultithreaded(std::shared_ptr<T> freeList)
{
    constexpr std::size_t numThreads = 4;
    std::array<std::future<void>, numThreads> fut;

    for (std::size_t i = 0; i < numThreads; ++i) {
        fut[i] = std::async(std::launch::async, allocatorTestThread<T>, freeList);
    }

    for (auto& f : fut) {
        f.wait();
    }

    // If we get here the code didn't lock up
    ASSERT_TRUE(true);
}

TEST(FreeListTest, testMultithreadedStaticMTMT)
{
    auto freeList = std::make_shared<fl::FreeListStaticMultipleProducerMultipleConsumer<TestNode, c_freeListSize>>();
    testMultithreaded(freeList);
}

TEST(FreeListTest, testMultithreadedDynamicMTMT)
{
    auto freeList = std::make_shared<fl::FreeListDynamicMultipleProducerMultipleConsumer<TestNode>>(c_freeListSize);
    testMultithreaded(freeList);
}

// --- New coverage tests ---

// Test move-only types (verifies forwarding references work)
TEST(FreeListTest, testMoveOnlyTypeStatic)
{
    constexpr std::size_t size = 100;
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer<MoveOnlyNode, size>>();

    std::vector<typename decltype(freeList)::element_type::ptr> nodes(size);

    for (std::size_t i = 0; i < size; ++i) {
        auto node = freeList->construct(std::string("node_") + std::to_string(i), static_cast<unsigned>(i));
        ASSERT_TRUE(node != nullptr);
        EXPECT_EQ(node->m_name, std::string("node_") + std::to_string(i));
        EXPECT_EQ(node->m_val, i);
        nodes[i] = std::move(node);
    }

    ASSERT_FALSE(freeList->construct(std::string("overflow"), 0U));

    for (auto& n : nodes) {
        n = nullptr;
    }
}

TEST(FreeListTest, testMoveOnlyTypeDynamic)
{
    constexpr std::size_t size = 100;
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer<MoveOnlyNode>>(size);

    std::vector<typename decltype(freeList)::element_type::ptr> nodes(size);

    for (std::size_t i = 0; i < size; ++i) {
        auto node = freeList->construct(std::string("node_") + std::to_string(i), static_cast<unsigned>(i));
        ASSERT_TRUE(node != nullptr);
        EXPECT_EQ(node->m_name, std::string("node_") + std::to_string(i));
        EXPECT_EQ(node->m_val, i);
        nodes[i] = std::move(node);
    }

    ASSERT_FALSE(freeList->construct(std::string("overflow"), 0U));

    for (auto& n : nodes) {
        n = nullptr;
    }
}

// Test high-alignment types (verifies deleter offsetof fix)
TEST(FreeListTest, testHighAlignmentStatic)
{
    constexpr std::size_t size = 100;
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer<HighAlignNode, size>>();

    std::vector<typename decltype(freeList)::element_type::ptr> nodes(size);

    for (std::size_t i = 0; i < size; ++i) {
        auto node = freeList->construct(static_cast<unsigned>(i), static_cast<unsigned>(i * 2));
        ASSERT_TRUE(node != nullptr);

        // Verify alignment
        ASSERT_EQ(reinterpret_cast<std::uintptr_t>(node.get()) % alignof(HighAlignNode), 0U);

        EXPECT_EQ(node->m_val1, i);
        EXPECT_EQ(node->m_val2, i * 2);
        nodes[i] = std::move(node);
    }

    ASSERT_FALSE(freeList->construct(0U, 0U));

    // Destroy and verify no crash
    for (auto& n : nodes) {
        n = nullptr;
    }
}

TEST(FreeListTest, testHighAlignmentDynamic)
{
    constexpr std::size_t size = 100;
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer<HighAlignNode>>(size);

    std::vector<typename decltype(freeList)::element_type::ptr> nodes(size);

    for (std::size_t i = 0; i < size; ++i) {
        auto node = freeList->construct(static_cast<unsigned>(i), static_cast<unsigned>(i * 2));
        ASSERT_TRUE(node != nullptr);

        ASSERT_EQ(reinterpret_cast<std::uintptr_t>(node.get()) % alignof(HighAlignNode), 0U);

        EXPECT_EQ(node->m_val1, i);
        EXPECT_EQ(node->m_val2, i * 2);
        nodes[i] = std::move(node);
    }

    ASSERT_FALSE(freeList->construct(0U, 0U));

    for (auto& n : nodes) {
        n = nullptr;
    }
}

// Test destructor tracking (verifies lifetime correctness with destroy_at/start_lifetime_as)
TEST(FreeListTest, testDestructorCalledOnRelease)
{
    constexpr std::size_t size = 100;
    LifetimeNode::s_liveCount.store(0, std::memory_order_relaxed);

    {
        auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer<LifetimeNode>>(size);

        std::vector<typename decltype(freeList)::element_type::ptr> nodes(size);

        for (std::size_t i = 0; i < size; ++i) {
            nodes[i] = freeList->construct(static_cast<unsigned>(i), static_cast<unsigned>(i));
            ASSERT_TRUE(nodes[i] != nullptr);
        }

        EXPECT_EQ(LifetimeNode::s_liveCount.load(std::memory_order_relaxed), static_cast<int>(size));

        // Release half
        for (std::size_t i = 0; i < size / 2; ++i) {
            nodes[i] = nullptr;
        }

        EXPECT_EQ(LifetimeNode::s_liveCount.load(std::memory_order_relaxed), static_cast<int>(size / 2));

        // Reallocate into freed slots
        for (std::size_t i = 0; i < size / 2; ++i) {
            nodes[i] = freeList->construct(static_cast<unsigned>(i + 1000), static_cast<unsigned>(i + 1000));
            ASSERT_TRUE(nodes[i] != nullptr);
        }

        EXPECT_EQ(LifetimeNode::s_liveCount.load(std::memory_order_relaxed), static_cast<int>(size));

        // Release all
        for (auto& n : nodes) {
            n = nullptr;
        }

        EXPECT_EQ(LifetimeNode::s_liveCount.load(std::memory_order_relaxed), 0);
    }
}

TEST(FreeListTest, testDestructorCalledMultithreaded)
{
    constexpr std::size_t size = 10000;
    LifetimeNode::s_liveCount.store(0, std::memory_order_relaxed);

    auto freeList = std::make_shared<fl::FreeListDynamicMultipleProducerMultipleConsumer<LifetimeNode>>(size);

    constexpr std::size_t numThreads = 4;
    std::array<std::future<void>, numThreads> futures;

    for (std::size_t t = 0; t < numThreads; ++t) {
        futures[t] = std::async(std::launch::async, [&freeList]() {
            std::vector<typename std::remove_reference_t<decltype(*freeList)>::ptr> nodes;
            nodes.reserve(size);

            // Allocate as many as we can
            for (std::size_t i = 0; i < size; ++i) {
                auto node = freeList->construct(static_cast<unsigned>(i), static_cast<unsigned>(i));
                if (!node) break;
                nodes.push_back(std::move(node));
            }

            // Release all
            nodes.clear();
        });
    }

    for (auto& f : futures) {
        f.wait();
    }

    EXPECT_EQ(LifetimeNode::s_liveCount.load(std::memory_order_relaxed), 0);
}

// Test single-element edge case (N=1)
TEST(FreeListTest, testSingleElementStatic)
{
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer<TestNode, 1>>();

    auto node = freeList->construct(42U, 99U);
    ASSERT_TRUE(node != nullptr);
    EXPECT_EQ(node->m_val1, 42U);
    EXPECT_EQ(node->m_val2, 99U);

    // List should be exhausted
    ASSERT_FALSE(freeList->construct(0U, 0U));

    // Release and reallocate
    node = nullptr;

    node = freeList->construct(100U, 200U);
    ASSERT_TRUE(node != nullptr);
    EXPECT_EQ(node->m_val1, 100U);
    EXPECT_EQ(node->m_val2, 200U);
}

TEST(FreeListTest, testSingleElementDynamic)
{
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer<TestNode>>(1);

    auto node = freeList->construct(42U, 99U);
    ASSERT_TRUE(node != nullptr);
    EXPECT_EQ(node->m_val1, 42U);
    EXPECT_EQ(node->m_val2, 99U);

    ASSERT_FALSE(freeList->construct(0U, 0U));

    node = nullptr;

    node = freeList->construct(100U, 200U);
    ASSERT_TRUE(node != nullptr);
    EXPECT_EQ(node->m_val1, 100U);
    EXPECT_EQ(node->m_val2, 200U);
}

// Test concurrent producer/consumer (separate allocate and deallocate threads)
TEST(FreeListTest, testConcurrentProducerConsumer)
{
    constexpr std::size_t size = 100000;
    auto freeList = std::make_shared<fl::FreeListDynamicMultipleProducerMultipleConsumer<TestNode>>(size);

    // Shared queue between producer and consumer
    std::vector<typename std::remove_reference_t<decltype(*freeList)>::ptr> produced(size);
    std::atomic<std::size_t> produceCount{0};
    std::atomic<bool> producerDone{false};

    // Producer thread: allocates nodes
    auto producer = std::async(std::launch::async, [&]() {
        for (std::size_t i = 0; i < size; ++i) {
            auto node = freeList->construct(static_cast<unsigned>(i), static_cast<unsigned>(i));
            if (!node) break;
            produced[i] = std::move(node);
            produceCount.store(i + 1, std::memory_order_release);
        }
        producerDone.store(true, std::memory_order_release);
    });

    // Consumer thread: destroys nodes as they become available
    auto consumer = std::async(std::launch::async, [&]() {
        std::size_t consumed = 0;
        while (!producerDone.load(std::memory_order_acquire) || consumed < produceCount.load(std::memory_order_acquire)) {
            auto available = produceCount.load(std::memory_order_acquire);
            while (consumed < available) {
                produced[consumed] = nullptr;
                ++consumed;
            }
        }
    });

    producer.wait();
    consumer.wait();

    // All nodes should be freed
    for (std::size_t i = 0; i < size; ++i) {
        ASSERT_FALSE(produced[i]);
    }
}
