# OpenAxis integration for PrusaSlicer 2.9.6

This branch backports the Rotatrix OpenAxis integration to stable PrusaSlicer
2.9.6. It uses the native `GLCanvas3D` and `Camera` APIs rather than the 3.0
scene architecture. The adapter supports camera navigation in the plater and
preview, perspective and orthographic projection, model/selection bounds,
cursor/center surface picks, a transient pivot indicator and diagnostics.

Enable with `-DSLIC3R_OPENAXIS=ON`. CMake 3.24+ and Git fetch the published
`cpp/v1.0.0-rc.1` SDK from https://github.com/rotatrix/openaxis.git. For SDK
 development, set `OPENAXIS_SOURCE_DIR` to a checkout containing `cpp/`.
The feature defaults off and requires a native GUI build. See OpenAxis-CI.md
for reproducible builds and package generation.

Start Rotatrix 1.6+ with a profile matching `app.prusaslicer`. The connection
reports the application PID, `workspace.modeling`, navigation capability and
focus. The SDK owns transport, retries, gesture state and reconciliation.
A wx scheduler dispatches work on the UI thread using queued callbacks and a
one-shot deadline timer. Each canvas has a separate context; only the visible,
enabled current canvas in the active application accepts navigation. Context
changes, hidden views and focus loss invalidate active gestures. Shutdown
invalidates pending callbacks before destroying the canvas.

Camera writes update the legacy camera quaternion, target and orbit distance,
then return the realized pose after native zoom and scene-clipping constraints.
The adapter detects native camera changes before SDK dispatch and on rendering.
Cursor coordinates account for Retina framebuffer scaling. Surface picks use
native mesh raycasters without changing hover/selection state or including
gizmo handles. Unlike 3.0, 2.9 has no selectable bed objects, so beds participate
only in ordinary picks. Preview geometry without mesh raycasters supplies no
surface hit, allowing the server to use its fallback policy.

Open Help > OpenAxis Diagnostics for connection status, camera-write counts,
a bounded event log, copy/clear/pause controls and optional viewport diagnostics.
The green pivot marker is visible independently of diagnostic overlays and fades
when its center is occluded by scene geometry.

The standalone tests in tests/openaxis cover clipping and viewport/DPI mapping.
Before distributing a build, verify physical-device navigation, native mouse
continuation, reconnect, focus/modal behavior, plater/preview switching,
perspective/orthographic views, picking and diagnostics on each target OS.
