#pragma once

// Read-only flattened device tree (DTB) reader.
//
// start() hands the firmware device tree pointer to kmain in a1. This reader
// exposes the tree as typed, bounds-checked views: no allocation, no I/O, no
// modification. Every access is checked against the blob's totalsize, so a
// truncated blob cannot walk off the buffer (the DTB itself is still assumed
// to come from trusted firmware).
//
// This layer only answers "what bytes are in the tree". It knows nothing about
// devices, drivers or resources; that belongs to the layers above.

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "mm/addr.h"

namespace fdt {

// Read a big-endian cell without requiring the source pointer to be aligned.
//
// DTB cells are big-endian while RISC-V is little-endian. Copying the bytes in
// reverse on a little-endian host lets std::bit_cast assemble the numeric
// value; the compiler folds this back into ordinary loads and shifts. Note
// that std::byteswap is not used: on rv64gc (no Zbb) it lowers to libgcc's
// __bswapsi2/__bswapdi2, and this kernel deliberately does not link libgcc.
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

// A non-owning view of one property value inside the blob. A default
// constructed Property means "not present"; a present but empty property (the
// encoding of a boolean flag) is a valid view whose size() is 0.
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

    // Typed accessors. Each rejects a value whose length does not match the
    // type exactly, so a malformed property reads as "absent" instead of as a
    // silently truncated number.
    [[nodiscard]] std::optional<uint32_t> u32() const noexcept;
    [[nodiscard]] std::optional<uint64_t> u64() const noexcept;

    // The first NUL-terminated string of the property, without the NUL. The
    // rest of a string-list property is reached through StringList.
    [[nodiscard]] std::optional<std::string_view> string() const noexcept;

    // A boolean property is defined by its presence; its length carries no
    // information.
    [[nodiscard]] constexpr bool boolean() const noexcept {
        return m_data != nullptr;
    }

private:
    const std::byte* m_data = nullptr;
    size_t m_size = 0;
};

// A handle to one node: the owning tree plus a byte offset into the structure
// block. Copying a Node is two machine words. Invalid handles are encoded as a
// null tree, so every accessor is safe on a default-constructed Node.
class Node {
public:
    constexpr Node() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return m_tree != nullptr && m_offset >= 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }

    // Two handles are equal when they name the same node of the same tree.
    // Every invalid handle is equal to every other invalid handle.
    [[nodiscard]] friend constexpr bool operator== (const Node& lhs, const Node& rhs) noexcept {
        if(!lhs.valid() || !rhs.valid()) {
            return lhs.valid() == rhs.valid();
        }
        return lhs.m_tree == rhs.m_tree && lhs.m_offset == rhs.m_offset;
    }

    // NUL-terminated name inside the blob, e.g. "uart@10000000".
    [[nodiscard]] const char* name() const noexcept;

    // The name without the unit address, e.g. "uart".
    [[nodiscard]] std::string_view basename() const noexcept;

    [[nodiscard]] Node parent() const noexcept;
    [[nodiscard]] Node first_child() const noexcept;
    [[nodiscard]] Node next_sibling() const noexcept;

    [[nodiscard]] Property property(std::string_view name) const noexcept;
    [[nodiscard]] bool is_compatible(std::string_view id) const noexcept;

    // Status defaults to "okay"; anything but "okay"/"ok" makes the node
    // unavailable, matching Linux of_device_is_available().
    [[nodiscard]] bool enabled() const noexcept;

    // `phandle`, falling back to the deprecated `linux,phandle` spelling.
    [[nodiscard]] std::optional<uint32_t> phandle() const noexcept;

private:
    friend class Tree;

    constexpr Node(const Tree* tree, int offset) noexcept : m_tree(tree), m_offset(offset) {}

    const Tree* m_tree = nullptr;
    int m_offset = -1;
};

// `#address-cells` / `#size-cells` that apply to a node's own `reg` property.
// The numbers are inherited from the parent bus, never from the node itself.
struct cell_counts {
    uint32_t address = 2;
    uint32_t size = 2;

    [[nodiscard]] friend constexpr bool operator== (const cell_counts&,
                                                    const cell_counts&) noexcept = default;
};

// Walks every (base, size) pair of a `reg` property. Entries whose size is
// zero are skipped: they describe no address range (QEMU's /cpus/cpu@0 has
// #size-cells = 0, so its `reg` is a lone address with no size).
class RegIterator {
public:
    constexpr RegIterator() noexcept = default;

    [[nodiscard]] bool next(Paddr& base, uint64_t& size) noexcept;

private:
    friend class Tree;

    constexpr RegIterator(Property reg, cell_counts cells) noexcept : m_reg(reg), m_cells(cells) {}

    Property m_reg{};
    cell_counts m_cells{};
    size_t m_offset = 0;
};

// Walks the memory reservation block (`/memreserve/` entries). Each entry is a
// (address, size) pair; the block ends at an all-zero pair.
class ReservedIterator {
public:
    constexpr ReservedIterator() noexcept = default;

    [[nodiscard]] bool next(Paddr& base, uint64_t& size) noexcept;

private:
    friend class Tree;

    constexpr ReservedIterator(const Tree* tree, uint32_t offset) noexcept :
        m_tree(tree), m_offset(offset) {}

    const Tree* m_tree = nullptr;
    uint32_t m_offset = 0;
};

// Walks the NUL-separated strings of a property such as `compatible` or
// `clock-names`. Empty strings are skipped. Property::string() returns only
// the first entry, which is why this is a separate type.
class StringList {
public:
    constexpr StringList() noexcept = default;

    constexpr explicit StringList(Property property) noexcept : m_bytes(property.bytes()) {}

    [[nodiscard]] bool next(std::string_view& value) noexcept;

private:
    std::span<const std::byte> m_bytes{};
    size_t m_offset = 0;
};

class Tree {
public:
    // An invalid tree. Lets the owner place the tree in static storage and fill
    // it in later, without a function-local static guard.
    constexpr Tree() noexcept = default;

    // Validates the header up front; an invalid blob leaves the tree invalid
    // but still safe to query.
    explicit Tree(const void* blob) noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return m_valid;
    }

    // Total size of the blob in bytes, as declared by its header.
    [[nodiscard]] uint32_t size() const noexcept {
        return m_total;
    }

    [[nodiscard]] Node root() const noexcept;
    [[nodiscard]] Node find(std::string_view path) const noexcept;
    [[nodiscard]] Node by_phandle(uint32_t phandle) const noexcept;

    // The counts that describe `node`'s own `reg`. The walk starts at the
    // parent: a bus node's #address-cells/#size-cells describe its children's
    // `reg` entries, not its own. root defaults to 2/2.
    [[nodiscard]] cell_counts cells(Node node) const noexcept;

    [[nodiscard]] RegIterator reg(Node node) const noexcept;
    [[nodiscard]] ReservedIterator reserved() const noexcept;

private:
    friend class Node;
    friend class ReservedIterator;

    [[nodiscard]] bool contains(uint32_t offset, size_t length) const noexcept;
    [[nodiscard]] bool read_u32(uint32_t offset, uint32_t& value) const noexcept;
    [[nodiscard]] bool skip_name(uint32_t node, uint32_t& next) const noexcept;
    [[nodiscard]] bool skip_property(uint32_t offset, uint32_t& next) const noexcept;
    [[nodiscard]] const char* string_at(uint32_t offset) const noexcept;

    // Byte layer used by Node. Offsets are structure-block offsets and every
    // accessor returns -1 when the token walk fails.
    [[nodiscard]] const char* node_name(int node) const noexcept;
    [[nodiscard]] int node_first_child(int node) const noexcept;
    [[nodiscard]] int node_next_sibling(int node) const noexcept;
    [[nodiscard]] int node_parent(int node) const noexcept;
    [[nodiscard]] bool node_property(int node, std::string_view name, Property& out) const noexcept;

    const std::byte* m_base = nullptr;
    uint32_t m_total = 0;
    uint32_t m_off_struct = 0;
    uint32_t m_off_strings = 0;
    uint32_t m_size_strings = 0;
    uint32_t m_off_mem_rsvmap = 0;
    bool m_valid = false;
};

}  // namespace fdt
