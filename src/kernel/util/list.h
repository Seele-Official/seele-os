#pragma once

// Intrusive and heap-owning circular doubly linked lists (O(1) push/pop/erase).
//
// - IntrusiveList<T>: non-owning, zero-allocation, trivially destructible
//   (safe in static storage). Requires public `T* next` and `T* prev` on T.
//   Pointer-based API returns nullptr when empty.
// - List<T>: owning wrapper built on IntrusiveList<node>; allocates nodes via
//   the kernel heap (sizeof/alignof <= heap::MAX_SIZE; panics on OOM). Non-
//   trivial destructor forbids namespace-scope static storage (no __cxa_atexit).
//
// Both types are non-copyable, non-movable, and unsynchronized (callers provide
// external locking). Iterators are bidirectional; insertion/removal invalidates
// only iterators to removed nodes.

#include <concepts>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

#include "panic.h"
#include "mm/heap.h"

template <typename T>
concept intrusive_node = requires(T& node) {
    requires std::same_as<decltype(node.next), T*>;
    requires std::same_as<decltype(node.prev), T*>;
};

// Non-owning circular doubly linked list with no sentinel object (`m_head ==
// nullptr` when empty; unlinked nodes have null links). Caller owns node
// lifetimes; destroying a non-empty list leaves nodes linked.
template <intrusive_node T>
class IntrusiveList {
private:
    template <bool IsConst>
    using list_pointer = std::conditional_t<IsConst, const IntrusiveList*, IntrusiveList*>;
    template <bool IsConst>
    using node_pointer = std::conditional_t<IsConst, const T*, T*>;

public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;

    // One iterator shape for both the mutable and const case; IsConst only
    // changes what dereferencing and arrow return. A mutable iterator converts
    // to a const one, never the other way around.
    template <bool IsConst>
    class Iterator {
        // The enclosing list builds iterators from (list, node) and the
        // conversion constructor reads the other specialization's fields.
        friend class IntrusiveList;
        template <bool OtherIsConst>
        friend class Iterator;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = std::conditional_t<IsConst, const T*, T*>;
        using reference = std::conditional_t<IsConst, const T&, T&>;

        // A default iterator compares equal to any other default iterator and
        // must not be dereferenced; there is deliberately no list to fall back
        // to, so both its list and its node are null.
        Iterator() noexcept = default;

        template <bool OtherIsConst>
            requires (IsConst && !OtherIsConst)
        Iterator(const Iterator<OtherIsConst>& other) noexcept :
            m_list(other.m_list), m_node(other.m_node) {}

        [[nodiscard]] reference operator* () const noexcept {
            return *m_node;
        }

        [[nodiscard]] pointer operator->() const noexcept {
            return m_node;
        }

        // The ring wraps at the head, so stepping past the last element becomes
        // end() rather than returning to begin(). A null node is a caller bug;
        // it is checked only in the Debug build because this is the hot loop of
        // every traversal.
        Iterator& operator++ () noexcept {
            SEELE_DASSERT(m_node != nullptr, "IntrusiveList: ++ past end()");
            m_node = m_node->next;
            if(m_node == m_list->m_head) {
                m_node = nullptr;
            }
            return *this;
        }

        Iterator operator++ (int) noexcept {
            Iterator old = *this;
            ++*this;
            return old;
        }

        // Decrementing end() needs the list to find the last element; every
        // other decrement just follows prev. Stepping back from begin() is the
        // same caller bug it is for a standard container (it would name the last
        // element here), so only the empty-list case is checked.
        Iterator& operator-- () noexcept {
            SEELE_DASSERT(m_list != nullptr, "IntrusiveList: -- on a default iterator");
            SEELE_DASSERT(!m_list->empty(), "IntrusiveList: -- on an empty list");
            m_node = m_node == nullptr ? m_list->m_head->prev : m_node->prev;
            return *this;
        }

        Iterator operator-- (int) noexcept {
            Iterator old = *this;
            --*this;
            return old;
        }

        // Compare the list as well as the node: two end() iterators from
        // different lists are not the same position, and a default iterator is
        // not the end() of an empty list. Mixed comparisons are intentionally
        // allowed, so a const_iterator and an iterator into the same list can
        // name the same node without the caller converting on one side.
        template <bool OtherIsConst>
        [[nodiscard]] bool operator== (const Iterator<OtherIsConst>& other) const noexcept {
            return m_list == other.m_list && m_node == other.m_node;
        }

        template <bool OtherIsConst>
        [[nodiscard]] bool operator!= (const Iterator<OtherIsConst>& other) const noexcept {
            return m_list != other.m_list || m_node != other.m_node;
        }

    private:
        Iterator(list_pointer<IsConst> list, node_pointer<IsConst> node) noexcept :
            m_list(list), m_node(node) {}

        list_pointer<IsConst> m_list = nullptr;
        node_pointer<IsConst> m_node = nullptr;
    };

    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    IntrusiveList() noexcept = default;

    // Nodes belong to the caller, so destruction does not unlink them; see the
    // type comment. This keeps the type usable in static storage.
    ~IntrusiveList() noexcept = default;

    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator= (const IntrusiveList&) = delete;
    IntrusiveList(IntrusiveList&&) = delete;
    IntrusiveList& operator= (IntrusiveList&&) = delete;

    [[nodiscard]] bool empty() const noexcept {
        return m_head == nullptr;
    }

    [[nodiscard]] size_type size() const noexcept {
        return m_size;
    }

    // First/last node, or nullptr when empty.
    [[nodiscard]] pointer front() noexcept {
        return m_head;
    }

    [[nodiscard]] const_pointer front() const noexcept {
        return m_head;
    }

    [[nodiscard]] pointer back() noexcept {
        return m_head == nullptr ? nullptr : m_head->prev;
    }

    [[nodiscard]] const_pointer back() const noexcept {
        return m_head == nullptr ? nullptr : m_head->prev;
    }

    // Push an unlinked non-null node; asserts on null or already-linked nodes.
    void push_front(T* node) {
        SEELE_ASSERT(node != nullptr, "IntrusiveList::push_front(nullptr)");
        require_unlinked(node);
        if(empty()) {
            link_self(node);
            m_head = node;
            ++m_size;
        } else {
            insert_before(m_head, node);
        }
    }

    void push_back(T* node) {
        SEELE_ASSERT(node != nullptr, "IntrusiveList::push_back(nullptr)");
        require_unlinked(node);
        if(empty()) {
            link_self(node);
            m_head = node;
            ++m_size;
        } else {
            insert_after(m_head->prev, node);
        }
    }

    // Insert unlinked `node` adjacent to `position` (must belong to this list).
    void insert_before(T* position, T* node) {
        SEELE_ASSERT(position != nullptr, "IntrusiveList::insert_before(nullptr)");
        SEELE_ASSERT(node != nullptr, "IntrusiveList::insert_before(nullptr)");
        require_linked(position);
        require_unlinked(node);
        node->prev = position->prev;
        node->next = position;
        position->prev->next = node;
        position->prev = node;
        if(m_head == position) {
            m_head = node;
        }
        ++m_size;
    }

    void insert_after(T* position, T* node) {
        SEELE_ASSERT(position != nullptr, "IntrusiveList::insert_after(nullptr)");
        SEELE_ASSERT(node != nullptr, "IntrusiveList::insert_after(nullptr)");
        require_linked(position);
        require_unlinked(node);
        node->next = position->next;
        node->prev = position;
        position->next->prev = node;
        position->next = node;
        ++m_size;
    }

    // Unlink and return the first/last node, or nullptr when empty.
    [[nodiscard]] T* pop_front() noexcept {
        return empty() ? nullptr : take(m_head);
    }

    [[nodiscard]] T* pop_back() noexcept {
        return empty() ? nullptr : take(m_head->prev);
    }

    // Unlink `node` (must belong to this list); nullptr is a no-op.
    void erase(T* node) noexcept {
        if(node == nullptr) {
            return;
        }
        SEELE_ASSERT(!empty(), "IntrusiveList::erase on an empty list");
        require_linked(node);

        if(node->next == node) {
            // The only element: the ring collapses to the empty list.
            SEELE_ASSERT(m_head == node, "IntrusiveList::erase of a node that is not in this list");
            m_head = nullptr;
        } else {
            node->prev->next = node->next;
            node->next->prev = node->prev;
            if(m_head == node) {
                m_head = node->next;
            }
        }
        node->next = nullptr;
        node->prev = nullptr;
        --m_size;
    }

    // Unlink every node without destroying them.
    void clear() noexcept {
        if(empty()) {
            return;
        }
        T* first = m_head;
        T* node = first;
        do {
            // Read the successor before clearing the node's links.
            T* next = node->next;
            node->next = nullptr;
            node->prev = nullptr;
            node = next;
        } while(node != first);
        m_head = nullptr;
        m_size = 0;
    }

    [[nodiscard]] iterator begin() noexcept {
        return iterator{this, m_head};
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return const_iterator{this, m_head};
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return const_iterator{this, m_head};
    }

    // end() is the null iterator; it remains equal to every end() of this list
    // across insertions and removals. It remembers the list only so that --end()
    // can reach the last element.
    [[nodiscard]] iterator end() noexcept {
        return iterator{this, nullptr};
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return const_iterator{this, nullptr};
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return const_iterator{this, nullptr};
    }

private:
    [[nodiscard]] static bool is_linked(const T* node) noexcept {
        return node->next != nullptr;
    }

    static void require_unlinked(const T* node) noexcept {
        SEELE_ASSERT(!is_linked(node), "IntrusiveList: node is already linked");
    }

    static void require_linked(const T* node) noexcept {
        SEELE_ASSERT(is_linked(node), "IntrusiveList: node is not linked");
    }

    // A one-element ring is its own successor and predecessor.
    static void link_self(T* node) noexcept {
        node->next = node;
        node->prev = node;
    }

    [[nodiscard]] T* take(T* node) noexcept {
        erase(node);
        return node;
    }

    T* m_head = nullptr;
    size_type m_size = 0;
};

template <typename T>
class List {
private:
    struct node {
        node* next = nullptr;
        node* prev = nullptr;
        T value;
    };

    static_assert(intrusive_node<node>,
                  "List's node must satisfy the intrusive list's link contract");

    using Links = IntrusiveList<node>;

    static_assert(sizeof(node) <= heap::MAX_SIZE,
                  "a List node must fit the kernel heap's largest size class");
    static_assert(alignof(node) <= heap::MAX_SIZE,
                  "a List node must be aligned within the heap's limit");

public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;

    // Wraps IntrusiveList<node>::Iterator and projects node to node->value.
    template <bool IsConst>
    class Iterator {
        friend class List;
        template <bool OtherIsConst>
        friend class Iterator;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = std::conditional_t<IsConst, const T*, T*>;
        using reference = std::conditional_t<IsConst, const T&, T&>;

        Iterator() noexcept = default;

        template <bool OtherIsConst>
            requires (IsConst && !OtherIsConst)
        Iterator(const Iterator<OtherIsConst>& other) noexcept : m_iterator(other.m_iterator) {}

        [[nodiscard]] reference operator* () const noexcept {
            return m_iterator->value;
        }

        [[nodiscard]] pointer operator->() const noexcept {
            return &m_iterator->value;
        }

        Iterator& operator++ () noexcept {
            ++m_iterator;
            return *this;
        }

        Iterator operator++ (int) noexcept {
            Iterator old = *this;
            ++*this;
            return old;
        }

        Iterator& operator-- () noexcept {
            --m_iterator;
            return *this;
        }

        Iterator operator-- (int) noexcept {
            Iterator old = *this;
            --*this;
            return old;
        }

        template <bool OtherIsConst>
        [[nodiscard]] bool operator== (const Iterator<OtherIsConst>& other) const noexcept {
            return m_iterator == other.m_iterator;
        }

        template <bool OtherIsConst>
        [[nodiscard]] bool operator!= (const Iterator<OtherIsConst>& other) const noexcept {
            return m_iterator != other.m_iterator;
        }

    private:
        using link_iterator =
            std::conditional_t<IsConst, typename Links::const_iterator, typename Links::iterator>;

        explicit Iterator(link_iterator iterator) noexcept : m_iterator(iterator) {}

        link_iterator m_iterator;
    };

    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    List() noexcept = default;

    // Non-trivial destructor: do not place List in namespace-scope static
    // storage (the kernel provides no atexit/__cxa_atexit).
    ~List() noexcept {
        clear();
    }

    List(const List&) = delete;
    List& operator= (const List&) = delete;
    List(List&&) = delete;
    List& operator= (List&&) = delete;

    [[nodiscard]] bool empty() const noexcept {
        return m_nodes.empty();
    }

    [[nodiscard]] size_type size() const noexcept {
        return m_nodes.size();
    }

    [[nodiscard]] reference front() noexcept {
        node* first = m_nodes.front();
        SEELE_ASSERT(first != nullptr, "List::front on an empty list");
        return first->value;
    }

    [[nodiscard]] const_reference front() const noexcept {
        const node* first = m_nodes.front();
        SEELE_ASSERT(first != nullptr, "List::front on an empty list");
        return first->value;
    }

    [[nodiscard]] reference back() noexcept {
        node* last = m_nodes.back();
        SEELE_ASSERT(last != nullptr, "List::back on an empty list");
        return last->value;
    }

    [[nodiscard]] const_reference back() const noexcept {
        const node* last = m_nodes.back();
        SEELE_ASSERT(last != nullptr, "List::back on an empty list");
        return last->value;
    }

    void push_front(const T& value) {
        push_front_node(value);
    }

    void push_front(T&& value) {
        push_front_node(std::move(value));
    }

    void push_back(const T& value) {
        push_back_node(value);
    }

    void push_back(T&& value) {
        push_back_node(std::move(value));
    }

    // Remove and destroy the first/last element; panics if empty.
    void pop_front() noexcept {
        node* element = m_nodes.pop_front();
        SEELE_ASSERT(element != nullptr, "List::pop_front on an empty list");
        delete element;
    }

    void pop_back() noexcept {
        node* element = m_nodes.pop_back();
        SEELE_ASSERT(element != nullptr, "List::pop_back on an empty list");
        delete element;
    }

    void clear() noexcept {
        while(node* element = m_nodes.pop_front()) {
            delete element;
        }
    }

    [[nodiscard]] iterator begin() noexcept {
        return iterator{m_nodes.begin()};
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return const_iterator{m_nodes.begin()};
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return const_iterator{m_nodes.cbegin()};
    }

    [[nodiscard]] iterator end() noexcept {
        return iterator{m_nodes.end()};
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return const_iterator{m_nodes.end()};
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return const_iterator{m_nodes.cend()};
    }

private:
    // Allocate the node first and link it second: a heap panic in `new node`
    // then leaves the list untouched.
    template <typename U>
    void push_front_node(U&& value) {
        node* element = new node{nullptr, nullptr, std::forward<U>(value)};
        m_nodes.push_front(element);
    }

    template <typename U>
    void push_back_node(U&& value) {
        node* element = new node{nullptr, nullptr, std::forward<U>(value)};
        m_nodes.push_back(element);
    }

    Links m_nodes;
};
