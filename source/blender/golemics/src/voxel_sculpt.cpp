#include "dcc/voxel_sculpt.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace dcc {
std::optional<std::pair<Vec3, Vec3>> SparseVoxelVolume::bounds() const {
  if (bricks_.empty()) return std::nullopt;
  Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
  Vec3 hi{-lo.x, -lo.y, -lo.z};
  for (const auto& [key, brick] : bricks_) {
    if (!brick.active) continue;
    lo.x = std::min(lo.x, (float(key.x) * 8.F - 1.F) * voxel_size_);
    lo.y = std::min(lo.y, (float(key.y) * 8.F - 1.F) * voxel_size_);
    lo.z = std::min(lo.z, (float(key.z) * 8.F - 1.F) * voxel_size_);
    hi.x = std::max(hi.x, (float(key.x) * 8.F + 9.F) * voxel_size_);
    hi.y = std::max(hi.y, (float(key.y) * 8.F + 9.F) * voxel_size_);
    hi.z = std::max(hi.z, (float(key.z) * 8.F + 9.F) * voxel_size_);
  }
  return std::pair{lo, hi};
}

namespace {
float clamp01(float value) { return std::clamp(value, 0.0F, 1.0F); }
float length(Vec3 value) { return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z); }
Vec3 subtract(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 lerp(Vec3 a, Vec3 b, float t) { return {std::lerp(a.x,b.x,t), std::lerp(a.y,b.y,t), std::lerp(a.z,b.z,t)}; }
VoxelCoord cell_for(Vec3 p, float size) {
  return {static_cast<std::int32_t>(std::floor(static_cast<double>(p.x)/size)), static_cast<std::int32_t>(std::floor(static_cast<double>(p.y)/size)), static_cast<std::int32_t>(std::floor(static_cast<double>(p.z)/size))};
}
Vec3 center(VoxelCoord c, float s) { return {(static_cast<float>(c.x)+0.5F)*s,(static_cast<float>(c.y)+0.5F)*s,(static_cast<float>(c.z)+0.5F)*s}; }
float weight(float normalized, SculptFalloff falloff) {
  const float x = clamp01(1.0F-normalized);
  if (falloff == SculptFalloff::constant) return normalized <= 1.0F ? 1.0F : 0.0F;
  if (falloff == SculptFalloff::linear) return x;
  return x*x*(3.0F-2.0F*x);
}
}

SparseVoxelVolume::SparseVoxelVolume(float voxel_size) : voxel_size_(voxel_size) {
  if (!std::isfinite(voxel_size) || voxel_size <= 0.0F) throw std::invalid_argument("voxel size must be finite and positive");
}
namespace {
// Floor division keeps negative coordinates in the same 8^3 brick convention.
std::int32_t brick_axis(std::int32_t x) { return x / 8 - (x % 8 < 0 ? 1 : 0); }
VoxelCoord brick_for(VoxelCoord c) { return {brick_axis(c.x), brick_axis(c.y), brick_axis(c.z)}; }
std::size_t brick_offset(VoxelCoord c) {
  const auto local=[](std::int32_t x) { return static_cast<std::size_t>((x % 8 + 8) % 8); };
  return local(c.x) + 8 * local(c.y) + 64 * local(c.z);
}
// A brick owns the cubes whose minimum corner it holds, so extraction reads one
// sample past its far face. These bounds keep that padded corner, and the 8*key
// origin, inside int32 instead of leaving the overflow to the cube loop.
constexpr std::int32_t kMinBrick=std::numeric_limits<std::int32_t>::min()/8;
constexpr std::int32_t kMaxBrick=(std::numeric_limits<std::int32_t>::max()-8)/8;
}
std::size_t SparseVoxelVolume::BrickHash::operator()(const VoxelCoord& cell) const noexcept {
  auto mix=[](std::int32_t v) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(v)); };
  std::uint64_t h=mix(cell.x)*0x9E3779B97F4A7C15ULL ^ (mix(cell.y)+0x9E3779B97F4A7C15ULL)*0xC2B2AE3D27D4EB4FULL ^ (mix(cell.z)+0xC2B2AE3D27D4EB4FULL)*0x165667B19E3779F9ULL;
  h^=h>>29; h*=0xBF58476D1CE4E5B9ULL; h^=h>>32;
  return static_cast<std::size_t>(h);
}
float SparseVoxelVolume::density(VoxelCoord cell) const noexcept {
  const auto it=bricks_.find(brick_for(cell));
  return it==bricks_.end()?0.0F:it->second.values[brick_offset(cell)];
}
void SparseVoxelVolume::invalidate_cache() noexcept { cache_.clear(); dirty_.clear(); touched_.reset(); cache_ready_=false; }
void SparseVoxelVolume::touch(VoxelCoord cell) {
  // Nothing to invalidate before the first remesh, and a stroke walks one brick at
  // a time, so remembering the last brick keeps this off the per-sample hot path.
  if(!cache_ready_) return;
  const auto key=brick_for(cell);
  if(touched_==key) return;
  touched_=key;
  // A sample is a corner of the cubes owned by its own brick and by the seven
  // bricks below it, so all eight lose validity together.
  for(int dz=-1;dz<=0;++dz) for(int dy=-1;dy<=0;++dy) for(int dx=-1;dx<=0;++dx) dirty_.insert({key.x+dx,key.y+dy,key.z+dz});
}
void SparseVoxelVolume::set_density(VoxelCoord cell, float value) {
  if (!std::isfinite(value)) throw std::invalid_argument("density must be finite");
  value=clamp01(value); if(value<=0.00001F) value=0.0F;
  const auto key=brick_for(cell);
  auto it=bricks_.find(key);
  if(it==bricks_.end()) {
    if(value==0.0F) return;
    it=bricks_.try_emplace(key).first;
  }
  auto& brick=it->second;
  auto& old=brick.values[brick_offset(cell)];
  if(old==value) return;
  touch(cell);
  if(old==0.0F && value!=0.0F) { ++brick.active; ++active_voxels_; }
  if(old!=0.0F && value==0.0F) { --brick.active; --active_voxels_; }
  old=value;
  if(brick.active==0) bricks_.erase(it);
}
std::map<VoxelCoord,float> SparseVoxelVolume::cells() const {
  std::map<VoxelCoord,float> result;
  for(const auto& [key,brick]:bricks_) for(int z=0;z<8;++z) for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
    const float value=brick.values[static_cast<std::size_t>(x+8*y+64*z)];
    if(value!=0.0F) result.emplace(VoxelCoord{key.x*8+x,key.y*8+y,key.z*8+z},value);
  }
  return result;
}
SparseVoxelVolume SparseVoxelVolume::voxelize(const Mesh& mesh, float voxel_size) {
  SparseVoxelVolume result(voxel_size);
  if(mesh.indices.size()%3!=0) throw std::invalid_argument("mesh indices must describe triangles");
  if(mesh.indices.empty()) return result;
  std::map<std::pair<Vec3,Vec3>,std::size_t> edges;
  Vec3 lo{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
  Vec3 hi{-lo.x,-lo.y,-lo.z};
  for(const auto index:mesh.indices) {
    if(index>=mesh.vertices.size()) throw std::invalid_argument("mesh index out of range");
    const auto p=mesh.vertices[index].position;
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)) throw std::invalid_argument("mesh position must be finite");
    lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
    hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
  }
  const auto checked_cell=[&](float v) {
    const double c=std::floor(static_cast<double>(v)/voxel_size);
    if(c<std::numeric_limits<std::int32_t>::min()+1.0 || c>std::numeric_limits<std::int32_t>::max()-1.0) throw std::invalid_argument("voxel coordinates out of range");
    return static_cast<std::int32_t>(c);
  };
  const VoxelCoord first{checked_cell(lo.x),checked_cell(lo.y),checked_cell(lo.z)},last{checked_cell(hi.x),checked_cell(hi.y),checked_cell(hi.z)};
  const auto nx=static_cast<std::uint64_t>(static_cast<std::int64_t>(last.x)-first.x+1);
  const auto ny=static_cast<std::uint64_t>(static_cast<std::int64_t>(last.y)-first.y+1);
  const auto nz=static_cast<std::uint64_t>(static_cast<std::int64_t>(last.z)-first.z+1);
  constexpr std::uint64_t max_cells=8'000'000;
  if(nx>max_cells || ny>max_cells/nx || nz>max_cells/(nx*ny)) throw std::invalid_argument("voxelization exceeds 8 million bounding cells; increase voxel size");
  // Most voxelization work writes cells in spatial order. Reserve the rough
  // upper bound of bricks up front so a large scan does not repeatedly rehash
  // the sparse table; cap it to keep thin, pathological bounds inexpensive.
  const auto estimated_bricks=std::min<std::uint64_t>((nx*ny*nz+511U)/512U, 1'000'000U);
  result.bricks_.reserve(static_cast<std::size_t>(estimated_bricks));
  bool closed=true;
  for(std::size_t i=0;i<mesh.indices.size();i+=3) for(std::size_t j=0;j<3;++j) {
    auto a=mesh.vertices[mesh.indices[i+j]].position,b=mesh.vertices[mesh.indices[i+(j+1)%3]].position;
    if(a==b) closed=false;
    if(b<a) std::swap(a,b);
    ++edges[{a,b}];
  }
  for(const auto& [edge,count]:edges) { (void)edge; if(count!=2) closed=false; }
  if(!closed) {
    // Open/nonmanifold input retains the sampled surface policy; never invent an interior.
    std::uint64_t samples=0;
    for(std::size_t i=0;i<mesh.indices.size();i+=3) {
      const Vec3 a=mesh.vertices[mesh.indices[i]].position,b=mesh.vertices[mesh.indices[i+1]].position,c=mesh.vertices[mesh.indices[i+2]].position;
      const double requested=std::ceil(static_cast<double>(std::max({length(subtract(a,b)),length(subtract(b,c)),length(subtract(c,a))}))/voxel_size);
      if(!std::isfinite(requested)||requested>4000.0) throw std::invalid_argument("surface sampling budget exceeded; increase voxel size");
      const int steps=std::max(1,static_cast<int>(requested));
      samples+=static_cast<std::uint64_t>(steps+1)*static_cast<std::uint64_t>(steps+2)/2;
      if(samples>max_cells) throw std::invalid_argument("surface sampling budget exceeded; increase voxel size");
      for(int u=0;u<=steps;++u) for(int v=0;v<=steps-u;++v) {
        const float fu=static_cast<float>(u)/static_cast<float>(steps),fv=static_cast<float>(v)/static_cast<float>(steps);
        result.set_density(cell_for({a.x+(b.x-a.x)*fu+(c.x-a.x)*fv,a.y+(b.y-a.y)*fu+(c.y-a.y)*fv,a.z+(b.z-a.z)*fu+(c.z-a.z)*fv},voxel_size),1.0F);
      }
    }
    return result;
  }
  // Rasterize triangles onto yz scanlines. Half-open edges assign shared projected
  // edges to exactly one triangle, including face diagonals through sample centers.
  std::map<std::pair<std::int32_t,std::int32_t>,std::vector<double>> crossings;
  const auto orient=[](Vec3 a,Vec3 b,double y,double z) { return (static_cast<double>(b.y)-a.y)*(z-a.z)-(static_cast<double>(b.z)-a.z)*(y-a.y); };
  const auto inclusive=[](Vec3 a,Vec3 b) { return b.z>a.z || (b.z==a.z && b.y<a.y); };
  std::uint64_t raster_work=0;
  for(std::size_t i=0;i<mesh.indices.size();i+=3) {
    Vec3 a=mesh.vertices[mesh.indices[i]].position,b=mesh.vertices[mesh.indices[i+1]].position,c=mesh.vertices[mesh.indices[i+2]].position;
    double area=orient(a,b,c.y,c.z);
    if(area==0.0) continue;
    if(area<0.0) { std::swap(b,c); area=-area; }
    const auto ymin=checked_cell(std::min({a.y,b.y,c.y})),ymax=checked_cell(std::max({a.y,b.y,c.y}));
    const auto zmin=checked_cell(std::min({a.z,b.z,c.z})),zmax=checked_cell(std::max({a.z,b.z,c.z}));
    raster_work+=static_cast<std::uint64_t>(static_cast<std::int64_t>(ymax)-ymin+1)*static_cast<std::uint64_t>(static_cast<std::int64_t>(zmax)-zmin+1);
    if(raster_work>100'000'000) throw std::invalid_argument("voxelization raster budget exceeded; simplify mesh or increase voxel size");
    for(auto z=zmin;z<=zmax;++z) for(auto y=ymin;y<=ymax;++y) {
      const double py=(static_cast<double>(y)+0.5)*voxel_size,pz=(static_cast<double>(z)+0.5)*voxel_size;
      const double wa=orient(b,c,py,pz),wb=orient(c,a,py,pz),wc=orient(a,b,py,pz);
      if((wa<0.0 || (wa==0.0&&!inclusive(b,c))) || (wb<0.0 || (wb==0.0&&!inclusive(c,a))) || (wc<0.0 || (wc==0.0&&!inclusive(a,b)))) continue;
      crossings[{y,z}].push_back((wa*a.x+wb*b.x+wc*c.x)/area);
    }
  }
  for(auto& [row,xs]:crossings) {
    std::sort(xs.begin(),xs.end());
    if(xs.size()%2!=0) throw std::invalid_argument("mesh has inconsistent solid scanline intersections");
    for(std::size_t i=0;i<xs.size();i+=2) {
      const auto start=static_cast<std::int32_t>(std::max(static_cast<double>(first.x),std::ceil(xs[i]/voxel_size-0.5)));
      const auto stop=static_cast<std::int32_t>(std::min(static_cast<double>(last.x),std::ceil(xs[i+1]/voxel_size-0.5)-1.0));
      for(auto x=start;x<=stop;++x) result.set_density({x,row.first,row.second},1.0F);
    }
  }
  return result;
}
void SparseVoxelVolume::boolean_union(const SparseVoxelVolume& other) {
  if (voxel_size_!=other.voxel_size_) throw std::invalid_argument("voxel sizes must match");
  if(this==&other) return;
  for(const auto& [key,brick]:other.bricks_) for(int z=0;z<8;++z) for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
    const float d=brick.values[static_cast<std::size_t>(x+8*y+64*z)];
    if(d==0.0F) continue;
    const VoxelCoord c{key.x*8+x,key.y*8+y,key.z*8+z};
    set_density(c,std::max(density(c),d));
  }
}
void SparseVoxelVolume::boolean_subtract(const SparseVoxelVolume& other) {
  if (voxel_size_!=other.voxel_size_) throw std::invalid_argument("voxel sizes must match");
  if(this==&other) { bricks_.clear(); active_voxels_=0; invalidate_cache(); return; }
  for(const auto& [key,brick]:other.bricks_) for(int z=0;z<8;++z) for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
    const float d=brick.values[static_cast<std::size_t>(x+8*y+64*z)];
    if(d==0.0F) continue;
    const VoxelCoord c{key.x*8+x,key.y*8+y,key.z*8+z};
    set_density(c,std::max(0.0F,density(c)-d));
  }
}
void SparseVoxelVolume::boolean_intersect(const SparseVoxelVolume& other) {
  if (voxel_size_!=other.voxel_size_) throw std::invalid_argument("voxel sizes must match");
  if(this==&other) return;
  // Unlike union/subtract this edits brick samples in place rather than through
  // set_density, so the incremental cache is dropped wholesale instead.
  invalidate_cache();
  for(auto it=bricks_.begin();it!=bricks_.end();) {
    const auto key=it->first; auto& brick=it->second;
    for(int z=0;z<8;++z) for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
      auto& d=brick.values[static_cast<std::size_t>(x+8*y+64*z)];
      if(d==0.0F) continue;
      d=std::min(d,other.density({key.x*8+x,key.y*8+y,key.z*8+z}));
      if(d==0.0F) { --brick.active; --active_voxels_; }
    }
    if(brick.active==0) it=bricks_.erase(it); else ++it;
  }
}
namespace {
Vec3 cross_product(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float dot_product(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
// Welding compares raw bits rather than a tolerance: crossings on a shared edge are
// evaluated from the same two samples in the same canonical order, so coincident
// vertices are bit-identical and near-but-distinct ones must stay distinct. The one
// pair that compares equal with different bits is -0 and +0, so it is folded first.
std::uint32_t position_bits(float v) { if(v==0.0F) v=0.0F; return std::bit_cast<std::uint32_t>(v); }
struct WeldKey { std::uint32_t x{},y{},z{}; bool operator==(const WeldKey&) const = default; };
std::size_t weld_hash(const WeldKey& k) noexcept {
  std::uint64_t h=((static_cast<std::uint64_t>(k.x)<<32)|k.y)*0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(k.z)+0xC2B2AE3D27D4EB4FULL)*0xBF58476D1CE4E5B9ULL;
  h^=h>>29; h*=0x94D049BB133111EBULL; h^=h>>32;
  return static_cast<std::size_t>(h);
}
// Assembly is the whole cost of a remesh once extraction is incremental, and it is
// three hash probes per triangle. A node-based map spends that budget on allocation
// and pointer chasing, so the weld table is a flat linear-probe array instead.
class VertexTable {
public:
  explicit VertexTable(std::size_t expected) { std::size_t capacity=16; while(capacity<expected*2) capacity*=2; slots_.assign(capacity,Slot{}); }
  // Reports the index already held for `key`, or claims `next` for it.
  std::pair<std::uint32_t,bool> intern(const WeldKey& key,std::uint32_t next) {
    if(used_*4>=slots_.size()*3) grow();
    auto& slot=probe(slots_,key);
    if(slot.used) return {slot.index,false};
    slot={key,next,true}; ++used_; return {next,true};
  }
private:
  struct Slot { WeldKey key{}; std::uint32_t index{}; bool used{}; };
  static Slot& probe(std::vector<Slot>& slots,const WeldKey& key) {
    const std::size_t mask=slots.size()-1;
    std::size_t i=weld_hash(key)&mask;
    while(slots[i].used && !(slots[i].key==key)) i=(i+1)&mask;
    return slots[i];
  }
  void grow() { std::vector<Slot> bigger(slots_.size()*2); for(const auto& slot:slots_) if(slot.used) probe(bigger,slot.key)=slot; slots_=std::move(bigger); }
  std::vector<Slot> slots_;
  std::size_t used_{};
};
}
SparseVoxelVolume::BrickGeometry SparseVoxelVolume::extract_brick(VoxelCoord key,float iso) const {
  BrickGeometry out;
  if(key.x<kMinBrick||key.y<kMinBrick||key.z<kMinBrick||key.x>kMaxBrick||key.y>kMaxBrick||key.z>kMaxBrick) return out;
  // One padded 9^3 gather replaces eight map lookups per cube corner: the eight
  // bricks the block can straddle are each found once, then every tetrahedron
  // reads samples by array index.
  std::array<float,729> block{};
  bool crossing=false;
  for(int ez=0;ez<2;++ez) for(int ey=0;ey<2;++ey) for(int ex=0;ex<2;++ex) {
    const auto it=bricks_.find({key.x+ex,key.y+ey,key.z+ez});
    if(it==bricks_.end()) continue;
    const auto& values=it->second.values;
    const int nx=ex?1:8,ny=ey?1:8,nz=ez?1:8,ox=ex?8:0,oy=ey?8:0,oz=ez?8:0;
    for(int z=0;z<nz;++z) for(int y=0;y<ny;++y) for(int x=0;x<nx;++x) {
      const float d=values[static_cast<std::size_t>(x+8*y+64*z)];
      if(d>=iso) crossing=true;
      block[static_cast<std::size_t>((ox+x)+9*(oy+y)+81*(oz+z))]=d;
    }
  }
  if(!crossing) return out;
  // A brick tops out at 8^3 cubes, and a marching-tetrahedra cube emits at most a
  // handful of triangles, so this table never grows past its first allocation.
  VertexTable local{512};
  constexpr std::array<std::array<int,4>,6> tetra{{{{0,1,3,7}},{{0,3,2,7}},{{0,2,6,7}},{{0,6,4,7}},{{0,4,5,7}},{{0,5,1,7}}}};
  const std::int32_t ox=key.x*8,oy=key.y*8,oz=key.z*8;
  for(int lz=0;lz<8;++lz) for(int ly=0;ly<8;++ly) for(int lx=0;lx<8;++lx) {
    std::array<float,8> d{}; int above=0;
    for(int i=0;i<8;++i) { d[static_cast<std::size_t>(i)]=block[static_cast<std::size_t>((lx+(i&1))+9*(ly+((i>>1)&1))+81*(lz+((i>>2)&1)))]; if(d[static_cast<std::size_t>(i)]>=iso) ++above; }
    if(above==0||above==8) continue;
    std::array<Vec3,8> p{};
    for(int i=0;i<8;++i) p[static_cast<std::size_t>(i)]=center({ox+lx+(i&1),oy+ly+((i>>1)&1),oz+lz+((i>>2)&1)},voxel_size_);
    for(const auto& t:tetra) {
      std::array<int,4> inside{},outside{}; int ni=0,no=0;
      for(int v:t) { if(d[static_cast<std::size_t>(v)]>=iso) inside[static_cast<std::size_t>(ni++)]=v; else outside[static_cast<std::size_t>(no++)]=v; }
      if(ni==0 || ni==4) continue;
      // Ordering the endpoints by corner index is what makes welding exact: adjacent
      // cubes address a shared lattice edge with different indices but the same
      // relative order, so both evaluate the identical lerp.
      const auto edge=[&](int i,int j) { if(i>j) std::swap(i,j); const auto lo=static_cast<std::size_t>(i),hi=static_cast<std::size_t>(j); return lerp(p[lo],p[hi],(iso-d[lo])/(d[hi]-d[lo])); };
      const Vec3 outward=subtract(p[static_cast<std::size_t>(outside[0])],p[static_cast<std::size_t>(inside[0])]);
      const auto emit=[&](Vec3 a,Vec3 b,Vec3 e) {
        const auto normal=cross_product(subtract(b,a),subtract(e,a));
        if(length(normal)==0.0F) return;
        if(dot_product(normal,outward)<0.0F) std::swap(b,e);
        for(const Vec3 corner:{a,b,e}) {
          const auto [index,inserted]=local.intern({position_bits(corner.x),position_bits(corner.y),position_bits(corner.z)},static_cast<std::uint32_t>(out.positions.size()));
          if(inserted) out.positions.push_back(corner);
          out.corners.push_back(index);
        }
      };
      if(ni==1) emit(edge(inside[0],outside[0]),edge(inside[0],outside[1]),edge(inside[0],outside[2]));
      else if(ni==3) emit(edge(outside[0],inside[0]),edge(outside[0],inside[1]),edge(outside[0],inside[2]));
      else {
        const auto a=edge(inside[0],outside[0]),b=edge(inside[0],outside[1]),e=edge(inside[1],outside[0]),f=edge(inside[1],outside[1]);
        emit(a,b,e); emit(b,f,e);
      }
    }
  }
  return out;
}
std::map<VoxelCoord,SparseVoxelVolume::BrickGeometry> SparseVoxelVolume::build_all(float iso) const {
  // Every active sample is a corner of cubes owned by its brick and the seven below
  // it; that candidate set is what bounds extraction to the neighbourhood of the
  // surface instead of the empty space between disconnected components.
  std::set<VoxelCoord> candidates;
  for(const auto& [key,brick]:bricks_) { static_cast<void>(brick);
    for(int dz=-1;dz<=0;++dz) for(int dy=-1;dy<=0;++dy) for(int dx=-1;dx<=0;++dx) candidates.insert({key.x+dx,key.y+dy,key.z+dz});
  }
  std::map<VoxelCoord,BrickGeometry> geometry;
  for(const auto candidate:candidates) { auto brick=extract_brick(candidate,iso); if(!brick.empty()) geometry.emplace(candidate,std::move(brick)); }
  return geometry;
}
Mesh SparseVoxelVolume::assemble(MeshId id,std::string name,const std::map<VoxelCoord,BrickGeometry>& geometry) const {
  Mesh out{id,std::move(name)};
  std::size_t triangles=0;
  for(const auto& [key,brick]:geometry) { static_cast<void>(key); triangles+=brick.corners.size()/3; }
  // A closed surface welds to a little over half as many vertices as triangles;
  // sizing to that keeps the table's zero-fill off the critical path, and it grows
  // on its own if a sheet-like volume beats the estimate.
  VertexTable welded{triangles/2+1};
  // Sums of unnormalized face normals, so each face contributes in proportion to its
  // area; the first face is kept as a fallback for the sheets where they cancel.
  std::vector<Vec3> accumulated,fallback;
  const std::size_t expected=triangles*2/3+1;
  out.indices.reserve(triangles*3); out.vertices.reserve(expected); accumulated.reserve(expected); fallback.reserve(expected);
  // Bricks are visited in key order and cubes in a fixed order inside each, so the
  // same samples always produce the same vertex and index buffers. A brick's corners
  // already name welded positions, so the global table is probed once per position a
  // brick holds rather than once per triangle corner; the numbering is unchanged
  // because a brick's positions are in the order their corners first appear.
  constexpr auto unclaimed=std::numeric_limits<std::uint32_t>::max();
  std::vector<std::uint32_t> global;
  for(const auto& [key,brick]:geometry) { static_cast<void>(key);
    global.assign(brick.positions.size(),unclaimed);
    for(std::size_t i=0;i+2<brick.corners.size();i+=3) {
      const Vec3 a=brick.positions[brick.corners[i]],b=brick.positions[brick.corners[i+1]],e=brick.positions[brick.corners[i+2]];
      const Vec3 normal=cross_product(subtract(b,a),subtract(e,a));
      for(std::size_t j=0;j<3;++j) {
        const auto corner=brick.corners[i+j];
        auto& index=global[corner];
        if(index==unclaimed) {
          const Vec3 v=brick.positions[corner];
          if(out.vertices.size()>=std::numeric_limits<std::uint32_t>::max()) throw std::length_error("voxel mesh exceeds index range");
          const auto [claimed,inserted]=welded.intern({position_bits(v.x),position_bits(v.y),position_bits(v.z)},static_cast<std::uint32_t>(out.vertices.size()));
          if(inserted) { out.vertices.push_back({v,{}}); accumulated.push_back({}); fallback.push_back(normal); }
          index=claimed;
        }
        auto& sum=accumulated[index];
        sum={sum.x+normal.x,sum.y+normal.y,sum.z+normal.z};
        out.indices.push_back(index);
      }
    }
  }
  for(std::size_t i=0;i<out.vertices.size();++i) {
    Vec3 normal=accumulated[i]; float norm=length(normal);
    if(norm==0.0F) { normal=fallback[i]; norm=length(normal); }
    out.vertices[i].normal={normal.x/norm,normal.y/norm,normal.z/norm};
  }
  return out;
}
Mesh SparseVoxelVolume::extract_surface(MeshId id, std::string name, float iso) const {
  if (!std::isfinite(iso) || iso<=0.0F || iso>1.0F) throw std::invalid_argument("iso must be in (0,1]");
  return assemble(id,std::move(name),build_all(iso));
}
Mesh SparseVoxelVolume::remesh(MeshId id, std::string name, float iso) const {
  if (!std::isfinite(iso) || iso<=0.0F || iso>1.0F) throw std::invalid_argument("iso must be in (0,1]");
  if(!cache_ready_ || cache_iso_!=iso) { cache_=build_all(iso); cache_iso_=iso; cache_ready_=true; }
  else for(const auto key:dirty_) { auto brick=extract_brick(key,iso); if(brick.empty()) cache_.erase(key); else cache_[key]=std::move(brick); }
  dirty_.clear(); touched_.reset();
  return assemble(id,std::move(name),cache_);
}

std::vector<SculptSample> sample_sculpt_stroke(std::span<const SculptSample> points,float spacing) {
  if(!std::isfinite(spacing)||spacing<=0.0F) throw std::invalid_argument("stroke spacing must be finite and positive");
  constexpr std::size_t max_samples=1'000'000;
  if(points.size()>max_samples) throw std::invalid_argument("too many sculpt control points");
  for(const auto& point:points) {
    if(!std::isfinite(point.position.x)||!std::isfinite(point.position.y)||!std::isfinite(point.position.z)||!std::isfinite(point.pressure)) throw std::invalid_argument("sculpt samples must be finite");
  }
  const auto distance_between=[](Vec3 a,Vec3 b) { return std::hypot(static_cast<double>(a.x)-b.x,static_cast<double>(a.y)-b.y,static_cast<double>(a.z)-b.z); };
  std::size_t budget=points.size();
  for(std::size_t i=1;i<points.size();++i) {
    const double count=std::floor(distance_between(points[i].position,points[i-1].position)/spacing);
    if(count>static_cast<double>(max_samples-budget)) throw std::invalid_argument("sculpt stroke exceeds one million dabs");
    budget+=static_cast<std::size_t>(count);
  }
  std::vector<SculptSample> out; if(points.empty()) return out; out.reserve(budget); out.push_back(points.front());
  for(std::size_t i=1;i<points.size();++i) {
    const double distance=distance_between(points[i].position,points[i-1].position);
    const auto n=static_cast<std::size_t>(std::floor(distance/spacing));
    for(std::size_t j=1;j<=n;++j) {
      const float t=static_cast<float>(std::min(1.0,static_cast<double>(j)*spacing/distance));
      out.push_back({lerp(points[i-1].position,points[i].position,t),std::lerp(points[i-1].pressure,points[i].pressure,t)});
    }
    if(n==0 || distance_between(out.back().position,points[i].position)>spacing*0.25) out.push_back(points[i]);
  }
  return out;
}
namespace {

// One dab's neighbourhood, as signed distance in voxels: positive inside the
// surface, negative outside, zero on it. The stored field saturates at
// +/-kSculptBand, and voxelize() seeds it binary, so the cube is reconstructed
// per dab rather than read straight out of the volume:
//
//   1. Seed from the stored band, which is exact wherever a brush has been.
//   2. Where a cell is saturated but has a neighbour on the other side of the
//      isovalue, replace it with the interpolated crossing -- for a binary field
//      that is +/-0.5 voxel, the surface lying halfway between cell centres.
//   3. Relax so no two neighbours differ by more than one voxel, which turns
//      those seeds into a distance ramp over the rest of the cube.
//
// Without this every brush is working on a field that is 0 or 1 almost
// everywhere, which is why all but one of them did nothing measurable.
struct DistanceCube {
  int reach{};
  int span{};
  std::vector<float> d;

  [[nodiscard]] std::size_t index(int x, int y, int z) const {
    return (static_cast<std::size_t>(z + reach + 1) * static_cast<std::size_t>(span) +
            static_cast<std::size_t>(y + reach + 1)) * static_cast<std::size_t>(span) +
           static_cast<std::size_t>(x + reach + 1);
  }
  [[nodiscard]] bool inside(int x, int y, int z) const {
    return x >= -reach - 1 && x <= reach + 1 && y >= -reach - 1 && y <= reach + 1 &&
           z >= -reach - 1 && z <= reach + 1;
  }
  [[nodiscard]] float at(int x, int y, int z) const { return d[index(x, y, z)]; }
  float& at(int x, int y, int z) { return d[index(x, y, z)]; }
};

void build_distance_cube(DistanceCube& cube, const SparseVoxelVolume& volume, VoxelCoord origin) {
  const int r = cube.reach;
  const auto density_at = [&](int x, int y, int z) {
    return volume.density({origin.x + x, origin.y + y, origin.z + z});
  };
  // 1 and 2: seed.
  for (int z = -r - 1; z <= r + 1; ++z)
    for (int y = -r - 1; y <= r + 1; ++y)
      for (int x = -r - 1; x <= r + 1; ++x) {
        const float own = density_at(x, y, z);
        float distance = sculpt_distance_of(own);
        if (std::abs(distance) >= kSculptBand - 1e-4F) {
          const float side = own - 0.5F;
          float nearest = kSculptBand;
          constexpr int step[6][3]{{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
          for (const auto& q : step) {
            const float other = density_at(x + q[0], y + q[1], z + q[2]);
            const float across = other - 0.5F;
            if ((side >= 0.0F) == (across >= 0.0F)) continue;
            const float t = std::abs(side) / std::max(std::abs(side - across), 1e-6F);
            nearest = std::min(nearest, t);
          }
          if (nearest < kSculptBand) distance = (side >= 0.0F ? 1.0F : -1.0F) * nearest;
        }
        cube.at(x, y, z) = distance;
      }
  // 3: relax. The update is on the MAGNITUDE with the sign held fixed --
  // distance grows away from the surface whichever side of it a cell is on, so
  // `|d| = min(|d|, |neighbour| + 1)`. Constraining the signed value instead
  // lets an outside cell three voxels out drag a correct +0.5 boundary seed
  // negative, which read as the surface having moved and made every brush eat
  // the shell it was supposed to build on.
  for (int pass = 0; pass < 4; ++pass) {
    const bool forward = (pass % 2) == 0;
    for (int zi = -r - 1; zi <= r + 1; ++zi) {
      const int z = forward ? zi : -zi;
      for (int yi = -r - 1; yi <= r + 1; ++yi) {
        const int y = forward ? yi : -yi;
        for (int xi = -r - 1; xi <= r + 1; ++xi) {
          const int x = forward ? xi : -xi;
          const float value = cube.at(x, y, z);
          const float sign = value >= 0.0F ? 1.0F : -1.0F;
          float magnitude = std::abs(value);
          constexpr int step[6][3]{{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
          for (const auto& q : step) {
            if (!cube.inside(x + q[0], y + q[1], z + q[2])) continue;
            magnitude = std::min(magnitude, std::abs(cube.at(x + q[0], y + q[1], z + q[2])) + 1.0F);
          }
          cube.at(x, y, z) = sign * std::min(magnitude, kSculptBand);
        }
      }
    }
  }
}

// Outward unit normal at the dab centre, from the gradient of the reconstructed
// distance. Distance increases inward, so the outward normal is the negated
// gradient. Falls back to +Y where the field is flat, which only happens when the
// dab is nowhere near a surface and the plane brushes have nothing to fit anyway.
Vec3 dab_normal(const DistanceCube& cube) {
  const Vec3 gradient{cube.at(1, 0, 0) - cube.at(-1, 0, 0),
                      cube.at(0, 1, 0) - cube.at(0, -1, 0),
                      cube.at(0, 0, 1) - cube.at(0, 0, -1)};
  const float len = length(gradient);
  if (!(len > 1e-6F)) return {0.0F, 1.0F, 0.0F};
  return {-gradient.x / len, -gradient.y / len, -gradient.z / len};
}

} // namespace

SculptTransaction apply_sculpt_stroke(SparseVoxelVolume& volume,std::span<const SculptSample> controls,const SculptBrush& brush) {
  if(!std::isfinite(brush.radius)||brush.radius<=0.0F||!std::isfinite(brush.strength)||brush.strength<0.0F||!std::isfinite(brush.spacing)||brush.spacing<=0.0F) throw std::invalid_argument("invalid sculpt brush");
  const double requested_reach=std::ceil(static_cast<double>(brush.radius)/volume.voxel_size());
  if(requested_reach>125.0) throw std::invalid_argument("sculpt brush exceeds voxel work budget; reduce radius or increase voxel size");
  const int reach=static_cast<int>(requested_reach);
  const auto samples=sample_sculpt_stroke(controls,brush.radius*brush.spacing);
  const auto diameter=static_cast<std::uint64_t>(reach*2+1);
  const unsigned symmetry_copies = (brush.symmetry_x ? 2U : 1U) *
                                   (brush.symmetry_y ? 2U : 1U) *
                                   (brush.symmetry_z ? 2U : 1U);
  const auto per_sample=diameter*diameter*diameter*symmetry_copies;
  if(samples.size()>16'000'000/per_sample) throw std::invalid_argument("sculpt stroke exceeds 16 million voxel visits");
  // The reconstruction cube is one ring wider than the brush on every side.
  const auto span=static_cast<std::uint64_t>(reach)*2+3;
  if(span*span*span>8'000'000ULL) throw std::invalid_argument("sculpt brush exceeds voxel work budget; reduce radius or increase voxel size");
  // Validate every generated/mirrored dab before the first edit, leaving room for
  // the reconstruction ring as well as the brush cube itself.
  for(const auto& raw:samples) for(unsigned mirror=0; mirror<symmetry_copies; ++mirror) {
    const auto valid_axis=[&](double value) {
      const double cell=std::floor(value/volume.voxel_size());
      return cell>=static_cast<double>(std::numeric_limits<std::int32_t>::min())+reach+1 && cell<=static_cast<double>(std::numeric_limits<std::int32_t>::max())-reach-1;
    };
    const auto sign = [mirror](bool enabled, unsigned bit) {
      return enabled && ((mirror >> bit) & 1U) ? -1.0 : 1.0;
    };
    if(!valid_axis(sign(brush.symmetry_x, 0U)*raw.position.x)||
       !valid_axis(sign(brush.symmetry_y, 1U)*raw.position.y)||
       !valid_axis(sign(brush.symmetry_z, 2U)*raw.position.z)) throw std::invalid_argument("sculpt sample coordinates out of range");
  }
  std::map<VoxelCoord,float> before;
  const float voxel=volume.voxel_size();
  DistanceCube cube;
  cube.reach=reach;
  cube.span=reach*2+3;
  cube.d.assign(static_cast<std::size_t>(cube.span)*static_cast<std::size_t>(cube.span)*
                    static_cast<std::size_t>(cube.span),
                0.0F);

  for(const auto& raw:samples) for(unsigned mirror=0; mirror<symmetry_copies; ++mirror) {
    auto sample=raw;
    if (brush.symmetry_x && ((mirror >> 0U) & 1U)) sample.position.x=-sample.position.x;
    if (brush.symmetry_y && ((mirror >> 1U) & 1U)) sample.position.y=-sample.position.y;
    if (brush.symmetry_z && ((mirror >> 2U) & 1U)) sample.position.z=-sample.position.z;
    const auto origin=cell_for(sample.position,voxel);
    const float pressure=clamp01(sample.pressure);
    if(!(pressure>0.0F)) continue;
    // How far one dab may move the surface, in voxels. Strength is the fraction
    // of the brush radius a single dab lays down, so a brush behaves the same at
    // any voxel size.
    const float advance=pressure*brush.strength*brush.radius/voxel;
    if(!(advance>0.0F)) continue;
    build_distance_cube(cube,volume,origin);

    // Plane brushes need a surface to work against: the dab centre with the
    // field's own normal there. Blender fits a plane to the whole brush area;
    // the centre is already a point on the surface the pointer chose, which is
    // the same plane to within the curvature under one brush.
    const Vec3 normal=dab_normal(cube);
    const bool plane_brush=brush.mode==SculptBrushMode::flatten||
                           brush.mode==SculptBrushMode::clay_add||
                           brush.mode==SculptBrushMode::clay_strips;
    // Clay deposits toward a plane lifted off the surface, which is what makes it
    // build up rather than level off.
    const float plane_lift=brush.mode==SculptBrushMode::flatten?0.0F
                          :brush.mode==SculptBrushMode::clay_add?0.35F*brush.radius/voxel
                                                                :0.25F*brush.radius/voxel;
    // Height of the plane along `normal`, relative to the dab centre: the
    // falloff-weighted mean of where the surface actually sits under the brush.
    //
    // Fitting it to the area rather than pinning it through the dab centre is
    // what makes flatten work at all. On any convex surface the centre is the
    // high point, so a plane through it lies above every other surface point
    // under the brush and flatten can only ever shave the peak -- which is
    // exactly what it did: 0.04 voxels of movement at the rim over twenty dabs.
    // An area plane cuts what stands above it and fills what falls below it,
    // which is the brush people expect and what Blender's area plane does.
    float plane_height=0.0F;
    if(plane_brush) {
      float weight_sum=0.0F,height_sum=0.0F;
      for(int z=-reach;z<=reach;++z) for(int y=-reach;y<=reach;++y) for(int x=-reach;x<=reach;++x) {
        const float band=cube.at(x,y,z);
        if(std::abs(band)>1.0F) continue;   // only cells straddling the surface
        const VoxelCoord c{origin.x+x,origin.y+y,origin.z+z};
        const Vec3 off=subtract(center(c,voxel),sample.position);
        const float radial=length(off);
        if(radial>brush.radius) continue;
        const float w=weight(radial/brush.radius,brush.falloff);
        if(!(w>0.0F)) continue;
        // The surface near this cell lies `band` voxels along the outward normal.
        height_sum+=w*((off.x*normal.x+off.y*normal.y+off.z*normal.z)+band*voxel);
        weight_sum+=w;
      }
      if(weight_sum>0.0F) plane_height=height_sum/weight_sum;
    }

    for(int z=-reach;z<=reach;++z) for(int y=-reach;y<=reach;++y) for(int x=-reach;x<=reach;++x) {
      const VoxelCoord c{origin.x+x,origin.y+y,origin.z+z};
      const Vec3 centre=center(c,voxel);
      const Vec3 offset=subtract(centre,sample.position);
      const float distance=length(offset);
      // Clay strips has a boxy footprint, as it does everywhere else it exists:
      // that flat-edged deposit is the whole point of the brush, and a round
      // falloff makes it indistinguishable from inflate -- which is exactly what
      // it was, the two sharing one line of arithmetic.
      const float footprint=brush.mode==SculptBrushMode::clay_strips
                                ? std::max({std::abs(offset.x),std::abs(offset.y),std::abs(offset.z)})
                                : distance;
      if(footprint>brush.radius) continue;
      float fall=weight(footprint/brush.radius,brush.falloff);
      if(brush.mode==SculptBrushMode::crease) fall*=fall*fall;   // creases are narrow
      if(!(fall>0.0F)) continue;

      const float old=cube.at(x,y,z);
      const float ball=(brush.radius-distance)/voxel;   // signed distance to the brush ball
      const float step=advance*fall;
      float next=old;
      switch(brush.mode) {
        case SculptBrushMode::add:
          // Union with the ball, approached at `step` a dab so the surface eases
          // out instead of snapping. Bounded by the ball, so holding still
          // converges rather than running away.
          if(ball>old) next=std::min(ball,old+step*2.0F);
          break;
        case SculptBrushMode::subtract:
          // A full-strength subtract removes the occupied band in one dab,
          // matching the scalar brush contract (density 1 -> 0). The signed
          // distance spans both sides of the isosurface, so it needs twice the
          // outward travel used by add to cross the complete band.
          if (ball > 0.0F) {
            next = -kSculptBand;
          } else if (-ball < old) {
            next = std::max(-ball, old - step * 2.0F);
          }
          break;
        case SculptBrushMode::inflate:
          next=old+step;      // no ball bound: pushes the surface along itself
          break;
        case SculptBrushMode::crease:
          next=old-step;
          break;
        case SculptBrushMode::smooth: {
          float sum=0.0F;
          int seen=0;
          constexpr int q[6][3]{{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
          for(const auto& n:q) { if(!cube.inside(x+n[0],y+n[1],z+n[2])) continue; sum+=cube.at(x+n[0],y+n[1],z+n[2]); ++seen; }
          if(seen>0) next=old+((sum/static_cast<float>(seen))-old)*fall*pressure*std::min(1.0F,brush.strength*4.0F);
          break;
        }
        default: {
          if(!plane_brush) break;
          // Distance from the plane through the dab centre, positive on the
          // inside, in voxels -- then lifted for the clay brushes.
          const float plane=-((offset.x*normal.x+offset.y*normal.y+offset.z*normal.z)-plane_height)/voxel
                            +plane_lift;
          if(brush.mode==SculptBrushMode::flatten) {
            next=old+(plane-old)*fall*pressure*std::min(1.0F,brush.strength*4.0F);
          } else {
            // Clay only adds, and only up to the lifted plane, so repeated dabs
            // fill a hollow and then stop instead of growing without limit.
            if(plane>old) next=std::min(plane,old+step);
          }
          break;
        }
      }
      if(next==old) continue;
      next=std::clamp(next,-kSculptBand,kSculptBand);
      const float encoded=sculpt_density_of(next);
      const float stored=volume.density(c);
      if(encoded==stored) continue;
      before.try_emplace(c,stored);
      volume.set_density(c,encoded);
      cube.at(x,y,z)=next;
    }
  }
  SculptTransaction tx; for(const auto& [c,old]:before) { const float now=volume.density(c); if(now!=old) tx.changes.push_back({c,old,now}); } return tx;
}

VoxelSculptHistory::VoxelSculptHistory(SparseVoxelVolume volume,std::filesystem::path journal):volume_(std::move(volume)),journal_(std::move(journal)){}
void VoxelSculptHistory::apply_changes(const SculptTransaction& tx,bool forward){for(const auto& c:tx.changes) volume_.set_density(c.cell,forward?c.after:c.before);}
void VoxelSculptHistory::append_journal(const SculptTransaction& tx) const { if(journal_.empty()) return; std::ofstream out(journal_,std::ios::app); if(!out) throw std::runtime_error("cannot open sculpt journal"); out<<std::setprecision(std::numeric_limits<float>::max_digits10); out<<"TX "<<tx.changes.size()<<'\n'; for(const auto& c:tx.changes) out<<c.cell.x<<' '<<c.cell.y<<' '<<c.cell.z<<' '<<c.before<<' '<<c.after<<'\n'; out.flush(); if(!out) throw std::runtime_error("cannot write sculpt journal"); }
void VoxelSculptHistory::commit(std::span<const SculptSample> points,const SculptBrush& brush){ auto tx=apply_sculpt_stroke(volume_,points,brush); if(tx.changes.empty()) return; try{append_journal(tx);}catch(...){apply_changes(tx,false);throw;} undo_.push_back(std::move(tx));redo_.clear();}
bool VoxelSculptHistory::undo(){
  if(undo_.empty()) return false;
  SculptTransaction inverse=undo_.back();
  for(auto& c:inverse.changes) std::swap(c.before,c.after);
  append_journal(inverse);
  apply_changes(undo_.back(),false);
  redo_.push_back(std::move(undo_.back())); undo_.pop_back();
  return true;
}
bool VoxelSculptHistory::redo(){
  if(redo_.empty()) return false;
  append_journal(redo_.back());
  apply_changes(redo_.back(),true);
  undo_.push_back(std::move(redo_.back())); redo_.pop_back();
  return true;
}
Mesh VoxelSculptHistory::remesh(MeshId id,std::string name)const{return volume_.remesh(id,std::move(name));}
VoxelSculptHistory VoxelSculptHistory::recover(float voxel_size,const std::filesystem::path& path){ VoxelSculptHistory result{SparseVoxelVolume(voxel_size),path}; std::ifstream in(path); std::string tag; std::size_t count{}; while(in>>tag>>count){if(tag!="TX")break;SculptTransaction tx; for(std::size_t i=0;i<count;++i){VoxelChange c;if(!(in>>c.cell.x>>c.cell.y>>c.cell.z>>c.before>>c.after)){tx.changes.clear();break;}tx.changes.push_back(c);}if(tx.changes.size()!=count)break;result.apply_changes(tx,true);}return result;}
} // namespace dcc
