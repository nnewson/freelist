#ifndef FL_FREELIST_H
#define FL_FREELIST_H

#include <atomic>
#include <memory>

namespace fl {

    // Private Implementation Classes
    class FreeListNode {
    public:
        void setNext(FreeListNode *const node) noexcept {
            m_next.store(node, std::memory_order_release);
        }

        FreeListNode *next() const noexcept {
            return m_next.load(std::memory_order_acquire);
        }

    private:
        FreeListNode() = default;
        ~FreeListNode() = default;

        FreeListNode(const FreeListNode &) = delete;
        FreeListNode(FreeListNode &&) = delete;
        FreeListNode &operator=(const FreeListNode &) = delete;
        FreeListNode &operator=(FreeListNode &&) = delete;

        std::atomic<FreeListNode *> m_next;
    };

    template<typename T>
    struct FreeListAlloc {
        template<typename... Args>
        FreeListAlloc(void *const alloc, Args... args)
                : m_allocator(alloc), m_data(std::forward<Args>(args)...) {
        }

        const void *m_allocator;
        T m_data;
    };

    // Public Interface Classes
    template<typename T, typename Allocator>
    class FreeListDeleter {
    public:
        void operator()(T *p) const noexcept {
            auto cursor = reinterpret_cast<void **>(p) - 1;
            auto alloc = reinterpret_cast< Allocator * >(*cursor);
            alloc->destroy(reinterpret_cast< FreeListAlloc<T> * >(cursor));
        }
    };

    template<typename T, typename Allocator>
    class FreeListMTConstruct {
    public:
        using Deleter = FreeListDeleter<T, Allocator>;
        using ptr = std::unique_ptr<T, Deleter>;

        FreeListMTConstruct() = default;
        ~FreeListMTConstruct() = default;

        void setHead(FreeListNode* const node) noexcept {
            m_head.store(node, std::memory_order_release);
        }

        // Multi-threaded Construct - Lock free
        template<typename... Args>
        ptr construct(Args... args) {
            auto head = m_head.load(std::memory_order_acquire);
            FreeListNode *next = nullptr;
            do {
                next = head->next();
            } while (next &&
                     !m_head.compare_exchange_strong(head, next, std::memory_order_acq_rel, std::memory_order_acquire));

            if (next) {
                try {
                    auto rtnObj = new(reinterpret_cast< void * >(head)) FreeListAlloc<T>(this,
                                                                                         std::forward<Args>(args)...);
                    return ptr(&rtnObj->m_data);
                }
                catch (...) {
                    // A constructor throw. We need to repair head and put it back in the list
                    do {
                        head->setNext(next);
                    } while (!m_head.compare_exchange_strong(next, head, std::memory_order_acq_rel,
                                                             std::memory_order_acquire));

                    throw;
                }
            }
            else {
                return nullptr;
            }
        }

    protected:
        FreeListMTConstruct(const FreeListMTConstruct &) = delete;
        FreeListMTConstruct(FreeListMTConstruct &&) = delete;
        FreeListMTConstruct &operator=(const FreeListMTConstruct &) = delete;
        FreeListMTConstruct &operator=(FreeListMTConstruct &) = delete;

        std::atomic< FreeListNode* >    m_head;
    };

    template<typename T, typename Allocator>
    class FreeListSTConstruct {
    public:
        using Deleter = FreeListDeleter<T, Allocator>;
        using ptr = std::unique_ptr<T, Deleter>;

        FreeListSTConstruct() = default;
        ~FreeListSTConstruct() = default;

        void setHead(FreeListNode* const node) noexcept {
            m_head = node;
        }

        // Single-threaded Construct - Wait free
        template<typename... Args>
        ptr construct(Args... args) {
            auto next = m_head->next();

            if (next) {
                try {
                    auto rtnObj = new(reinterpret_cast< void * >(m_head)) FreeListAlloc<T>(this,
                                                                                           std::forward<Args>(args)...);
                    m_head = next;
                    return ptr(&rtnObj->m_data);
                }
                catch (...) {
                    // A constructor throw. Repair headm_
                    m_head->setNext(next);
                    throw;
                }
            }
            else {
                return nullptr;
            }
        }

    protected:
        FreeListSTConstruct(const FreeListSTConstruct &) = delete;
        FreeListSTConstruct(FreeListSTConstruct &&) = delete;
        FreeListSTConstruct &operator=(const FreeListSTConstruct &) = delete;
        FreeListSTConstruct &operator=(FreeListSTConstruct &) = delete;

        FreeListNode*                   m_head;
    };

    template < typename T >
    class FreeListMTDestroy {
    public:
        FreeListMTDestroy() = default;
        ~FreeListMTDestroy() = default;

        void setTail(FreeListNode* const node) noexcept {
            m_tail.store(node, std::memory_order_release);
        }

        // Multi-threaded destroy - Wait free - assumes node is non-null
        void destroy(FreeListAlloc<T>* const node) noexcept {
            node->m_data.~T();
            auto freeNode = reinterpret_cast< FreeListNode* >(node);
            freeNode->setNext(nullptr);
            auto prevNode = m_tail.exchange(freeNode, std::memory_order_acq_rel);
            prevNode->setNext(freeNode);
        }

    protected:
        FreeListMTDestroy(const FreeListMTDestroy &) = delete;
        FreeListMTDestroy(FreeListMTDestroy &&) = delete;
        FreeListMTDestroy &operator=(const FreeListMTDestroy &) = delete;
        FreeListMTDestroy &operator=(FreeListMTDestroy &) = delete;

        std::atomic< FreeListNode* >    m_tail;

    };

    template < typename T >
    class FreeListSTDestroy {
    public:
        FreeListSTDestroy() = default;
        ~FreeListSTDestroy() = default;

        void setTail(FreeListNode* const node) noexcept {
            m_tail = node;
        }

        // Single-threaded destroy - Wait free - assumes node is non-null
        void destroy(FreeListAlloc<T>* const node) noexcept {
            node->m_data.~T();
            auto freeNode = reinterpret_cast< FreeListNode* >(node);
            freeNode->setNext(nullptr);
            m_tail->setNext(freeNode);
            m_tail = freeNode;
        }

    protected:
        FreeListSTDestroy(const FreeListSTDestroy &) = delete;
        FreeListSTDestroy(FreeListSTDestroy &&) = delete;
        FreeListSTDestroy &operator=(const FreeListSTDestroy &) = delete;
        FreeListSTDestroy &operator=(FreeListSTDestroy &) = delete;

        FreeListNode*                   m_tail;

    };

    template< typename T, template< typename, typename > class Construct, template < typename > class Destroy >
    class FreeListBase {
    public:
        using Deleter = typename Construct< T, FreeListBase >::Deleter;
        using ptr = typename Construct< T, FreeListBase>::ptr;

        template< typename... Args >
        ptr construct(Args... args) {
            return m_construct.construct(std::forward< Args >(args)...);
        }

        void destroy(FreeListAlloc<T>* const node) noexcept {
            m_destroy.destroy(node);
        }

    protected:
        FreeListBase() = default;
        ~FreeListBase() = default;

        FreeListBase(const FreeListBase &) = delete;
        FreeListBase(FreeListBase &&) = delete;
        FreeListBase &operator=(const FreeListBase &) = delete;
        FreeListBase &operator=(FreeListBase &) = delete;

        void initFreeList(FreeListAlloc<T>* const array, const size_t size) noexcept {
            m_construct.setHead(reinterpret_cast< FreeListNode* >(&array[0]));
            m_destroy.setTail(reinterpret_cast< FreeListNode* >(&array[size]));

            // Point each array element toAlignmentNode the subsequent one
            auto prevNode = reinterpret_cast< FreeListNode* >(&array[0]);
            for (size_t i = 1 ; i <= size ; ++i) {
                auto freeNode = reinterpret_cast< FreeListNode* >(&array[i]);
                prevNode->setNext(freeNode);
                prevNode = freeNode;
            }
            prevNode->setNext(nullptr);
        }

    private:
        Construct< T, FreeListBase >    m_construct;
        Destroy< T >                    m_destroy;
    };

    // Always allocate to N + 1, so array has a sentinel if it's fully used
    template< typename T, size_t N , template < typename, class > class Construct, template < typename > class Destroy >
    class FreeListStatic : public FreeListBase< T, Construct, Destroy > {
    public:
        FreeListStatic() noexcept {
            static_assert(sizeof(T) >= sizeof(FreeListNode), "Size of T must be greater or equal to FreeListNode");
            static_assert(N >= 1, "N must be greater than 0");

            FreeListBase< T, Construct, Destroy >::initFreeList(reinterpret_cast<AllocT*>(m_array), N);
        }

        ~FreeListStatic() = default;

    private:
        FreeListStatic(const FreeListStatic &) = delete;
        FreeListStatic(FreeListStatic &&) = delete;
        FreeListStatic &operator=(const FreeListStatic &) = delete;
        FreeListStatic &operator=(FreeListStatic &) = delete;

        using AllocT = FreeListAlloc<T>;
        typename std::aligned_storage< sizeof(AllocT), alignof(AllocT)>::type
                                        m_array[N + 1];
    };

    // Always allocate to size + 1, so array has a sentinel if it's fully used
    template< typename T, template < typename, class > class Construct, template < typename > class Destroy >
    class FreeListDynamic : public FreeListBase< T, Construct, Destroy > {
    public:
        explicit FreeListDynamic(const size_t size) {
            static_assert(sizeof(T) >= sizeof(FreeListNode), "Size of T must be greater or equal to FreeListNode");

            if ( (m_array = reinterpret_cast< AllocT* >(std::aligned_alloc(alignof(AllocT), sizeof(AllocT) * (size + 1)))) == nullptr ) {
                throw std::bad_alloc();
            }
            FreeListBase< T, Construct, Destroy >::initFreeList(reinterpret_cast<AllocT*>(m_array), size);
        }

        ~FreeListDynamic() {
            std::free(m_array);
        }

    private:
        FreeListDynamic(const FreeListDynamic &) = delete;
        FreeListDynamic(FreeListDynamic &&) = delete;
        FreeListDynamic &operator=(const FreeListDynamic &) = delete;
        FreeListDynamic &operator=(FreeListDynamic &) = delete;

        using AllocT = FreeListAlloc<T>;
        AllocT*                         m_array;
    };

    template < typename T >
    using FreeListDynamicSingleProducerSingleConsumer       = FreeListDynamic< T, FreeListSTConstruct, FreeListSTDestroy >;
    template < typename T >
    using FreeListDynamicSingleProducerMultipleConsumer     = FreeListDynamic< T, FreeListSTConstruct, FreeListMTDestroy >;
    template < typename T >
    using FreeListDynamicMultipleProducerSingleConsumer     = FreeListDynamic< T, FreeListMTConstruct, FreeListSTDestroy >;
    template < typename T >
    using FreeListDynamicMultipleProducerMultipleConsumer   = FreeListDynamic< T, FreeListMTConstruct, FreeListMTDestroy >;

    template < typename T, size_t N >
    using FreeListStaticSingleProducerSingleConsumer        = FreeListStatic< T, N, FreeListSTConstruct, FreeListSTDestroy >;
    template < typename T, size_t N >
    using FreeListStaticSingleProducerMultipleConsumer      = FreeListStatic< T, N, FreeListSTConstruct, FreeListMTDestroy >;
    template < typename T, size_t N >
    using FreeListStaticMultipleProducerSingleConsumer      = FreeListStatic< T, N, FreeListMTConstruct, FreeListSTDestroy >;
    template < typename T, size_t N >
    using FreeListStaticMultipleProducerMultipleConsumer    = FreeListStatic< T, N, FreeListMTConstruct, FreeListMTDestroy >;
}

#endif //FL_FREELIST_H
