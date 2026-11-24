// Flattened device tree (DTB v17) reader and query layer (fdt/fdt.h).

#include "fdt/fdt.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fdt {
namespace {

constexpr uint32_t FDT_MAGIC = 0xd00dfeed;
constexpr uint32_t HEADER_SIZE = 40;
constexpr uint32_t MAX_TOTAL_SIZE = 16u << 20;
constexpr uint32_t DEFAULT_ADDRESS_CELLS = 2;
constexpr uint32_t DEFAULT_SIZE_CELLS = 2;

// Mirrors struct fdt_header from the Devicetree Specification. Used only with
// offsetof() so a possibly unaligned blob is never reinterpreted as a struct.
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

static_assert(sizeof(header) == HEADER_SIZE);

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

[[nodiscard]] constexpr uint32_t align4(uint32_t value) noexcept {
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

[[nodiscard]] bool name_matches(std::string_view full, std::string_view component) noexcept {
    if(component.size() > full.size() || !full.starts_with(component)) {
        return false;
    }
    return full.size() == component.size() || full[component.size()] == '@';
}

[[nodiscard]] uint64_t read_cells(const std::byte* data, uint32_t count) noexcept {
    uint64_t value = 0;
    for(uint32_t i = 0; i < count; ++i) {
        value = (value << 32) | be32(data + i * sizeof(uint32_t));
    }
    return value;
}

}  // namespace

// -----------------------------------------------------------------------------
// Property & Ranges
// -----------------------------------------------------------------------------

void StringList::Iterator::advance() noexcept {
    while(m_offset < m_bytes.size()) {
        size_t end = m_offset;
        while(end < m_bytes.size() && m_bytes[end] != std::byte{0}) {
            ++end;
        }
        if(end == m_bytes.size()) {
            break;
        }
        const size_t start = m_offset;
        m_offset = end + 1;
        if(end != start) {
            m_current = std::string_view{reinterpret_cast<const char*>(m_bytes.data() + start),
                                         end - start};
            return;
        }
    }
    m_current = {};
    m_done = true;
}

std::optional<uint32_t> Property::u32() const noexcept {
    return m_size == sizeof(uint32_t) ? std::optional<uint32_t>{be32(m_data)} : std::nullopt;
}

std::optional<uint64_t> Property::u64() const noexcept {
    return m_size == sizeof(uint64_t) ? std::optional<uint64_t>{be64(m_data)} : std::nullopt;
}

std::optional<uint64_t> Property::u32_or_u64() const noexcept {
    if(const auto value = u64()) {
        return value;
    }
    if(const auto value = u32()) {
        return uint64_t{*value};
    }
    return std::nullopt;
}

std::optional<std::string_view> Property::string() const noexcept {
    if(m_data == nullptr) {
        return std::nullopt;
    }
    for(size_t i = 0; i < m_size; ++i) {
        if(m_data[i] == std::byte{0}) {
            return std::string_view{reinterpret_cast<const char*>(m_data), i};
        }
    }
    return std::nullopt;
}

void RegRange::Iterator::advance() noexcept {
    if(m_cells.address < 1 || m_cells.address > 2 || m_cells.size < 1 || m_cells.size > 2) {
        m_done = true;
        return;
    }

    const auto bytes = m_reg.bytes();
    const size_t entry_bytes = (size_t{m_cells.address} + m_cells.size) * sizeof(uint32_t);

    while(m_offset + entry_bytes <= bytes.size()) {
        const auto* entry = bytes.data() + m_offset;
        m_offset += entry_bytes;

        const uint64_t address = read_cells(entry, m_cells.address);
        const uint64_t length =
            read_cells(entry + m_cells.address * sizeof(uint32_t), m_cells.size);
        if(length != 0) {
            m_current = phys_range{paddr(address), length};
            return;
        }
    }
    m_done = true;
}

void ReservedRange::Iterator::advance() noexcept {
    if(m_tree == nullptr || m_offset == 0) {
        m_done = true;
        return;
    }

    constexpr size_t ENTRY_SIZE = 2 * sizeof(uint64_t);
    if(m_tree->contains(m_offset, ENTRY_SIZE)) {
        const auto* entry = m_tree->m_base + m_offset;
        m_offset += ENTRY_SIZE;

        const uint64_t address = be64(entry);
        const uint64_t length = be64(entry + sizeof(uint64_t));
        if(address != 0 || length != 0) {
            m_current = phys_range{paddr(address), length};
            return;
        }
    }
    m_done = true;
}

// -----------------------------------------------------------------------------
// Node
// -----------------------------------------------------------------------------

std::string_view Node::name() const noexcept {
    return valid() ? m_tree->node_name(m_offset) : std::string_view{};
}

std::string_view Node::basename() const noexcept {
    const std::string_view full = name();
    const size_t at = full.find('@');
    return at == std::string_view::npos ? full : std::string_view{full.data(), at};
}

Node Node::parent() const noexcept {
    return valid() ? Node{m_tree, m_tree->node_parent(m_offset)} : Node{};
}

Node Node::first_child() const noexcept {
    return valid() ? Node{m_tree, m_tree->node_first_child(m_offset)} : Node{};
}

Node Node::next_sibling() const noexcept {
    return valid() ? Node{m_tree, m_tree->node_next_sibling(m_offset)} : Node{};
}

Property Node::property(std::string_view name) const noexcept {
    return valid() ? m_tree->node_property(m_offset, name) : Property{};
}

bool Node::is_compatible(std::string_view id) const noexcept {
    for(const std::string_view candidate: property("compatible").strings()) {
        if(candidate == id) {
            return true;
        }
    }
    return false;
}

bool Node::enabled() const noexcept {
    if(!valid()) {
        return false;
    }
    const Property status = property("status");
    if(!status) {
        return true;
    }
    const auto value = status.string();
    return value && (*value == "okay" || *value == "ok");
}

std::optional<uint32_t> Node::phandle() const noexcept {
    if(const auto value = property("phandle").u32()) {
        return value;
    }
    return property("linux,phandle").u32();
}

cell_counts Node::cells(std::span<const Node> ancestors) noexcept {
    cell_counts counts{DEFAULT_ADDRESS_CELLS, DEFAULT_SIZE_CELLS};
    bool have_address = false;
    bool have_size = false;

    for(size_t i = ancestors.size(); i > 0; --i) {
        const Node current = ancestors[i - 1];
        if(!have_address) {
            if(const auto value = current.property("#address-cells").u32()) {
                counts.address = *value;
                have_address = true;
            }
        }
        if(!have_size) {
            if(const auto value = current.property("#size-cells").u32()) {
                counts.size = *value;
                have_size = true;
            }
        }
        if(have_address && have_size) {
            break;
        }
    }
    return counts;
}

cell_counts Node::cells() const noexcept {
    if(!valid()) {
        return cell_counts{DEFAULT_ADDRESS_CELLS, DEFAULT_SIZE_CELLS};
    }
    cell_counts counts{DEFAULT_ADDRESS_CELLS, DEFAULT_SIZE_CELLS};
    m_tree->walk([&](Node current, std::span<const Node> ancestors) noexcept {
        if(current.m_offset == m_offset) {
            counts = cells(ancestors);
            return WalkAction::Stop;
        }
        return WalkAction::Continue;
    });
    return counts;
}

RegRange Node::regs() const noexcept {
    return valid() ? RegRange{property("reg"), cells()} : RegRange{};
}

RegRange Node::regs(std::span<const Node> ancestors) const noexcept {
    return valid() ? RegRange{property("reg"), cells(ancestors)} : RegRange{};
}

std::optional<uint64_t> Node::reg_address() const noexcept {
    if(!valid()) {
        return std::nullopt;
    }
    const Property reg = property("reg");
    const cell_counts counts = cells();
    if(!reg || counts.address < 1 || counts.address > 2) {
        return std::nullopt;
    }
    const auto bytes = reg.bytes();
    if(bytes.size() < size_t{counts.address} * sizeof(uint32_t)) {
        return std::nullopt;
    }
    return read_cells(bytes.data(), counts.address);
}

// -----------------------------------------------------------------------------
// Tree
// -----------------------------------------------------------------------------

Tree::Tree(const void* blob) noexcept : m_base(static_cast<const std::byte*>(blob)) {
    if(m_base == nullptr) {
        return;
    }

    const auto total = be32(m_base + offsetof(header, totalsize));
    if(be32(m_base + offsetof(header, magic)) != FDT_MAGIC || total < HEADER_SIZE ||
       total > MAX_TOTAL_SIZE) {
        return;
    }

    const auto version = be32(m_base + offsetof(header, version));
    const auto last_comp_version = be32(m_base + offsetof(header, last_comp_version));
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

std::string_view Tree::string_at(uint32_t offset) const noexcept {
    if(offset >= m_size_strings) {
        return {};
    }
    const auto* strings = m_base + m_off_strings;
    const uint32_t limit = m_size_strings - offset;
    for(uint32_t i = 0; i < limit; ++i) {
        if(strings[offset + i] == std::byte{0}) {
            return std::string_view{reinterpret_cast<const char*>(strings + offset), i};
        }
    }
    return {};
}

bool Tree::skip_name(uint32_t node, uint32_t& next) const noexcept {
    uint32_t token = 0;
    if(!m_valid || !read_u32(node, token) || token != value_of(Token::BEGIN_NODE)) {
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

std::string_view Tree::node_name(uint32_t node) const noexcept {
    uint32_t next = 0;
    if(node == 0 || !skip_name(node, next)) {
        return {};
    }
    const auto* start = reinterpret_cast<const char*>(m_base + node + 4);
    size_t length = 0;
    while(start[length] != '\0') {
        ++length;
    }
    return std::string_view{start, length};
}

uint32_t Tree::node_first_child(uint32_t node) const noexcept {
    uint32_t p = 0;
    if(node == 0 || !skip_name(node, p)) {
        return 0;
    }
    for(uint32_t token = 0; read_u32(p, token);) {
        switch(static_cast<Token>(token)) {
            case Token::BEGIN_NODE: return p;
            case Token::PROP:
                if(!skip_property(p, p)) {
                    return 0;
                }
                continue;
            case Token::NOP: p += 4; continue;
            default: return 0;
        }
    }
    return 0;
}

uint32_t Tree::node_next_sibling(uint32_t node) const noexcept {
    uint32_t p = 0;
    if(node == 0 || !skip_name(node, p)) {
        return 0;
    }
    unsigned depth = 0;
    for(uint32_t token = 0; read_u32(p, token);) {
        switch(static_cast<Token>(token)) {
            case Token::BEGIN_NODE:
                ++depth;
                if(!skip_name(p, p)) {
                    return 0;
                }
                continue;
            case Token::END_NODE:
                p += 4;
                if(depth == 0) {
                    while(read_u32(p, token) && token == value_of(Token::NOP)) {
                        p += 4;
                    }
                    return read_u32(p, token) && token == value_of(Token::BEGIN_NODE) ? p : 0;
                }
                --depth;
                continue;
            case Token::PROP:
                if(!skip_property(p, p)) {
                    return 0;
                }
                continue;
            case Token::NOP: p += 4; continue;
            default: return 0;
        }
    }
    return 0;
}

uint32_t Tree::node_parent(uint32_t node) const noexcept {
    if(!m_valid || node == 0 || node == m_off_struct) {
        return 0;
    }
    uint32_t found = 0;
    walk([&](Node current, std::span<const Node> ancestors) noexcept {
        if(current.offset() == node) {
            if(!ancestors.empty()) {
                found = ancestors.back().offset();
            }
            return WalkAction::Stop;
        }
        return WalkAction::Continue;
    });
    return found;
}

Property Tree::node_property(uint32_t node, std::string_view name) const noexcept {
    uint32_t p = 0;
    if(node == 0 || !skip_name(node, p)) {
        return {};
    }
    for(uint32_t token = 0; read_u32(p, token);) {
        switch(static_cast<Token>(token)) {
            case Token::PROP: {
                uint32_t len = 0;
                uint32_t name_off = 0;
                if(!read_u32(p + 4, len) || !read_u32(p + 8, name_off)) {
                    return {};
                }
                const std::string_view pname = string_at(name_off);
                const auto data = uint64_t{p} + 12;
                if(pname.data() == nullptr || data + len > m_total) {
                    return {};
                }
                if(pname == name) {
                    return Property{m_base + static_cast<uint32_t>(data), len};
                }
                p = align4(static_cast<uint32_t>(data + len));
                continue;
            }
            case Token::NOP: p += 4; continue;
            default: return {};
        }
    }
    return {};
}

Node Tree::root() const noexcept {
    return m_valid ? Node{this, m_off_struct} : Node{};
}

Node Tree::node_at(uint32_t offset) const noexcept {
    uint32_t next = 0;
    if(!m_valid || offset == 0 || !skip_name(offset, next)) {
        return {};
    }
    return Node{this, offset};
}

Node Tree::find(std::string_view path) const noexcept {
    if(!m_valid || path.empty() || path.front() != '/') {
        return {};
    }

    Node node = root();
    size_t i = 0;
    while(i < path.size() && path[i] == '/') {
        ++i;
    }
    while(i < path.size()) {
        const size_t start = i;
        while(i < path.size() && path[i] != '/') {
            ++i;
        }
        const std::string_view component{path.data() + start, i - start};

        Node match;
        for(const Node child: node.children()) {
            const std::string_view child_name = child.name();
            if(!child_name.empty() && name_matches(child_name, component)) {
                match = child;
                break;
            }
        }
        if(!match) {
            return {};
        }
        node = match;

        while(i < path.size() && path[i] == '/') {
            ++i;
        }
    }
    return node;
}

Node Tree::by_phandle(uint32_t target) const noexcept {
    Node found;
    walk([&](Node node) noexcept {
        if(const auto value = node.phandle(); value && *value == target) {
            found = node;
            return WalkAction::Stop;
        }
        return WalkAction::Continue;
    });
    return found;
}

ReservedRange Tree::reserved_regions() const noexcept {
    return m_valid && m_off_mem_rsvmap != 0 ? ReservedRange{this, m_off_mem_rsvmap}
                                            : ReservedRange{};
}

void Tree::walk_raw(WalkCallback callback, void* ctx, bool* truncated) const noexcept {
    if(!m_valid || callback == nullptr) {
        return;
    }

    Node ancestors[MAX_DEPTH + 1]{};
    uint32_t depth = 0;
    uint32_t skip_depth = 0;
    uint32_t p = m_off_struct;

    for(uint32_t token = 0; read_u32(p, token);) {
        switch(static_cast<Token>(token)) {
            case Token::BEGIN_NODE: {
                const uint32_t node_offset = p;
                if(!skip_name(node_offset, p)) {
                    return;
                }
                if(skip_depth > 0) {
                    ++skip_depth;
                    continue;
                }
                if(depth > MAX_DEPTH) {
                    if(truncated != nullptr) {
                        *truncated = true;
                    }
                    skip_depth = 1;
                    continue;
                }
                const Node current{this, node_offset};
                switch(callback(current, std::span<const Node>{ancestors, depth}, ctx)) {
                    case WalkAction::Continue: ancestors[depth++] = current; break;
                    case WalkAction::SkipChildren: skip_depth = 1; break;
                    case WalkAction::Stop: return;
                }
                continue;
            }
            case Token::END_NODE:
                p += 4;
                if(skip_depth > 0) {
                    --skip_depth;
                    continue;
                }
                if(depth == 0 || --depth == 0) {
                    return;
                }
                continue;
            case Token::PROP:
                if(!skip_property(p, p)) {
                    return;
                }
                continue;
            case Token::NOP: p += 4; continue;
            default: return;
        }
    }
}

}  // namespace fdt
