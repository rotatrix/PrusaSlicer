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
reports the application PID and version, the `app.prusaslicer` tag, navigation
capability and focus. The integration is always enabled and connects
automatically. The SDK owns transport, retries, gesture state and reconciliation.

The plater owns a single integration. It follows the current canvas (3D view,
preview or G-code viewer), so there is one Rotatrix connection per application.
A wx scheduler dispatches work on the UI thread using queued callbacks and a
one-shot deadline timer. The captured context includes the canvas, model,
scene revision, active bed, size and DPI; switching views, reloading the scene,
hiding the view, opening a modal dialog or losing application focus cancels
the active gesture. The plater shuts the integration down before its canvases.

Camera writes keep PrusaSlicer's own orbit distance, so native mouse orbit
continues around the camera's native target. A write never changes the
projection, which is the user's persisted preference; it returns the realized
pose after native zoom and scene-clipping constraints. Native camera changes,
including 3Dconnexion input, are detected before SDK dispatch and on rendering.
Cursor coordinates account for Retina framebuffer scaling.

Surface picks use native mesh raycasters with the active gizmo's clipping plane,
exactly like hover picking, without changing hover/selection state or including
gizmo handles. Hits report the picked part's bounds. Beds participate only in
ordinary picks, never selection picks. In preview, toolpaths are picked from
their rendered depth at the sample pixel and model bounds include the G-code
paths; preview has no selection facts.

Use **View > OpenAxis Diagnostics** to show connection status, query evidence
and corrections in the viewport. The toggle applies to all views and is not
saved. Session logs are written by the SDK; use the Rotatrix log viewer. The
green pivot marker is independent of diagnostics. It is depth tested per
fragment: visible parts are opaque and occluded parts show through faintly.

The standalone tests in tests/openaxis cover clipping and viewport/DPI mapping.
Before distributing a build, verify physical-device navigation, native mouse
continuation, reconnect, focus/modal behavior, plater/preview switching,
perspective/orthographic views, picking and diagnostics on each target OS.
See [the integration assessment](OpenAxis-assessment.md) for scope and coverage.
