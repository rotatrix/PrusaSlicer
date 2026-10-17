# Rotatrix navigation in PrusaSlicer 2.9.6

Camera navigation supports the plater, preview and G-code viewer, perspective
and orthographic projection, selection-aware pivots and viewport diagnostics.

Enable with `-DSLIC3R_OPENAXIS=ON`. CMake 3.24+ and Git fetch the published
`cpp/v1.0.0-rc.1` SDK from https://github.com/rotatrix/openaxis.git. For SDK
development, set `OPENAXIS_SOURCE_DIR` to a checkout containing `cpp/`.
The feature defaults off and requires a native GUI build. See [building](OpenAxis-build.md)
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

Camera writes place the native orbit target on the viewing axis at the OpenAxis
pivot's depth, preserving the requested position and orientation even for an
off-center pivot. With no usable forward pivot, the previous orbit distance is
retained. The user's projection type is preserved. External views bypass native
mouse zoom limits and clip near-eye geometry instead of moving the camera; this
also applies on subsequent redraws. Native fit-to-bed/object restores the usual
scene-fitting behavior. Each write returns the realized pose. Native camera changes,
including 3Dconnexion input, are detected before SDK dispatch and on rendering.
Cursor coordinates account for Retina framebuffer scaling.

Surface picks use native mesh raycasters with the active gizmo's clipping plane,
exactly like hover picking, without changing hover/selection state or including
gizmo handles. Hits report the picked part's bounds. Beds participate only in
ordinary picks, never selection picks. In preview, toolpaths are picked from
their rendered depth in an isolated framebuffer at the sample pixel. The query
uses geometry-only rendering without invoking the legend or other ImGui UI;
the library's optional tool/center-of-gravity markers do not write depth.
If a depth framebuffer is unavailable, the pick falls back to ordinary
mesh/bed candidates. Model bounds include the G-code
paths; preview has no selection facts.

Use **View > OpenAxis Diagnostics** to show connection status, query evidence
and corrections in the viewport. The toggle applies to all views and is not
saved. Session logs are written by the SDK; use the Rotatrix log viewer. The
green pivot marker is independent of diagnostics. It is depth tested per
fragment: visible parts are opaque and occluded parts show through faintly.
