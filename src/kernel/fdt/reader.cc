// See fdt/fdt.h. Parsed layout (Devicetree Specification v0.4, chapter 5):
//
//   header  (40 bytes for version 17)
//   memory reservation block
//   structure block:  u32 tokens; BEGIN_NODE + NUL name + 4-aligned
//                     PROP = u32 len, u32 nameoff, data (4-aligned)
//   strings block:    NUL-separated property names
//
// This file is the byte layer: tokens, bounds checks and the string table.
// Node/Property semantics live in tree.cc.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "fdt/fdt.h"

namespace fdt {
namespace {

constexpr uint32_t FDT_MAGIC = 0xd00dfeed;

// Mirrors struct fdt_header from the Devicetree Specification. Only the fields
// this reader needs are named; offsetof() turns them into byte offsets so the
// (possibly unaligned) blob is never reinterpreted as a header object.
struct header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

constexpr uint32_t HEADER_SIZE = 40;  // version 17
static_assert(sizeof(header) == HEADER_SIZE);
constexpr uint32_t MAX_TOTAL_SIZE = 16u << 20;

// The reservation block always ends with a terminator, so an offset that
// cannot hold at least one entry is treated as an absent block.
constexpr uint32_t RESERVED_ENTRY_SIZE = 2 * sizeof(uint64_t);

enum class Token : uint32_t {
    BEGIN_NODE = 1,
    END_NODE = 2,
    PROP = 3,
    NOP = 4,
    END = 9,
};

[[nodiscard]] constexpr uint32_t value_of(Token token) noexcept {
    return static_cast<uint32_t>(token);
}

constexpr uint32_t align4(uint32_t value) noexcept {
    return (value + 3u) & ~3u;
}

consteval bool check_load_be() noexcept {
    constexpr std::byte bytes[] = {std::byte{0x12},
                                   std::byte{0x34},
                                   std::byte{0x56},
                                   std::byte{0x78}};
    return load_be<uint32_t>(bytes) == 0x12345678u;
}

static_assert(check_load_be());

}  // namespace

Tree::Tree(const void* blob) noexcept : m_base(static_cast<const std::byte*>(blob)) {
    if(m_base == nullptr) {
        return;
    }

    // The caller hands us a firmware pointer, so the first 8 bytes may be read
    // before the size is known; everything else is checked against totalsize.
    const auto total = be32(m_base + offsetof(header, totalsize));
    if(be32(m_base + offsetof(header, magic)) != FDT_MAGIC || total < HEADER_SIZE ||
       total > MAX_TOTAL_SIZE) {
        return;
    }

    const auto version = be32(m_base + offsetof(header, version));
    const auto last_comp_version = be32(m_base + offsetof(header, last_comp_version));
    // Only the version 17 layout is parsed here. QEMU, U-Boot and OpenSBI all
    // emit v17; a v16 blob has a 36-byte header without the size fields.
    if(version != 17 || last_comp_version > 17) {
        return;
    }

    const auto off_struct = be32(m_base + offsetof(header, off_dt_struct));
    const auto off_strings = be32(m_base + offsetof(header, off_dt_strings));
    const auto size_struct = be32(m_base + offsetof(header, size_dt_struct));
    const auto size_strings = be32(m_base + offsetof(header, size_dt_strings));
    const auto off_rsvmap = be32(m_base + offsetof(header, off_mem_rsvmap));

    const auto struct_end = uint64_t{off_struct} + size_struct;
    const auto strings_end = uint64_t{off_strings} + size_strings;
    if(off_struct < HEADER_SIZE || off_strings < HEADER_SIZE || size_struct < 4 ||
       size_strings < 1 || struct_end > total || strings_end > total) {
        return;
    }

    m_total = total;
    m_off_struct = off_struct;
    m_off_strings = off_strings;
    m_size_strings = size_strings;

    // The reservation block is optional in practice: without a usable offset
    // (zero, inside the header, or past the blob) reserved() iterates nothing
    // rather than reading the header as if it were a reservation.
    if(off_rsvmap >= HEADER_SIZE && off_rsvmap <= total) {
        m_off_mem_rsvmap = off_rsvmap;
    }

    uint32_t token = 0;
    m_valid = read_u32(m_off_struct, token) && token == value_of(Token::BEGIN_NODE);
}

bool Tree::contains(uint32_t offset, size_t length) const noexcept {
    return offset <= m_total && length <= m_total - offset;
}

bool Tree::read_u32(uint32_t offset, uint32_t& value) const noexcept {
    if(m_base == nullptr || !contains(offset, sizeof(uint32_t))) {
        return false;
    }
    value = be32(m_base + offset);
    return true;
}

const char* Tree::string_at(uint32_t offset) const noexcept {
    if(offset >= m_size_strings) {
        return nullptr;
    }

    const auto* strings = m_base + m_off_strings;
    const uint32_t limit = m_size_strings - offset;
    for(uint32_t i = 0; i < limit; ++i) {
        if(strings[offset + i] == std::byte{0}) {
            return reinterpret_cast<const char*>(strings + offset);
        }
    }
    return nullptr;
}

// Validate a BEGIN_NODE token at `node` and find the offset just past its
// 4-aligned name.
bool Tree::skip_name(uint32_t node, uint32_t& next) const noexcept {
    if(!m_valid) {
        return false;
    }

    uint32_t token = 0;
    if(!read_u32(node, token) || token != value_of(Token::BEGIN_NODE)) {
        return false;
    }

    uint32_t p = node + 4;
    while(p < m_total && m_base[p] != std::byte{0}) {
        ++p;
    }
    if(p >= m_total) {
        return false;
    }

    next = align4(p + 1);
    return next <= m_total;
}

// Skip a PROP token and its 4-aligned data payload.
bool Tree::skip_property(uint32_t offset, uint32_t& next) const noexcept {
    uint32_t len = 0;
    if(!read_u32(offset + 4, len)) {
        return false;
    }

    const auto end = uint64_t{offset} + 12 + len;
    if(end > m_total) {
        return false;
    }

    next = align4(static_cast<uint32_t>(end));
    return next <= m_total;
}

const char* Tree::node_name(int node) const noexcept {
    if(node < 0) {
        return nullptr;
    }

    const auto offset = static_cast<uint32_t>(node);
    uint32_t next = 0;
    if(!skip_name(offset, next)) {
        return nullptr;
    }
    return reinterpret_cast<const char*>(m_base + offset + 4);
}

int Tree::node_first_child(int node) const noexcept {
    if(node < 0) {
        return -1;
    }

    uint32_t p = 0;
    if(!skip_name(static_cast<uint32_t>(node), p)) {
        return -1;
    }

    // A node may have properties before its first child.
    for(;;) {
        uint32_t token = 0;
        if(!read_u32(p, token)) {
            return -1;
        }

        switch(static_cast<Token>(token)) {
            case Token::BEGIN_NODE: return static_cast<int>(p);
            case Token::PROP: {
                uint32_t next = 0;
                if(!skip_property(p, next)) {
                    return -1;
                }
                p = next;
                continue;
            }
            case Token::NOP: p += 4; continue;
            default: return -1;  // END_NODE: leaf node
        }
    }
}

int Tree::node_next_sibling(int node) const noexcept {
    if(node < 0) {
        return -1;
    }

    uint32_t p = 0;
    if(!skip_name(static_cast<uint32_t>(node), p)) {
        return -1;
    }

    unsigned depth = 0;
    for(;;) {
        uint32_t token = 0;
        if(!read_u32(p, token)) {
            return -1;
        }

        switch(static_cast<Token>(token)) {
            case Token::BEGIN_NODE: {
                ++depth;
                uint32_t next = 0;
                if(!skip_name(p, next)) {
                    return -1;
                }
                p = next;
                continue;
            }
            case Token::END_NODE: {
                if(depth == 0) {
                    // The node's own end: the next token is a sibling or the
                    // parent's END_NODE.
                    const uint32_t q = p + 4;
                    uint32_t next = 0;
                    if(!read_u32(q, next) || next != value_of(Token::BEGIN_NODE)) {
                        return -1;
                    }
                    return static_cast<int>(q);
                }
                --depth;
                p += 4;
                continue;
            }
            case Token::PROP: {
                uint32_t next = 0;
                if(!skip_property(p, next)) {
                    return -1;
                }
                p = next;
                continue;
            }
            case Token::NOP: p += 4; continue;
            default: return -1;
        }
    }
}

// The structure block does not store parent links, so walk it from the root.
// The first pass finds the target's depth (root is depth 0); the second pass
// remembers the last node opened one level above it. Reading the token stream
// twice costs O(blob) but needs no stack, which matters in early boot.
int Tree::node_parent(int node) const noexcept {
    if(!m_valid || node < 0) {
        return -1;
    }

    const auto target = static_cast<uint32_t>(node);
    if(target == m_off_struct) {
        return -1;
    }

    uint32_t target_depth = 0;
    {
        uint32_t depth = 0;
        uint32_t p = m_off_struct;
        for(;;) {
            uint32_t token = 0;
            if(!read_u32(p, token)) {
                return -1;
            }

            if(token == value_of(Token::BEGIN_NODE)) {
                if(p == target) {
                    target_depth = depth;
                    break;
                }
                uint32_t next = 0;
                if(!skip_name(p, next)) {
                    return -1;
                }
                ++depth;
                p = next;
            } else if(token == value_of(Token::END_NODE)) {
                if(depth == 0) {
                    return -1;  // the tree ended before the target was seen
                }
                --depth;
                p += 4;
            } else if(token == value_of(Token::PROP)) {
                uint32_t next = 0;
                if(!skip_property(p, next)) {
                    return -1;
                }
                p = next;
            } else if(token == value_of(Token::NOP)) {
                p += 4;
            } else {
                return -1;
            }
        }
    }

    uint32_t candidate = 0;
    bool have_candidate = false;
    uint32_t depth = 0;
    uint32_t p = m_off_struct;
    for(;;) {
        uint32_t token = 0;
        if(!read_u32(p, token)) {
            return -1;
        }

        if(token == value_of(Token::BEGIN_NODE)) {
            if(p == target) {
                return have_candidate ? static_cast<int>(candidate) : -1;
            }
            if(target_depth >= 1 && depth == target_depth - 1) {
                candidate = p;
                have_candidate = true;
            }
            uint32_t next = 0;
            if(!skip_name(p, next)) {
                return -1;
            }
            ++depth;
            p = next;
        } else if(token == value_of(Token::END_NODE)) {
            if(depth == 0) {
                return -1;
            }
            --depth;
            p += 4;
        } else if(token == value_of(Token::PROP)) {
            uint32_t next = 0;
            if(!skip_property(p, next)) {
                return -1;
            }
            p = next;
        } else if(token == value_of(Token::NOP)) {
            p += 4;
        } else {
            return -1;
        }
    }
}

bool Tree::node_property(int node, std::string_view name, Property& out) const noexcept {
    out = Property{};
    if(node < 0) {
        return false;
    }

    uint32_t p = 0;
    if(!skip_name(static_cast<uint32_t>(node), p)) {
        return false;
    }

    for(;;) {
        uint32_t token = 0;
        if(!read_u32(p, token)) {
            return false;
        }

        switch(static_cast<Token>(token)) {
            case Token::PROP: {
                uint32_t len = 0;
                uint32_t name_off = 0;
                if(!read_u32(p + 4, len) || !read_u32(p + 8, name_off)) {
                    return false;
                }
                const char* pname = string_at(name_off);
                if(pname == nullptr) {
                    return false;
                }

                const auto data = uint64_t{p} + 12;
                if(data + len > m_total) {
                    return false;
                }
                if(std::string_view{pname} == name) {
                    out = Property{m_base + static_cast<uint32_t>(data), len};
                    return true;
                }
                p = align4(static_cast<uint32_t>(data + len));
                continue;
            }
            case Token::NOP: p += 4; continue;
            default: return false;  // END_NODE or malformed
        }
    }
}

}  // namespace fdt
