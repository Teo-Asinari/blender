#pragma once

// Shared value types extracted from Golemics core. Blender owns scene entities,
// image storage and serialization, so their standalone APIs are not imported.
#include <compare>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dcc {

enum class EntityKind : std::uint8_t { mesh, material, image, object };

template<EntityKind Kind> struct Id {
  std::uint64_t value{};
  auto operator<=>(const Id&) const = default;
  explicit operator bool() const noexcept { return value != 0; }
};
using MeshId = Id<EntityKind::mesh>;

struct Vec3 {
  float x{}, y{}, z{};
  auto operator<=>(const Vec3&) const = default;
};
struct Vertex {
  Vec3 position{};
  Vec3 normal{};
  auto operator<=>(const Vertex&) const = default;
};
// Mesh owns its CPU vertex/index storage. Indices describe triangles.
struct Mesh {
  MeshId id; std::string name; std::vector<Vertex> vertices; std::vector<std::uint32_t> indices;
  Mesh() = default;
  Mesh(MeshId value_id, std::string value_name,
       std::vector<Vertex> value_vertices = {}, std::vector<std::uint32_t> value_indices = {})
    : id(value_id), name(std::move(value_name)), vertices(std::move(value_vertices)),
      indices(std::move(value_indices)) {}
};
struct ImageRegion {
  std::uint32_t x{}, y{}, width{}, height{};
  auto operator<=>(const ImageRegion &) const = default;
};

} // namespace dcc
