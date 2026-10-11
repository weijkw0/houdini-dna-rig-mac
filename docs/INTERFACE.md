# Interface notes

What the official HDAs expect from the two native components, and how RigLogic has to be built and
driven to match Unreal Engine. The interface was taken from the plugin's HDAs (the networks and Python
that call these nodes) and from the exported symbol names of the Linux build; no Epic code is used here.

## SOP `mhdna_import` (`dso/metahuman_dna_import.dylib`)

Parameters: `headdnafile`, `bodydnafile` (file), `maxlod`, `minlod` (int 0..7, default 0; if min < max,
max wins), `addmeshes`, `addjoints`, `addskinning`, `addblendshapes`, `addriglogic` (toggles, default on),
`unitsmultiplier` (float, 1.0), `reload` (button). The head has 8 LODs and the body 4; body LOD = head LOD / 2.

Detail attributes read downstream (e.g. by the `create_capt_pose` wrangle):
- `character_name` (string), `max_min_lod` (int[2]), `units_multiplier` (float)
- `joint_names` (string[]), `joint_parents` (int[], -1 = root), `joint_xforms` (matrix4[], world; joint position = `{0,0,0} * m`)
- `rl_dna_file_head` / `_body` (string), `rl_ctrl_names_head` / `_body` (string[], raw DNA control names such as
  `CTRL_expressions.browDownL` or `head.qx`), `rl_joint_names_head` / `_body` (string[], joints driven by RigLogic)
- `dna_coord_system`, `dna_handedness`, `dna_rotation_sequence`, `dna_rotation_sign`, `dna_face_winding`,
  `dna_translation_unit`, `dna_rotation_unit`

Geometry: one polygon set per DNA mesh, prim `name` = DNA mesh name (`head_lod0_mesh`, `body_lod0_mesh`, ...;
materials are assigned by name), vertex `uv` and `N`, `boneCapture` with the inverse world rest matrices.
Blend shapes follow the KineFX convention: packed prims in the `_3d_hidden_primitives` group with prim
attributes `name` (base mesh), `blendshape_channel`, `blendshape_name` (`{mesh}.{channel}`), sparse points
identified by `id`.

DNA coordinates map directly to Houdini: x = character left = +X, y up, z front, centimetres (x 0.01),
Euler xyz in degrees in parent space.

## APEX callbacks (`apexdso/metahuman_rig_logic.dylib`)

`mh::RigLogic` ports: `skel` (geometry, in/out), `joints` (variadic Matrix4, out), `dna_file`, `max_lod`,
`min_lod`, `lod`, `units_multiplier`, `basic_ctrls` (variadic Float), `joint_ctrls` (variadic Matrix4).
- `basic_ctrls` sub-ports are named after the part of the raw control name after the dot;
  `joint_ctrls` sub-ports are the joints behind the `.qx/.qy/.qz/.qw` controls and receive their local transform.
- `joints` sub-ports are the `rl_joint_names` joints; their values feed the joint nodes' `restlocal_in`.
- Blend shape weights are written to `skel` as detail floats named by channel (read by
  `kinefx::characterblendshapescore`), animated map weights as the detail dict `material_override`
  (keys = map names with `.` replaced by `__`).

`mh::DrivenKeys`: `driven = f(driver)` piecewise linear over `driver_keys` / `driven_keys`, clamped at the ends.

## RigLogic build and semantics

- Build with the defines UE 5.8 uses on Mac (`RigLogicLib.Build.cs`): `RL_BUILD_WITH_XYZ_ROTATION_ORDER`,
  `RL_BUILD_WITH_HALF_FLOATS`, `TRIO_MMAP_AVAILABLE`, `RL_AUTODETECT_SSE`, `RL_AUTODETECT_HALF_FLOATS`.
  Without the XYZ rotation order the Euler-to-quaternion conversion is empty: face joints get no output
  in quaternion mode and body quaternion joints get no rotation in Euler mode.
- Drive it like UE's `AnimNode_RigLogic`: quaternion mode, outputs are deltas from the neutral pose,
  `T = Nt + Dt`, `R = Nq * Dq` (delta applied in the joint's own frame first), `S = Ns + Ds`.
- RBF inputs are the local rotation relative to neutral (`InvNeutral * Abs`) as a quaternion.
  Houdini's `UT_Quaternion` uses the same convention as RigLogic (`tests/quat_convention.py`).
- Both dylibs link RigLogic statically, so it is compiled with hidden visibility; otherwise dyld binds the
  second dylib's status-code registry to the first one's and the duplicate registration aborts.
