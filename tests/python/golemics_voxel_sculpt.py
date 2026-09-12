"""Native voxel adapter smoke test: blender -b --factory-startup --python this_file."""
import bpy
from mathutils import Euler, Matrix, Vector


def cube(transform=Matrix.Identity(4)):
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    bpy.ops.mesh.primitive_cube_add(size=1)
    obj = bpy.context.object
    obj.matrix_world = transform
    bpy.context.view_layer.update()
    return obj


def geometry(mesh):
    return (tuple(tuple(v.co) for v in mesh.vertices),
            tuple(tuple(p.vertices) for p in mesh.polygons),
            tuple(tuple(e.vertices) for e in mesh.edges))


def execute(obj, **kwargs):
    assert bpy.ops.sculpt.golemics_voxel_sculpt(
        'EXEC_DEFAULT', voxel_size=0.1, radius=0.35, strength=0.8, **kwargs) == {'FINISHED'}
    assert len(obj.data.vertices) > 8, 'Quad corner indices must map to vertex indices'
    assert len(obj.data.edges), 'Extracted preview needs edges for Blender topology'
    assert not obj.data.validate(verbose=True), 'Backend output is a valid Blender mesh'
    return geometry(obj.data)


obj = cube()
original = geometry(obj.data)
# Cancellation must restore exact original topology and custom attributes.
attribute = obj.data.attributes.new('source_test', 'FLOAT', 'POINT')
for i, item in enumerate(attribute.data):
    item.value = i * 0.25
assert bpy.ops.sculpt.golemics_voxel_sculpt(
    'EXEC_DEFAULT', voxel_size=0.1, commit=False, use_dab=True, radius=0.35) == {'FINISHED'}
assert geometry(obj.data) == original
assert [v.value for v in obj.data.attributes['source_test'].data] == [i * 0.25 for i in range(8)]
# A second invocation must work: no session survives EXEC_DEFAULT.
remeshed = execute(obj)

obj = cube()
origin, direction = Vector((0, 0, 2)), Vector((0, 0, -1))
sculpted = execute(obj, use_dab=True, ray_origin=origin, ray_direction=direction)
assert sculpted != remeshed, 'Scripted dab must change the voxel surface'

transform = (Matrix.Translation((4, -3, 2)) @
             Euler((0.3, 0.5, 0.7)).to_matrix().to_4x4() @
             Matrix.Diagonal((2, 0.75, 1.5, 1)))
obj = cube(transform)
transformed = execute(obj, use_dab=True, ray_origin=transform @ origin,
                      ray_direction=transform.to_3x3() @ direction)
# Ray conversion must yield the same local surface even under nonuniform scale.
assert transformed[1:] == sculpted[1:]
assert len(transformed[0]) == len(sculpted[0])
assert all((Vector(a) - Vector(b)).length < 1e-5
           for a, b in zip(transformed[0], sculpted[0]))
print('Golemics voxel Blender smoke test passed')
