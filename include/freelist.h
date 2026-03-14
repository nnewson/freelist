#ifndef FL_FREELIST_H
#define FL_FREELIST_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>

namespace fl
{

// Polyfill for detail::start_lifetime_as (P2590R2) until compiler support lands.
// For implicit-lifetime types with a defaulted default constructor,
// placement-new default construction is a correct substitute: it begins the
// object's lifetime at the given address. The subsequent setNext() call
// overwrites the value-initialized m_next, so the extra initialization is benign.
namespace detail
{
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
using detail::start_lifetime_as;
#else
template <typename T>
T* start_lifetime_as(void* ptr) noexcept
{
    return ::new (ptr) T;
}
#endif
} // namespace detail

class FreeListNode;

// Tagged pointer to prevent ABA problem in lock-free operations.
// Packs a 16-bit generation counter into the upper bits of a pointer-sized
// word so that std::atomic<TaggedPtr> is always lock-free. Safe on all
// mainstream 64-bit platforms (x86_64 48-bit VA, ARM64 48-bit VA, macOS 47-bit VA).
// Not compatible with 5-level paging (LA57) or ARM64 PAC/MTE — see README.md.
class TaggedPtr
{
    static constexpr int TAG_BITS = 16;
    static constexpr int TAG_SHIFT = sizeof(std::uintptr_t) * 8 - TAG_BITS;
    static constexpr std::uintptr_t PTR_MASK = (std::uintptr_t{1} << TAG_SHIFT) - 1;
    static constexpr std::uintptr_t TAG_MAX = (std::uintptr_t{1} << TAG_BITS) - 1;

    std::uintptr_t m_value{0};

public:
    static_assert(sizeof(void*) == 8, "Tagged pointer requires a 64-bit platform");

    TaggedPtr() = default;

    TaggedPtr(FreeListNode* p, std::uintptr_t t)
        : m_value(reinterpret_cast<std::uintptr_t>(p) |
                  ((t & TAG_MAX) << TAG_SHIFT))
    {
        assert((reinterpret_cast<std::uintptr_t>(p) & ~PTR_MASK) == 0 &&
               "Pointer uses upper bits - tagged pointer packing is unsafe");
    }

    FreeListNode* ptr() const noexcept
    {
        return reinterpret_cast<FreeListNode*>(m_value & PTR_MASK);
    }

    std::uintptr_t tag() const noexcept
    {
        return m_value >> TAG_SHIFT;
    }

    bool operator==(const TaggedPtr&) const noexcept = default;
};

static_assert(sizeof(TaggedPtr) == sizeof(std::uintptr_t),
              "TaggedPtr must be pointer-sized for lock-free atomics");

// Private Implementation Classes
class FreeListNode
{
public:
    void setNext(FreeListNode* const node) noexcept
    {
        m_next.store(node, std::memory_order_release);
    }

    FreeListNode* next() const noexcept
    {
        return m_next.load(std::memory_order_acquire);
    }

    FreeListNode() = default;
    ~FreeListNode() = default;

    FreeListNode(const FreeListNode&) = delete;
    FreeListNode(FreeListNode&&) = delete;
    FreeListNode& operator=(const FreeListNode&) = delete;
    FreeListNode& operator=(FreeListNode&&) = delete;

private:
    std::atomic<FreeListNode*> m_next{nullptr};
};

template <typename T>
struct FreeListAlloc
{
    template <typename... Args>
    explicit FreeListAlloc(void* const alloc, Args&&... args)
        : m_allocator(alloc),
          m_data(std::forward<Args>(args)...)
    {
    }

    const void* m_allocator;
    T m_data;
};

// Public Interface Classes
template <typename T, typename Allocator>
class FreeListDeleter
{
public:
    void operator()(T* p) const noexcept
    {
        static constexpr auto data_offset = offsetof(FreeListAlloc<T>, m_data);
        auto* base =
            reinterpret_cast<FreeListAlloc<T>*>(reinterpret_cast<std::byte*>(p) - data_offset);
        auto* alloc = const_cast<Allocator*>(static_cast<const Allocator*>(base->m_allocator));
        alloc->destroy(base);
    }
};

template <typename T, typename Allocator>
    requires(std::atomic<TaggedPtr>::is_always_lock_free)
class FreeListMTConstruct
{
public:
    using Deleter = FreeListDeleter<T, Allocator>;
    using ptr = std::unique_ptr<T, Deleter>;

    FreeListMTConstruct() = default;
    ~FreeListMTConstruct() = default;

    void setHead(FreeListNode* const node) noexcept
    {
        m_head.store(TaggedPtr{node, 0}, std::memory_order_release);
    }

    // Multi-threaded Construct - Lock free
    template <typename... Args>
    ptr construct(Args&&... args)
    {
        auto head = m_head.load(std::memory_order_acquire);
        FreeListNode* next = nullptr;
        do
        {
            next = head.ptr()->next();
        } while (next && !m_head.compare_exchange_weak(
                             head, TaggedPtr{next, head.tag() + 1},
                             std::memory_order_acq_rel, std::memory_order_acquire));

        if (next)
        {
            try
            {
                auto* rtnObj = std::construct_at(
                    reinterpret_cast<FreeListAlloc<T>*>(head.ptr()),
                    static_cast<void*>(this), std::forward<Args>(args)...);
                return ptr(&rtnObj->m_data);
            }
            catch (...)
            {
                // A constructor throw. Push the node back onto the list
                auto* freeNode = detail::start_lifetime_as<FreeListNode>(head.ptr());
                auto expected = m_head.load(std::memory_order_acquire);
                do
                {
                    freeNode->setNext(expected.ptr());
                } while (!m_head.compare_exchange_weak(
                    expected, TaggedPtr{freeNode, expected.tag() + 1},
                    std::memory_order_acq_rel, std::memory_order_acquire));

                throw;
            }
        }
        else
        {
            return nullptr;
        }
    }

protected:
    FreeListMTConstruct(const FreeListMTConstruct&) = delete;
    FreeListMTConstruct(FreeListMTConstruct&&) = delete;
    FreeListMTConstruct& operator=(const FreeListMTConstruct&) = delete;
    FreeListMTConstruct& operator=(FreeListMTConstruct&&) = delete;

    std::atomic<TaggedPtr> m_head{};
};

template <typename T, typename Allocator>
class FreeListSTConstruct
{
public:
    using Deleter = FreeListDeleter<T, Allocator>;
    using ptr = std::unique_ptr<T, Deleter>;

    FreeListSTConstruct() = default;
    ~FreeListSTConstruct() = default;

    void setHead(FreeListNode* const node) noexcept
    {
        m_head = node;
    }

    // Single-threaded Construct - Wait free
    template <typename... Args>
    ptr construct(Args&&... args)
    {
        auto next = m_head->next();

        if (next)
        {
            try
            {
                auto* rtnObj = std::construct_at(
                    reinterpret_cast<FreeListAlloc<T>*>(m_head),
                    static_cast<void*>(this), std::forward<Args>(args)...);
                m_head = next;
                return ptr(&rtnObj->m_data);
            }
            catch (...)
            {
                // A constructor throw. Repair head
                m_head = detail::start_lifetime_as<FreeListNode>(m_head);
                m_head->setNext(next);
                throw;
            }
        }
        else
        {
            return nullptr;
        }
    }

protected:
    FreeListSTConstruct(const FreeListSTConstruct&) = delete;
    FreeListSTConstruct(FreeListSTConstruct&&) = delete;
    FreeListSTConstruct& operator=(const FreeListSTConstruct&) = delete;
    FreeListSTConstruct& operator=(FreeListSTConstruct&&) = delete;

    FreeListNode* m_head{nullptr};
};

template <typename T>
class FreeListMTDestroy
{
public:
    FreeListMTDestroy() = default;
    ~FreeListMTDestroy() = default;

    void setTail(FreeListNode* const node) noexcept
    {
        m_tail.store(node, std::memory_order_release);
    }

    // Multi-threaded destroy - Wait free - assumes node is non-null
    void destroy(FreeListAlloc<T>* const node) noexcept
    {
        std::destroy_at(&node->m_data);
        auto* freeNode = detail::start_lifetime_as<FreeListNode>(node);
        freeNode->setNext(nullptr);
        auto* prevNode = m_tail.exchange(freeNode, std::memory_order_acq_rel);
        prevNode->setNext(freeNode);
    }

protected:
    FreeListMTDestroy(const FreeListMTDestroy&) = delete;
    FreeListMTDestroy(FreeListMTDestroy&&) = delete;
    FreeListMTDestroy& operator=(const FreeListMTDestroy&) = delete;
    FreeListMTDestroy& operator=(FreeListMTDestroy&&) = delete;

    std::atomic<FreeListNode*> m_tail;
};

template <typename T>
class FreeListSTDestroy
{
public:
    FreeListSTDestroy() = default;
    ~FreeListSTDestroy() = default;

    void setTail(FreeListNode* const node) noexcept
    {
        m_tail = node;
    }

    // Single-threaded destroy - Wait free - assumes node is non-null
    void destroy(FreeListAlloc<T>* const node) noexcept
    {
        std::destroy_at(&node->m_data);
        auto* freeNode = detail::start_lifetime_as<FreeListNode>(node);
        freeNode->setNext(nullptr);
        m_tail->setNext(freeNode);
        m_tail = freeNode;
    }

protected:
    FreeListSTDestroy(const FreeListSTDestroy&) = delete;
    FreeListSTDestroy(FreeListSTDestroy&&) = delete;
    FreeListSTDestroy& operator=(const FreeListSTDestroy&) = delete;
    FreeListSTDestroy& operator=(FreeListSTDestroy&&) = delete;

    FreeListNode* m_tail{nullptr};
};

template <typename T, template <typename, typename> class Construct,
          template <typename> class Destroy>
class FreeListBase
{
public:
    using Deleter = typename Construct<T, FreeListBase>::Deleter;
    using ptr = typename Construct<T, FreeListBase>::ptr;

    template <typename... Args>
    ptr construct(Args&&... args)
    {
        return m_construct.construct(std::forward<Args>(args)...);
    }

    void destroy(FreeListAlloc<T>* const node) noexcept
    {
        m_destroy.destroy(node);
    }

protected:
    FreeListBase() = default;
    ~FreeListBase() = default;

    FreeListBase(const FreeListBase&) = delete;
    FreeListBase(FreeListBase&&) = delete;
    FreeListBase& operator=(const FreeListBase&) = delete;
    FreeListBase& operator=(FreeListBase&&) = delete;

    void initFreeList(FreeListAlloc<T>* const array, const size_t size) noexcept
    {
        m_construct.setHead(detail::start_lifetime_as<FreeListNode>(&array[0]));
        m_destroy.setTail(detail::start_lifetime_as<FreeListNode>(&array[size]));

        // Point each array element to the subsequent one
        auto* prevNode = detail::start_lifetime_as<FreeListNode>(&array[0]);
        for (size_t i = 1; i <= size; ++i)
        {
            auto* freeNode = detail::start_lifetime_as<FreeListNode>(&array[i]);
            prevNode->setNext(freeNode);
            prevNode = freeNode;
        }
        prevNode->setNext(nullptr);
    }

private:
    alignas(std::hardware_destructive_interference_size) Construct<T, FreeListBase> m_construct;
    alignas(std::hardware_destructive_interference_size) Destroy<T> m_destroy;
};

// Always allocate to N + 1, so array has a sentinel if it's fully used
template <typename T, size_t N, template <typename, class> class Construct,
          template <typename> class Destroy>
    requires(sizeof(T) >= sizeof(FreeListNode)) && (N >= 1)
class FreeListStatic : public FreeListBase<T, Construct, Destroy>
{
public:
    FreeListStatic() noexcept
    {
        FreeListBase<T, Construct, Destroy>::initFreeList(reinterpret_cast<AllocT*>(m_array), N);
    }

    ~FreeListStatic() = default;

private:
    FreeListStatic(const FreeListStatic&) = delete;
    FreeListStatic(FreeListStatic&&) = delete;
    FreeListStatic& operator=(const FreeListStatic&) = delete;
    FreeListStatic& operator=(FreeListStatic&&) = delete;

    using AllocT = FreeListAlloc<T>;
    alignas(AllocT) std::byte m_array[sizeof(AllocT) * (N + 1)];
};

// Always allocate to size + 1, so array has a sentinel if it's fully used
template <typename T, template <typename, class> class Construct, template <typename> class Destroy>
    requires(sizeof(T) >= sizeof(FreeListNode))
class FreeListDynamic : public FreeListBase<T, Construct, Destroy>
{
public:
    explicit FreeListDynamic(const size_t size)
    {
        if ((m_array = reinterpret_cast<AllocT*>(
                 std::aligned_alloc(alignof(AllocT), sizeof(AllocT) * (size + 1)))) == nullptr)
        {
            throw std::bad_alloc();
        }
        FreeListBase<T, Construct, Destroy>::initFreeList(m_array, size);
    }

    ~FreeListDynamic()
    {
        std::free(m_array);
    }

private:
    FreeListDynamic(const FreeListDynamic&) = delete;
    FreeListDynamic(FreeListDynamic&&) = delete;
    FreeListDynamic& operator=(const FreeListDynamic&) = delete;
    FreeListDynamic& operator=(FreeListDynamic&&) = delete;

    using AllocT = FreeListAlloc<T>;
    AllocT* m_array{nullptr};
};

template <typename T>
using FreeListDynamicSingleProducerSingleConsumer =
    FreeListDynamic<T, FreeListSTConstruct, FreeListSTDestroy>;
template <typename T>
using FreeListDynamicSingleProducerMultipleConsumer =
    FreeListDynamic<T, FreeListSTConstruct, FreeListMTDestroy>;
template <typename T>
using FreeListDynamicMultipleProducerSingleConsumer =
    FreeListDynamic<T, FreeListMTConstruct, FreeListSTDestroy>;
template <typename T>
using FreeListDynamicMultipleProducerMultipleConsumer =
    FreeListDynamic<T, FreeListMTConstruct, FreeListMTDestroy>;

template <typename T, size_t N>
using FreeListStaticSingleProducerSingleConsumer =
    FreeListStatic<T, N, FreeListSTConstruct, FreeListSTDestroy>;
template <typename T, size_t N>
using FreeListStaticSingleProducerMultipleConsumer =
    FreeListStatic<T, N, FreeListSTConstruct, FreeListMTDestroy>;
template <typename T, size_t N>
using FreeListStaticMultipleProducerSingleConsumer =
    FreeListStatic<T, N, FreeListMTConstruct, FreeListSTDestroy>;
template <typename T, size_t N>
using FreeListStaticMultipleProducerMultipleConsumer =
    FreeListStatic<T, N, FreeListMTConstruct, FreeListMTDestroy>;
} // namespace fl

#endif // FL_FREELIST_H
