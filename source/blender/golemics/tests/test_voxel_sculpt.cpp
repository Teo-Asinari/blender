#include "dcc/voxel_sculpt.hpp"

#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <map>
#include <span>
#include <tuple>
#include <utility>

namespace {
using namespace dcc;
static Mesh cube_fixture(MeshId id,std::string name = "Cube"){
  Mesh mesh{id,std::move(name)};
  constexpr float h=0.5F;
  const auto face=[&](Vec3 n,Vec3 a,Vec3 b,Vec3 c,Vec3 d){const auto base=static_cast<std::uint32_t>(mesh.vertices.size());mesh.vertices.insert(mesh.vertices.end(),{{a,n},{b,n},{c,n},{d,n}});mesh.indices.insert(mesh.indices.end(),{base,base+1,base+2,base,base+2,base+3});};
  face({0,0,1},{-h,-h,h},{h,-h,h},{h,h,h},{-h,h,h}); face({0,0,-1},{h,-h,-h},{-h,-h,-h},{-h,h,-h},{h,h,-h});
  face({1,0,0},{h,-h,h},{h,-h,-h},{h,h,-h},{h,h,h}); face({-1,0,0},{-h,-h,-h},{-h,-h,h},{-h,h,h},{-h,h,-h});
  face({0,1,0},{-h,h,h},{h,h,h},{h,h,-h},{-h,h,-h}); face({0,-1,0},{-h,-h,-h},{h,-h,-h},{h,-h,h},{-h,-h,h});
  return mesh;
}
} // namespace

int main() {
  using namespace dcc;
  const auto require = [](bool condition) {
    if (!condition) throw std::runtime_error("voxel sculpt test failed");
  };
  SparseVoxelVolume volume(0.25F);
  volume.set_density({0, 0, 0}, 1.0F);
  require(volume.active_voxel_count() == 1);
  auto surface = volume.extract_surface(MeshId{9});
  require(!surface.indices.empty());
  // Closed interpolated surface: each geometric edge has two incident triangles.
  using Point = std::tuple<int,int,int>;
  std::map<std::pair<Point,Point>,int> edges;
  const auto quantize=[](Vec3 p) { return Point{static_cast<int>(std::lround(p.x*100000)),static_cast<int>(std::lround(p.y*100000)),static_cast<int>(std::lround(p.z*100000))}; };
  for(std::size_t i=0;i<surface.indices.size();i+=3) {
    for(std::size_t j=0;j<3;++j) {
      const auto& vertex=surface.vertices[surface.indices[i+j]];
      require(std::isfinite(vertex.normal.x));
      const Vec3 offset{vertex.position.x-0.125F,vertex.position.y-0.125F,vertex.position.z-0.125F};
      require(offset.x*vertex.normal.x+offset.y*vertex.normal.y+offset.z*vertex.normal.z>0.0F);
      auto a=quantize(vertex.position),b=quantize(surface.vertices[surface.indices[i+(j+1)%3]].position);
      if(b<a) std::swap(a,b);
      ++edges[{a,b}];
    }
  }
  for(const auto& [edge,count]:edges) { (void)edge; require(count==2); }
  const auto tighter=volume.extract_surface(MeshId{9},"tight",0.75F);
  float extent=0.0F;
  for(const auto& v:tighter.vertices) extent=std::max(extent,std::abs(v.position.x-0.125F));
  require(std::abs(extent-0.0625F)<0.00001F);

  SparseVoxelVolume bricks;
  for(int z=-8;z<0;++z) for(int y=-8;y<0;++y) for(int x=-8;x<0;++x) bricks.set_density({x,y,z},0.7F);
  require(bricks.active_voxel_count()==512 && bricks.active_brick_count()==1);
  bricks.set_density({-9,-1,-1},0.4F);
  require(bricks.active_brick_count()==2 && bricks.density({-9,-1,-1})==0.4F);
  bricks.set_density({-9,-1,-1},0.0F);
  require(bricks.active_brick_count()==1);
  const auto snapshot=bricks.cells();
  require(snapshot.size()==512 && snapshot.at({-8,-8,-8})==0.7F);
  bricks.boolean_subtract(bricks);
  require(bricks.active_voxel_count()==0 && bricks.active_brick_count()==0);

  SparseVoxelVolume rhs(0.25F);
  rhs.set_density({1, 0, 0}, 0.75F);
  volume.boolean_union(rhs);
  require(volume.density({1, 0, 0}) == 0.75F);
  volume.boolean_subtract(rhs);
  require(volume.density({1, 0, 0}) == 0.0F);

  SparseVoxelVolume intersected(0.25F);
  intersected.set_density({0,0,0},1.0F);
  intersected.set_density({1,0,0},1.0F);
  intersected.boolean_intersect(rhs);
  require(intersected.active_voxel_count()==1 && intersected.density({1,0,0})==0.75F);
  const auto repeat=volume.extract_surface(MeshId{9});
  require(repeat.indices==surface.indices && repeat.vertices.size()==surface.vertices.size());
  for(std::size_t i=0;i<surface.vertices.size();++i) require(quantize(repeat.vertices[i].position)==quantize(surface.vertices[i].position));

  const auto cube = cube_fixture(MeshId{1});
  const auto voxelized = SparseVoxelVolume::voxelize(cube, 0.2F);
  require(voxelized.active_voxel_count() > 8);
  require(voxelized.density({0,0,0})==1.0F);
  require(voxelized.density({50,0,0})==0.0F);
  auto cut=voxelized;
  SculptBrush carve;
  carve.mode=SculptBrushMode::subtract; carve.radius=0.3F; carve.strength=1.0F; carve.falloff=SculptFalloff::constant;
  const std::array<SculptSample,1> interior_dab{{{{0.1F,0.1F,0.1F},1.0F}}};
  const auto carved=apply_sculpt_stroke(cut,interior_dab,carve);
  require(!carved.changes.empty() && cut.density({0,0,0})==0.0F);
  Mesh open_triangle{MeshId{20},"open",{{{0,0,0},{}},{{1,0,0},{}},{{0,1,0},{}}},{0,1,2}};
  const auto shell=SparseVoxelVolume::voxelize(open_triangle,0.2F);
  require(shell.active_voxel_count()>0 && shell.density({0,0,-1})==0.0F);
  bool oversized=false;
  try { (void)SparseVoxelVolume::voxelize(cube,0.00001F); } catch(const std::invalid_argument&) { oversized=true; }
  require(oversized);

  const std::array<SculptSample, 2> line{{{{0, 0, 0}, 1}, {{1, 0, 0}, 0.5F}}};
  const auto sampled = sample_sculpt_stroke(line, 0.2F);
  require(sampled.size() >= 6);

  SparseVoxelVolume sculpt(0.1F);
  SculptBrush add;
  add.radius = 0.3F;
  add.strength = 1.0F;
  add.symmetry_x = true;
  const std::array<SculptSample, 1> dab{{{{0.4F, 0, 0}, 1}}};
  const auto tx = apply_sculpt_stroke(sculpt, dab, add);
  require(!tx.changes.empty());
  require(sculpt.density({4, 0, 0}) > 0.0F);
  require(sculpt.density({-4, 0, 0}) > 0.0F);
  SculptBrush xyz = add;
  xyz.symmetry_x = xyz.symmetry_y = xyz.symmetry_z = true;
  SparseVoxelVolume symmetric(0.1F);
  const std::array<SculptSample, 1> off_axis{{{{0.4F, 0.4F, 0.4F}, 1}}};
  (void)apply_sculpt_stroke(symmetric, off_axis, xyz);
  require(symmetric.density({4, 4, 4}) > 0.0F && symmetric.density({-4, 4, 4}) > 0.0F);
  require(symmetric.density({4, -4, 4}) > 0.0F && symmetric.density({4, 4, -4}) > 0.0F);

  const auto journal = std::filesystem::temp_directory_path() / "golemics_voxel_sculpt_test.journal";
  std::filesystem::remove(journal);
  VoxelSculptHistory history(SparseVoxelVolume(0.1F), journal);
  history.commit(dab, add);
  const auto active = history.volume().active_voxel_count();
  require(active > 0);
  require(history.undo() && history.volume().active_voxel_count() == 0);
  require(VoxelSculptHistory::recover(0.1F,journal).volume().active_voxel_count()==0);
  require(history.redo() && history.volume().active_voxel_count() == active);
  const auto recovered = VoxelSculptHistory::recover(0.1F, journal);
  require(recovered.volume().active_voxel_count() == active);
  require(recovered.volume().cells()==history.volume().cells());
  auto remesh = recovered.remesh(MeshId{7});
  require(!remesh.indices.empty());
  std::filesystem::remove(journal);

  // Incremental remeshing. extract_surface stays the from-scratch definition of the
  // surface, so pinning remesh() against it is what proves the dirty-brick bookkeeping
  // and the one-voxel neighbour skirt right.
  const auto same_mesh=[](const Mesh& a,const Mesh& b) {
    if(a.indices!=b.indices || a.vertices.size()!=b.vertices.size()) return false;
    for(std::size_t i=0;i<a.vertices.size();++i) {
      const auto& u=a.vertices[i]; const auto& v=b.vertices[i];
      if(u.position.x!=v.position.x||u.position.y!=v.position.y||u.position.z!=v.position.z) return false;
      if(u.normal.x!=v.normal.x||u.normal.y!=v.normal.y||u.normal.z!=v.normal.z) return false;
    }
    return true;
  };
  SparseVoxelVolume incremental(0.1F);
  SculptBrush blob; blob.radius=0.35F; blob.strength=1.0F;
  const std::array<SculptSample,1> seed{{{{0.0F,0.0F,0.0F},1.0F}}};
  (void)apply_sculpt_stroke(incremental,seed,blob);
  require(incremental.cached_brick_count()==0 && incremental.dirty_brick_count()==0);
  require(same_mesh(incremental.remesh(MeshId{3}),incremental.extract_surface(MeshId{3})));
  require(incremental.cached_brick_count()>0 && incremental.dirty_brick_count()==0);
  // Dabs on brick seams (multiples of eight cells) are the case a naive per-brick
  // extraction cracks on, so the walk deliberately crosses them in every axis.
  for(const auto& step:std::array<SculptSample,6>{{{{0.8F,0.0F,0.0F},1.0F},{{0.8F,0.8F,0.0F},1.0F},{{0.0F,0.8F,0.8F},1.0F},{{-0.8F,0.0F,0.8F},1.0F},{{0.0F,-0.8F,-0.8F},1.0F},{{-0.8F,-0.8F,0.0F},1.0F}}}) {
    (void)apply_sculpt_stroke(incremental,std::span{&step,std::size_t{1}},blob);
    require(incremental.dirty_brick_count()>0);
    require(same_mesh(incremental.remesh(MeshId{3}),incremental.extract_surface(MeshId{3})));
    require(incremental.dirty_brick_count()==0);
  }
  // Carving back out again empties bricks; the cache has to drop them, not keep
  // stale geometry, and a no-op write must not dirty anything.
  SculptBrush carve_back; carve_back.mode=SculptBrushMode::subtract; carve_back.radius=0.5F; carve_back.strength=1.0F; carve_back.falloff=SculptFalloff::constant;
  const std::array<SculptSample,1> erase{{{{0.8F,0.8F,0.0F},1.0F}}};
  (void)apply_sculpt_stroke(incremental,erase,carve_back);
  require(same_mesh(incremental.remesh(MeshId{3}),incremental.extract_surface(MeshId{3})));
  incremental.set_density({0,0,0},incremental.density({0,0,0}));
  require(incremental.dirty_brick_count()==0);
  // A different isovalue cannot reuse geometry cut at the old one.
  require(same_mesh(incremental.remesh(MeshId{3},"iso",0.8F),incremental.extract_surface(MeshId{3},"iso",0.8F)));
  require(same_mesh(incremental.remesh(MeshId{3}),incremental.extract_surface(MeshId{3})));
  // Boolean intersect edits samples in place rather than through set_density.
  SparseVoxelVolume half(0.1F);
  for(const auto& [cell,value]:incremental.cells()) if(cell.x>=0) half.set_density(cell,value);
  incremental.boolean_intersect(half);
  require(same_mesh(incremental.remesh(MeshId{3}),incremental.extract_surface(MeshId{3})));

  // Welded output: shared edges reference one vertex, so every edge still has exactly
  // two incident triangles and no position appears twice.
  const auto welded=incremental.remesh(MeshId{3});
  require(welded.vertices.size()*3<welded.indices.size());
  std::map<std::pair<std::uint32_t,std::uint32_t>,int> shared;
  for(std::size_t i=0;i<welded.indices.size();i+=3) for(std::size_t j=0;j<3;++j) {
    auto a=welded.indices[i+j],b=welded.indices[i+(j+1)%3];
    if(b<a) std::swap(a,b);
    ++shared[{a,b}];
  }
  for(const auto& [edge,count]:shared) { (void)edge; require(count==2); }
  std::map<Point,std::size_t> distinct;
  for(const auto& vertex:welded.vertices) {
    require(std::isfinite(vertex.normal.x)&&std::isfinite(vertex.normal.y)&&std::isfinite(vertex.normal.z));
    require(distinct.emplace(quantize(vertex.position),distinct.size()).second);
  }

  // Determinism: brick storage is hashed now, so ordering has to come from the
  // extraction itself rather than from container iteration order.
  SparseVoxelVolume forward(0.1F),backward(0.1F);
  const auto ordered=incremental.cells();
  for(auto it=ordered.begin();it!=ordered.end();++it) forward.set_density(it->first,it->second);
  for(auto it=ordered.rbegin();it!=ordered.rend();++it) backward.set_density(it->first,it->second);
  require(same_mesh(forward.extract_surface(MeshId{3}),backward.extract_surface(MeshId{3})));
  require(same_mesh(forward.remesh(MeshId{3}),backward.remesh(MeshId{3})));

  bool rejected = false;
  try { SparseVoxelVolume invalid(0.0F); } catch (const std::invalid_argument&) { rejected = true; }
  require(rejected);
}
