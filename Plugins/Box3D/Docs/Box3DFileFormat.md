# Box3D scene file (`.box3d`), version 3

The Box3D editor exporter writes the **loaded** box and capsule components and
Landscape component heightfields in the open editor world. World Partition actors in unloaded cells are not present
in the export. Load the cells you need before exporting.

All integers and IEEE 754 floats are little-endian. All positions, half
extents, capsule endpoints, and radii are in **meters**. The file contains no
padding, Unreal object names, or UObject serialization.

## Header (12 bytes)

| Offset | Type | Value |
| --- | --- | --- |
| 0 | 4 bytes | ASCII `B3DF` |
| 4 | `u16` | Format version, currently `3` |
| 6 | `u16` | Reserved, `0` |
| 8 | `u32` | Number of shape records, from `1` to `1,000,000` |

## Each shape record

| Offset within record | Type | Meaning |
| --- | --- | --- |
| 0 | `u8` | Shape type: `0` = box, `2` = capsule, `3` = heightfield |
| 1 | `u8` | Flags: bit `0` = collision enabled, bit `1` = physics enabled |
| 2 | `u8` | Collision category bits from `EBox3DCollisionProfile` |
| 3 | `u8` | Reserved, `0` |
| 4 | `3 × f32` | World position, X/Y/Z |
| 16 | `4 × f32` | World rotation quaternion, X/Y/Z/W |

The common record prefix is 32 bytes. It is followed by one of:

| Type | Data | Total record size |
| --- | --- | --- |
| Box | `3 × f32` local half extents X/Y/Z, then 16-byte component GUID | 60 bytes |
| Capsule | `3 × f32` local endpoint A, `3 × f32` local endpoint B, `f32` radius, then 16-byte component GUID | 76 bytes |

Heightfield records contain `u32 countX`, `u32 countZ`, three `f32` scale
values (local X, height Y, local Z), `f32` global minimum and maximum, a
`u8` clockwise winding flag, `countX * countZ` row-major `f32` heights,
`(countX - 1) * (countZ - 1)` cell material bytes, and a 16-byte GUID.
Material `0` is solid and `255` is a Landscape visibility hole. Heights
are sampled at full loaded Landscape component resolution. The static body
rotates Box3D's height axis to Unreal's up axis; non-positive Landscape
scales are rejected.

The GUID is four little-endian `u32` values (`FGuid.A/B/C/D`). The editor
exporter assigns one to each box and capsule component and marks the level dirty. Save the
level after exporting so the same IDs are present in a later PIE or packaged
session. Heightfield GUIDs are export-only. The importer also accepts version
1 records without GUIDs and version 2 box/capsule records; version 1
bodies can simulate but cannot bind to visual meshes.

Component scale is baked into the dimensions. Box half extents use the
absolute component scale; capsule endpoints use signed component scale and
capsule radius uses the largest absolute scale axis. The shape records are
independent; this format does not group several shapes into one rigid body.

A reader should reject an unknown version or shape type, a count above the
limit, truncated records, non-finite floats, non-positive dimensions, and
unexpected trailing bytes. Use the flags to choose a dynamic or static body
when constructing the Box3D world, then attach the corresponding shape.

## Client simulator

Export the level to `<Project>/Saved/<MapName>.box3d` (for example,
`Saved/MyLevel.box3d`). The ClickNet client subsystem loads this file when
that map starts in Play In Editor or in the game and steps the Box3D world at
60 Hz. Physics enabled records become dynamic bodies with density 1; other
records become static bodies. A record with collision disabled still has a
shape and mass, but its collision mask is zero.

For a file at another location, call
`UClickNetSubsystem::LoadLocalBox3DFile(FilePath)` from C++ or Blueprint.
The call replaces the current local Box3D world only after the file imports
successfully. `GetLocalBox3DWorldId()` exposes the world to client C++ code;
the import does not create Unreal actors. For a version 2 or 3 file, dynamic bodies
drive the matching actor's Static Mesh component. The component's **Visual
Mesh** field selects the mesh when needed. If unset, the client uses an
attached parent Static Mesh or the only Static Mesh on the actor. A mesh can
follow only one body. The matching Box3D component follows the body too, so
its selected green collision outline stays aligned in PIE. Static bodies do
not move their meshes. When an actor has exactly one exported shape, its actor
root and pivot follow that body as well. An actor with multiple independent
exported shapes has no single body transform to follow; its components can
move separately while its actor pivot remains at the authored transform.
During Play In Editor, imported bodies are drawn as wireframes: cyan for
static bodies and orange for dynamic bodies. This view follows their Box3D
simulation transforms.
