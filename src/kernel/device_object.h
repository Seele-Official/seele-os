#pragma once

// A bound device object is distinct from the firmware's device::Device node.
// Probe returns an Object; subsystems borrow its interfaces for the rest of the
// kernel. There is no heap ownership, deletion path or dependency on C++ RTTI.

#include <type_traits>

namespace device {

using InterfaceId = const void*;

namespace detail {
// The address, not the byte's value, identifies an interface. Inline variables
// give each type one identity across translation units, with no registration
// table or static constructors. These ids are internal to the kernel image.
template <typename Interface>
constexpr inline char interface_key = 0;
}  // namespace detail

template <typename Interface>
constexpr inline InterfaceId interface_id = &detail::interface_key<std::remove_cv_t<Interface>>;

class Object {
public:
    [[nodiscard]] virtual void* query_interface(InterfaceId id) noexcept = 0;
    [[nodiscard]] virtual const void* query_interface(InterfaceId id) const noexcept = 0;

protected:
    // Objects live in static storage or the probe arena. Do not allow delete
    // through this interface: neither kind of storage belongs to the heap.
    ~Object() = default;
};

// Declare the interfaces an object exposes. Each conversion is a real C++ base
// conversion, so multiple inheritance and nonzero interface offsets are safe.
// Querying an interface not listed here returns nullptr; this is deliberately
// interface discovery, not a replacement for arbitrary C++ RTTI casts.
template <typename... Interfaces>
class Implements : public Object, public Interfaces... {
public:
    [[nodiscard]] void* query_interface(InterfaceId id) noexcept final {
        if(id == interface_id<Object>) {
            return static_cast<Object*>(this);
        }
        void* result = nullptr;
        (void)((id == interface_id<Interfaces> ? (result = static_cast<Interfaces*>(this), true)
                                               : false) ||
               ...);
        return result;
    }

    [[nodiscard]] const void* query_interface(InterfaceId id) const noexcept final {
        if(id == interface_id<Object>) {
            return static_cast<const Object*>(this);
        }
        const void* result = nullptr;
        (void)((id == interface_id<Interfaces>
                    ? (result = static_cast<const Interfaces*>(this), true)
                    : false) ||
               ...);
        return result;
    }

protected:
    ~Implements() = default;
};

template <typename Interface>
[[nodiscard]] Interface* dyn_cast(Object* object) noexcept {
    return object == nullptr
               ? nullptr
               : static_cast<Interface*>(object->query_interface(interface_id<Interface>));
}

template <typename Interface>
[[nodiscard]] const Interface* dyn_cast(const Object* object) noexcept {
    return object == nullptr
               ? nullptr
               : static_cast<const Interface*>(object->query_interface(interface_id<Interface>));
}

}  // namespace device
