// See fdt/fdt.h. This file is the query layer: typed properties, node
// navigation, cells inheritance, phandles and the two iterators. The token
// scanning and bounds checks it builds on live in reader.cc.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "fdt/fdt.h"

namespace fdt {
namespace {

constexpr uint32_t DEFAULT_ADDRESS_CELLS = 2;
constexpr uint32_t DEFAULT_SIZE_CELLS = 2;

[[nodiscard]] bool name_matches(const char* name, std::string_view component) noexcept {
    const std::string_view full{name};
    if(component.size() > full.size()) {
        return false;
    }
    // starts_with() rather than substr(0, n): substr() carries a range-check
    // path that pulls std::terminate() into Debug builds, where
    // _GLIBCXX_ASSERTIONS is on.
    if(!full.starts_with(component)) {
        return false;
    }
    // A path component may omit the unit address: "/memory" matches
    // "memory@80000000", exactly like libfdt's fdt_path_offset().
    return full.size() == component.size() || full[component.size()] == '@';
}

}  // namespace

std::optional<uint32_t> Property::u32() const noexcept {
    if(m_size != sizeof(uint32_t)) {
        return std::nullopt;
    }
    return be32(m_data);
}

std::optional<uint64_t> Property::u64() const noexcept {
    if(m_size != sizeof(uint64_t)) {
        return std::nullopt;
    }
    return be64(m_data);
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

const char* Node::name() const noexcept {
    if(!valid()) {
        return nullptr;
    }
    return m_tree->node_name(m_offset);
}

std::string_view Node::basename() const noexcept {
    const char* full = name();
    if(full == nullptr) {
        return {};
    }
    // The reader has already checked that the name is NUL-terminated inside
    // the blob, so the scan cannot run past the buffer.
    size_t length = 0;
    while(full[length] != '\0' && full[length] != '@') {
        ++length;
    }
    return {full, length};
}

Node Node::parent() const noexcept {
    if(!valid()) {
        return {};
    }
    return Node{m_tree, m_tree->node_parent(m_offset)};
}

Node Node::first_child() const noexcept {
    if(!valid()) {
        return {};
    }
    return Node{m_tree, m_tree->node_first_child(m_offset)};
}

Node Node::next_sibling() const noexcept {
    if(!valid()) {
        return {};
    }
    return Node{m_tree, m_tree->node_next_sibling(m_offset)};
}

Property Node::property(std::string_view name) const noexcept {
    Property out;
    if(valid()) {
        // A missing property is reported as an empty view, not as an error.
        (void)m_tree->node_property(m_offset, name, out);
    }
    return out;
}

bool Node::is_compatible(std::string_view id) const noexcept {
    const Property compatible = property("compatible");
    if(!compatible) {
        return false;
    }

    for(StringList list{compatible};;) {
        std::string_view candidate;
        if(!list.next(candidate)) {
            return false;
        }
        if(candidate == id) {
            return true;
        }
    }
}

bool Node::enabled() const noexcept {
    if(!valid()) {
        return false;
    }

    const Property status = property("status");
    if(!status) {
        return true;  // the spec's default is "okay"
    }

    const auto value = status.string();
    return value && (*value == "okay" || *value == "ok");
}

std::optional<uint32_t> Node::phandle() const noexcept {
    if(const auto value = property("phandle").u32()) {
        return value;
    }
    // Old firmware (and some device trees in the wild) spell it this way;
    // the modern name wins when both are present.
    return property("linux,phandle").u32();
}

bool RegIterator::next(Paddr& base, uint64_t& size) noexcept {
    // Only 32- and 64-bit addresses/sizes are understood. Refusing anything
    // else keeps a malformed cell count from silently misaligned the walk.
    if(m_cells.address < 1 || m_cells.address > 2 || m_cells.size < 1 || m_cells.size > 2) {
        return false;
    }

    constexpr size_t CELL_SIZE = sizeof(uint32_t);
    const auto bytes = m_reg.bytes();
    const size_t entry_bytes = (size_t{m_cells.address} + m_cells.size) * CELL_SIZE;

    while(m_offset + entry_bytes <= bytes.size()) {
        const auto* entry = bytes.data() + m_offset;
        m_offset += entry_bytes;

        uint64_t address = 0;
        for(uint32_t i = 0; i < m_cells.address; ++i) {
            address = (address << 32) | be32(entry + i * CELL_SIZE);
        }

        uint64_t length = 0;
        for(uint32_t i = 0; i < m_cells.size; ++i) {
            length = (length << 32) | be32(entry + (m_cells.address + i) * CELL_SIZE);
        }

        if(length == 0) {
            // A zero-length entry describes no address range: QEMU's
            // /cpus/cpu@0 has #size-cells = 0, so this is what keeps it from
            // being mistaken for a device.
            continue;
        }

        base = paddr(address);
        size = length;
        return true;
    }
    return false;
}

bool ReservedIterator::next(Paddr& base, uint64_t& size) noexcept {
    if(m_tree == nullptr || m_offset == 0) {
        return false;
    }

    constexpr size_t ENTRY_SIZE = 2 * sizeof(uint64_t);
    while(m_tree->contains(m_offset, ENTRY_SIZE)) {
        const auto* entry = m_tree->m_base + m_offset;
        m_offset += ENTRY_SIZE;

        const uint64_t address = be64(entry);
        const uint64_t length = be64(entry + sizeof(uint64_t));
        if(address == 0 && length == 0) {
            return false;  // terminator
        }

        base = paddr(address);
        size = length;
        return true;
    }
    return false;
}

bool StringList::next(std::string_view& value) noexcept {
    while(m_offset < m_bytes.size()) {
        size_t end = m_offset;
        while(end < m_bytes.size() && m_bytes[end] != std::byte{0}) {
            ++end;
        }
        if(end == m_bytes.size()) {
            return false;  // unterminated string list
        }

        if(end != m_offset) {
            value = std::string_view{reinterpret_cast<const char*>(m_bytes.data() + m_offset),
                                     end - m_offset};
            m_offset = end + 1;
            return true;
        }
        ++m_offset;  // empty string, keep looking
    }
    return false;
}

Node Tree::root() const noexcept {
    if(!m_valid) {
        return {};
    }
    return Node{this, static_cast<int>(m_off_struct)};
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
        // Sliced by hand, not with substr(): the range check would drag an
        // exception path into Debug builds.
        const std::string_view component{path.data() + start, i - start};

        Node match;
        for(Node child = node.first_child(); child; child = child.next_sibling()) {
            const char* child_name = child.name();
            if(child_name != nullptr && name_matches(child_name, component)) {
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

Node Tree::by_phandle(uint32_t phandle) const noexcept {
    if(!m_valid) {
        return {};
    }

    // A depth-first walk over a boot-time tree (a few dozen nodes) is cheaper
    // than building an index. Node::parent() re-walks the blob, but it is
    // called about once per node, not per property.
    Node node = root();
    while(node) {
        if(const auto value = node.phandle(); value && *value == phandle) {
            return node;
        }

        if(Node child = node.first_child()) {
            node = child;
            continue;
        }
        while(node) {
            if(Node sibling = node.next_sibling()) {
                node = sibling;
                break;
            }
            node = node.parent();
        }
    }
    return {};
}

cell_counts Tree::cells(Node node) const noexcept {
    cell_counts counts{DEFAULT_ADDRESS_CELLS, DEFAULT_SIZE_CELLS};
    bool have_address = false;
    bool have_size = false;

    // The parent chain, not the node: a bus's cell counts describe the `reg`
    // of its children. root has no parent, so it falls back to 2/2.
    for(Node current = node.parent(); current; current = current.parent()) {
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

RegIterator Tree::reg(Node node) const noexcept {
    if(!node) {
        return {};
    }
    return RegIterator{node.property("reg"), cells(node)};
}

ReservedIterator Tree::reserved() const noexcept {
    if(!m_valid || m_off_mem_rsvmap == 0) {
        return {};
    }
    return ReservedIterator{this, m_off_mem_rsvmap};
}

}  // namespace fdt
