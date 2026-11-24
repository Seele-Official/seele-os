#pragma once

// Read-only flattened device tree (DTB v17) reader.
//
// Exposes a firmware DTB blob as typed, bounds-checked views: no allocation, no
// I/O, no mutation. Every read is checked against the header's totalsize so a
// truncated blob cannot walk past the buffer (the blob itself is still assumed
// to come from trusted firmware).
//
// This layer only answers "what bytes and nodes are in the tree". It knows
// nothing about devices, drivers, or kernel resource policy.

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "mm/addr.h"

namespace fdt {

// Maximum depth Tree::walk() descends below the root node (-DSEELE_FDT_MAX_DEPTH).
#ifndef SEELE_FDT_MAX_DEPTH
#error "SEELE_FDT_MAX_DEPTH is not defined: build the kernel through CMake (see CMakeLists.txt)"
#endif
constexpr uint32_t MAX_DEPTH = SEELE_FDT_MAX_DEPTH;

// Read a big-endian cell without requiring alignment. Avoids std::byteswap,
// which lowers to libgcc's __bswapsi2/__bswapdi2 on rv64gc without Zbb.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T load_be(const std::byte* bytes) noexcept {
    struct byte_storage {
        std::byte value[sizeof(T)];
    } storage{};

    static_assert(sizeof(byte_storage) == sizeof(T));

    if constexpr(std::endian::native == std::endian::little) {
        for(size_t i = 0; i < sizeof(T); ++i) {
            storage.value[sizeof(T) - 1 - i] = bytes[i];
        }
    } else {
        for(size_t i = 0; i < sizeof(T); ++i) {
            storage.value[i] = bytes[i];
        }
    }
    return std::bit_cast<T>(storage);
}

[[nodiscard]] constexpr uint32_t be32(const void* p) noexcept {
    return load_be<uint32_t>(static_cast<const std::byte*>(p));
}

[[nodiscard]] constexpr uint64_t be64(const void* p) noexcept {
    return load_be<uint64_t>(static_cast<const std::byte*>(p));
}

class Tree;
class Node;

// `#address-cells` / `#size-cells` that apply to a node's own `reg` property.
// Inherited from the parent bus, never from the node itself.
struct cell_counts {
    uint32_t address = 2;
    uint32_t size = 2;

    [[nodiscard]] friend constexpr bool operator== (const cell_counts&,
                                                    const cell_counts&) noexcept = default;
};

// Control flow returned by a Tree::walk() visitor.
enum class WalkAction : uint8_t {
    Continue,      // descend into this node's children
    SkipChildren,  // skip this node's subtree and continue with siblings
    Stop,          // stop the walk immediately
};

// Range-for view over non-empty NUL-terminated strings in a property value.
class StringList {
public:
    class Iterator {
    public:
        constexpr Iterator() noexcept = default;

        [[nodiscard]] constexpr std::string_view operator* () const noexcept {
            return m_current;
        }

        Iterator& operator++ () noexcept {
            advance();
            return *this;
        }

        [[nodiscard]] constexpr bool operator== (const Iterator& other) const noexcept {
            return m_done == other.m_done && (m_done || m_offset == other.m_offset);
        }

    private:
        friend class StringList;

        explicit Iterator(std::span<const std::byte> bytes) noexcept :
            m_bytes(bytes), m_done(false) {
            advance();
        }

        void advance() noexcept;

        std::span<const std::byte> m_bytes{};
        size_t m_offset = 0;
        std::string_view m_current{};
        bool m_done = true;
    };

    constexpr StringList() noexcept = default;

    constexpr explicit StringList(std::span<const std::byte> bytes) noexcept : m_bytes(bytes) {}

    [[nodiscard]] Iterator begin() const noexcept {
        return Iterator{m_bytes};
    }

    [[nodiscard]] constexpr Iterator end() const noexcept {
        return Iterator{};
    }

private:
    std::span<const std::byte> m_bytes{};
};

// Non-owning view of one property value inside the blob. Default-constructed
// means absent; a present empty property (boolean flag) has operator bool() == true.
class Property {
public:
    constexpr Property() noexcept = default;

    constexpr Property(const std::byte* data, size_t size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept {
        return {m_data, m_size};
    }

    [[nodiscard]] constexpr size_t size() const noexcept {
        return m_size;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return m_size == 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return m_data != nullptr;
    }

    [[nodiscard]] std::optional<uint32_t> u32() const noexcept;
    [[nodiscard]] std::optional<uint64_t> u64() const noexcept;
    [[nodiscard]] std::optional<uint64_t> u32_or_u64() const noexcept;
    [[nodiscard]] std::optional<std::string_view> string() const noexcept;

    [[nodiscard]] constexpr StringList strings() const noexcept {
        return StringList{bytes()};
    }

    [[nodiscard]] constexpr bool boolean() const noexcept {
        return m_data != nullptr;
    }

private:
    const std::byte* m_data = nullptr;
    size_t m_size = 0;
};

// Range-for view over non-zero `phys_range` entries of a `reg` property.
// Supports 1- or 2-cell addresses and sizes; skips zero-length entries.
class RegRange {
public:
    class Iterator {
    public:
        constexpr Iterator() noexcept = default;

        [[nodiscard]] constexpr phys_range operator* () const noexcept {
            return m_current;
        }

        Iterator& operator++ () noexcept {
            advance();
            return *this;
        }

        [[nodiscard]] constexpr bool operator== (const Iterator& other) const noexcept {
            return m_done == other.m_done && (m_done || m_offset == other.m_offset);
        }

    private:
        friend class RegRange;

        Iterator(Property reg, cell_counts cells) noexcept :
            m_reg(reg), m_cells(cells), m_done(false) {
            advance();
        }

        void advance() noexcept;

        Property m_reg{};
        cell_counts m_cells{};
        size_t m_offset = 0;
        phys_range m_current{};
        bool m_done = true;
    };

    constexpr RegRange() noexcept = default;

    constexpr RegRange(Property reg, cell_counts cells) noexcept : m_reg(reg), m_cells(cells) {}

    [[nodiscard]] Iterator begin() const noexcept {
        return Iterator{m_reg, m_cells};
    }

    [[nodiscard]] constexpr Iterator end() const noexcept {
        return Iterator{};
    }

private:
    Property m_reg{};
    cell_counts m_cells{};
};

// Range-for view over the memory reservation block (`/memreserve/`).
class ReservedRange {
public:
    class Iterator {
    public:
        constexpr Iterator() noexcept = default;

        [[nodiscard]] constexpr phys_range operator* () const noexcept {
            return m_current;
        }

        Iterator& operator++ () noexcept {
            advance();
            return *this;
        }

        [[nodiscard]] constexpr bool operator== (const Iterator& other) const noexcept {
            return m_done == other.m_done && (m_done || m_offset == other.m_offset);
        }

    private:
        friend class ReservedRange;

        Iterator(const Tree* tree, uint32_t offset) noexcept :
            m_tree(tree), m_offset(offset), m_done(false) {
            advance();
        }

        void advance() noexcept;

        const Tree* m_tree = nullptr;
        uint32_t m_offset = 0;
        phys_range m_current{};
        bool m_done = true;
    };

    constexpr ReservedRange() noexcept = default;

    constexpr ReservedRange(const Tree* tree, uint32_t offset) noexcept :
        m_tree(tree), m_offset(offset) {}

    [[nodiscard]] Iterator begin() const noexcept {
        return Iterator{m_tree, m_offset};
    }

    [[nodiscard]] constexpr Iterator end() const noexcept {
        return Iterator{};
    }

private:
    const Tree* m_tree = nullptr;
    uint32_t m_offset = 0;
};

class ChildRange;

// Lightweight handle to one node: owning tree pointer plus structure-block byte
// offset. Offset 0 is the invalid sentinel (valid nodes always start at or
// after the 40-byte DTB header). Every accessor is safe on an invalid Node.
class Node {
public:
    constexpr Node() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return m_tree != nullptr && m_offset != 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }

    // Structure-block byte offset, or 0 when invalid. Stable for the tree's
    // lifetime and reconstructible via Tree::node_at().
    [[nodiscard]] constexpr uint32_t offset() const noexcept {
        return valid() ? m_offset : 0;
    }

    [[nodiscard]] friend constexpr bool operator== (const Node& lhs, const Node& rhs) noexcept {
        if(!lhs.valid() || !rhs.valid()) {
            return lhs.valid() == rhs.valid();
        }
        return lhs.m_tree == rhs.m_tree && lhs.m_offset == rhs.m_offset;
    }

    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::string_view basename() const noexcept;

    [[nodiscard]] Node parent() const noexcept;
    [[nodiscard]] Node first_child() const noexcept;
    [[nodiscard]] Node next_sibling() const noexcept;
    [[nodiscard]] ChildRange children() const noexcept;

    [[nodiscard]] Property property(std::string_view name) const noexcept;
    [[nodiscard]] bool is_compatible(std::string_view id) const noexcept;

    // Status defaults to "okay"; anything other than "okay"/"ok" is disabled.
    [[nodiscard]] bool enabled() const noexcept;

    // `phandle`, falling back to legacy `linux,phandle`.
    [[nodiscard]] std::optional<uint32_t> phandle() const noexcept;

    // `#address-cells` / `#size-cells` inherited from parent chain (or directly
    // from an ancestor slice provided by Tree::walk() to avoid re-scanning).
    [[nodiscard]] cell_counts cells() const noexcept;
    [[nodiscard]] static cell_counts cells(std::span<const Node> ancestors) noexcept;

    // Non-zero (base, size) entries of this node's `reg` property.
    [[nodiscard]] RegRange regs() const noexcept;
    [[nodiscard]] RegRange regs(std::span<const Node> ancestors) const noexcept;

    // First address cell value of `reg`, ignoring size cells (e.g. /cpus/cpu@N).
    [[nodiscard]] std::optional<uint64_t> reg_address() const noexcept;

private:
    friend class Tree;

    constexpr Node(const Tree* tree, uint32_t offset) noexcept : m_tree(tree), m_offset(offset) {}

    const Tree* m_tree = nullptr;
    uint32_t m_offset = 0;
};

// Range-for view over a node's direct children.
class ChildRange {
public:
    class Iterator {
    public:
        constexpr Iterator() noexcept = default;

        constexpr explicit Iterator(Node node) noexcept : m_node(node) {}

        [[nodiscard]] constexpr Node operator* () const noexcept {
            return m_node;
        }

        Iterator& operator++ () noexcept {
            m_node = m_node.next_sibling();
            return *this;
        }

        [[nodiscard]] constexpr bool operator== (const Iterator& other) const noexcept {
            return m_node == other.m_node;
        }

    private:
        Node m_node{};
    };

    constexpr ChildRange() noexcept = default;

    constexpr explicit ChildRange(Node first) noexcept : m_first(first) {}

    [[nodiscard]] constexpr Iterator begin() const noexcept {
        return Iterator{m_first};
    }

    [[nodiscard]] constexpr Iterator end() const noexcept {
        return Iterator{};
    }

private:
    Node m_first{};
};

inline ChildRange Node::children() const noexcept {
    return ChildRange{first_child()};
}

class Tree {
public:
    constexpr Tree() noexcept = default;

    // Validates the DTB v17 header up front; an invalid blob leaves valid() ==
    // false while keeping every query safe.
    explicit Tree(const void* blob) noexcept;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return m_valid;
    }

    [[nodiscard]] constexpr uint32_t size() const noexcept {
        return m_total;
    }

    [[nodiscard]] Node root() const noexcept;
    [[nodiscard]] Node node_at(uint32_t offset) const noexcept;
    [[nodiscard]] Node find(std::string_view path) const noexcept;
    [[nodiscard]] Node by_phandle(uint32_t phandle) const noexcept;
    [[nodiscard]] ReservedRange reserved_regions() const noexcept;

    // Non-recursive pre-order depth-first walk capped at MAX_DEPTH.
    // `fn` may accept either `(Node)` or `(Node, std::span<const Node> ancestors)`
    // (root-to-parent order) and returns `WalkAction`. When `truncated` is
    // non-null, it is set to true if any subtree was cut off by MAX_DEPTH.
    template <typename Fn>
    void walk(Fn&& fn, bool* truncated = nullptr) const noexcept {
        auto thunk =
            [](Node node, std::span<const Node> ancestors, void* ctx) noexcept -> WalkAction {
            auto& visitor = *static_cast<std::remove_reference_t<Fn>*>(ctx);
            if constexpr(std::invocable<Fn, Node, std::span<const Node>>) {
                return visitor(node, ancestors);
            } else {
                (void)ancestors;
                return visitor(node);
            }
        };
        walk_raw(thunk, &fn, truncated);
    }

private:
    friend class Node;
    friend class ReservedRange::Iterator;

    using WalkCallback = WalkAction (*)(Node node,
                                        std::span<const Node> ancestors,
                                        void* ctx) noexcept;

    void walk_raw(WalkCallback callback, void* ctx, bool* truncated) const noexcept;

    [[nodiscard]] bool contains(uint32_t offset, size_t length) const noexcept;
    [[nodiscard]] bool read_u32(uint32_t offset, uint32_t& value) const noexcept;
    [[nodiscard]] bool skip_name(uint32_t node, uint32_t& next) const noexcept;
    [[nodiscard]] bool skip_property(uint32_t offset, uint32_t& next) const noexcept;
    [[nodiscard]] std::string_view string_at(uint32_t offset) const noexcept;

    [[nodiscard]] std::string_view node_name(uint32_t node) const noexcept;
    [[nodiscard]] uint32_t node_first_child(uint32_t node) const noexcept;
    [[nodiscard]] uint32_t node_next_sibling(uint32_t node) const noexcept;
    [[nodiscard]] uint32_t node_parent(uint32_t node) const noexcept;
    [[nodiscard]] Property node_property(uint32_t node, std::string_view name) const noexcept;

    const std::byte* m_base = nullptr;
    uint32_t m_total = 0;
    uint32_t m_off_struct = 0;
    uint32_t m_off_strings = 0;
    uint32_t m_size_strings = 0;
    uint32_t m_off_mem_rsvmap = 0;
    bool m_valid = false;
};

}  // namespace fdt
