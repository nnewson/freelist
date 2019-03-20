#include <gtest/gtest.h>

#include <freelist.h>

#include <vector>
#include <future>
#include "../include/freelist.h"

// Constants
constexpr size_t c_freeListSize = 10000000;

// Types
struct TestNode {
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

struct AlignmentNode {
    AlignmentNode(unsigned val1, unsigned val2)
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

struct ExceptionNode {
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

TEST(FreeListTest, testStaticAlignment) {
    auto freeList = std::make_unique<fl::FreeListStaticSingleProducerSingleConsumer< AlignmentNode, c_freeListSize > >();
    testAlignment(freeList);
}

TEST(FreeListTest, testDynamicAlignment) {
    auto freeList = std::make_unique<fl::FreeListDynamicSingleProducerSingleConsumer< AlignmentNode > >(c_freeListSize);
    testAlignment(freeList);
}