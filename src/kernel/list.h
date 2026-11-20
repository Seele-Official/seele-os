#pragma once

// Two doubly linked lists built on one link engine.
//
// IntrusiveList<T> chains the caller's objects together: T must expose `next` and
// `prev` as T*, which the intrusive_node concept checks at the point of
// instantiation. Linking allocates nothing and owns nothing, and erasing an
// element works from the element alone, so a node costs two words whichever
// container it belongs to. Every push, insert and erase below is O(1).
//
// The intrusive surface is pointer-based: the inserting and erasing operations
// take T*, and an empty pop_front()/pop_back()/front()/back() returns nullptr.
// A caller that does not know whether anything is queued can therefore ask and
// remove in one call instead of testing empty() first.
//
// The links form a ring with no sentinel object: `m_head` names the first
// element, the last element's next points back at the first, and the first
// element's prev points at the last. An empty list is `m_head == nullptr`, and a
// node that is not in a list has both links null. A sentinel *object* would have
// had to be a T, which would force every element type to be default-constructible
// and would put a fake element in the object's footprint; keeping just a head
// pointer avoids both. The cost is that end() is not a real node but a null
// iterator that remembers its list, which is what lets `--end()` still name the
// last element and `++last` become end() again.
//
// The list is circular, so every insertion and removal below is the same four
// stores regardless of which end the caller touched. A node must be unlinked
// before it is pushed; the link state is a caller bug, checked with SEELE_ASSERT
// rather than silently repaired. A node passed to erase(), insert_before() or
// insert_after() must belong to this list. There is no back-pointer from a node
// to its list, so membership cannot be checked cheaply and remains a documented
// precondition (erase() can only catch a singleton from another list). Node
// fields are public by design: the list is the link mechanism, not the element's
// owner.
//
// List<T> is the owning form, implemented on top of IntrusiveList rather than a
// second set of link operations. Each element is a heap node holding the two links
// followed by the T, so an empty List<T> does not need a T; push_front/push_back
// allocate from the kernel heap, and pop/clear/destruction delete. The heap cannot
// fail a `new` expression -- the kernel runs with exceptions off and operator new
// panics on exhaustion (mm/heap.cc) -- so a push either succeeds or stops the
// hart; there is no empty-result state to inspect. A node has to fit the heap's
// largest size class, which MAX_SIZE bounds, so the static_assert below rejects a
// List<T> whose T is too large to allocate at compile time instead of failing the
// boot on the first push.
//
// There is deliberately no middle insertion-for-List, erase-for-List, splice,
// sort or resize. A caller that needs a queue, a run queue or a wait list only
// ever touches the ends; adding an iterator-based erase to the owning list would
// expose an invalidation contract and a failure mode this kernel does not need
// yet. An intrusive caller that needs to remove an arbitrary node has erase().
//
// Iterators are bidirectional. For both containers begin() is the first element
// and end() is the null iterator; a mutable iterator converts to a const one.
// Inserting or removing at an end does not invalidate iterators to other nodes;
// the iterator to the node a pop/erase removed, and the iterator that referenced
// the list's end() after a clear(), are the only ones that go stale. Comparison
// between mutable and const iterators is supported in both orders.
//
// Copy and move are deleted for both types: the nodes are shared (intrusive) or
// owned (List), and either would need a deep copy or a sentinel rebuild that
// nothing in the kernel asks for yet. A list that must be handed from one owner
// to another belongs behind a pointer instead.
//
// A list carries no lock of its own. It is a plain data structure, so the owner
// serializes access with its own Spinlock when it can be reached from more than
// one hart at a time.

#include <concepts>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

#include "panic.h"
#include "mm/heap.h"

// The link contract shared by every list in this header. A node is intrusive
// precisely because the links live in the caller's object; requiring the members
// to be exactly T* (rather than merely convertible to T*) keeps the ordinary
// object pointer usable as the node pointer, so no offset or container_of cast is
// involved.
template <typename T>
concept intrusive_node = requires(T& node) {
    requires std::same_as<decltype(node.next), T*>;
    requires std::same_as<decltype(node.prev), T*>;
};

// An intrusive doubly linked list over T. The caller creates and destroys the T
// objects; this list only writes their `next` and `prev` fields. The list itself
// is two words (head pointer plus element count) and has a trivial destructor, so
// it can live in static storage without registering a static destructor.
//
// Nodes must outlive their membership: destroying a list without clear()/erase()
// leaves the nodes linked in their ring, which owns nothing but makes them
// unavailable to another list until they are unlinked. The list never dereferences
// a node it was not handed.
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

    // The first/last node, or nullptr when there is none. These return the node
    // rather than a reference into it because the list is a link mechanism; the
    // caller already has the object and only needs to know where it sits.
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

    // Insert an unlinked node at the front or the back. The list does not copy
    // the node: the caller keeps ownership and must keep it alive while linked.
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

    // Insert `node` next to `position`, which must be an element of this list.
    // insert_before() at the first element and insert_after() at the last are
    // therefore the two end insertions, expressed at an existing node.
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

    // Unlink the first/last node and return it to the caller, or return nullptr
    // when the list is empty. The node is not destroyed: its links are null again
    // and its lifetime remains the caller's. A pointer result and the null-on-
    // empty rule make both parts of that contract visible in the signature.
    [[nodiscard]] T* pop_front() noexcept {
        return empty() ? nullptr : take(m_head);
    }

    [[nodiscard]] T* pop_back() noexcept {
        return empty() ? nullptr : take(m_head->prev);
    }

    // Unlink an element chosen by the caller. This is the operation an intrusive
    // list exists for: it needs no iterator and no search. A null pointer is
    // "nothing to unlink" and is a no-op, which lets a caller pass an optional
    // link without testing it first.
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

    // Unlink every element, leaving the list empty. It does not destroy the
    // nodes and does not run T destructors: ownership stays with the caller.
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
    // A node is the caller's value preceded by the link pair that
    // IntrusiveList<node> writes. The links are first so the intrusive list's
    // node pointer is the owning list's node pointer, and an empty List<T> needs
    // no T: the head is a pointer, not a sentinel node.
    struct node {
        node* next = nullptr;
        node* prev = nullptr;
        T m_value;
    };

    // List<T> has one link implementation, not two: its node satisfies the same
    // concept the intrusive list imposes. If that ever stops holding, this
    // instantiation fails here rather than in a template error deep in a push.
    static_assert(intrusive_node<node>,
                  "List's node must satisfy the intrusive list's link contract");

    using Links = IntrusiveList<node>;

    // node is one `new` expression, and the heap has no size class above
    // MAX_SIZE: a larger node would panic inside operator new at the first push.
    // Reject it when the List is instantiated instead.
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

    // The owning iterator wraps the intrusive iterator and projects node to
    // node->m_value. It is still bidirectional, and the inner iterator's end()
    // keeps the list pointer, so --end() reaches the last value.
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
            return m_iterator->m_value;
        }

        [[nodiscard]] pointer operator->() const noexcept {
            return &m_iterator->m_value;
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

    // Nodes are owned, so leaving a list destroys them. This is not a static
    // storage-friendly type: see the note at the end of the file.
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
        return first->m_value;
    }

    [[nodiscard]] const_reference front() const noexcept {
        const node* first = m_nodes.front();
        SEELE_ASSERT(first != nullptr, "List::front on an empty list");
        return first->m_value;
    }

    [[nodiscard]] reference back() noexcept {
        node* last = m_nodes.back();
        SEELE_ASSERT(last != nullptr, "List::back on an empty list");
        return last->m_value;
    }

    [[nodiscard]] const_reference back() const noexcept {
        const node* last = m_nodes.back();
        SEELE_ASSERT(last != nullptr, "List::back on an empty list");
        return last->m_value;
    }

    // Insert at the front or the back. The node is allocated before the list is
    // touched, so a panic from the heap leaves the list exactly as it was.
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

    // Remove the first/last element and destroy it. Popping is a caller bug when
    // the list is empty; like the heap's free(), it is checked in every build
    // rather than silently doing nothing, because the list would otherwise be
    // corrupted.
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

    // Destroy every element, leaving the list empty. Iterators into the removed
    // elements are invalid afterwards; end() remains equal to end(). pop_front()
    // returning nullptr is the loop's natural stop condition.
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

// A List has a non-trivial destructor, and GCC registers a destructor for every
// namespace-scope object with one (GCC has no -fno-c++-static-destructors). The
// kernel currently provides neither atexit nor __cxa_atexit, so a static List
// would fail to link. Keep Lists in automatic storage, or reach a static one
// through a pointer (which does not register a destructor), until the libc
// grows those entry points.
//
// IntrusiveList has no such restriction: its destructor is trivial, and its
// nodes are the caller's, so a static IntrusiveList links without an exit hook.
