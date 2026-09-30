# OpenAxis integration assessment (PrusaSlicer 2.9.6)

Status: **scope decisions approved; implementation not started.** This assessment
reviews the existing 2.9.6 backport against the OpenAxis documentation and
PrusaSlicer 2.9.6's native `GLCanvas3D`/`Camera` APIs. It records the approved
scope and the changes the current code needs to meet it.

## Baselines

| Item | Value |
| --- | --- |
| Application baseline | Upstream release tag `version_2.9.6`; work branch `rotatrix/work/version_2.9.6` at `1a2e919b24` (7 downstream commits) |
| SDK pinned by the build | `cpp/v1.0.0-rc.1` via FetchContent (`cmake/OpenAxis.cmake:22`) |
| SDK pin | Approved as is |
| SDK docs assessed | `rotatrix/openaxis-dev` at `4621660` (2026-09-29, includes the revised settings guidance) |


Doc references are relative to `$OPENAXIS/docs/src/content/docs/`. Source references
are relative to this repository; `GUI/` means `src/slic3r/GUI/`.

## Approved scope decisions

| Decision | Approved choice |
| --- | --- |
| Maintained line | 2.9.6 (the 3.0 alpha branch is exploratory only) |
| Editors | Full navigation in the 3D view **and** the G-code preview, including the standalone G-code viewer |
| Object manipulation | Deferred for this implementation |
| Free camera | Not supported |
| Native mouse orbit | Unchanged by the integration |
| Projection | Report the native projection; a Rotatrix pose never switches it |
| Controls | Always connected; no enable/connect UI; a diagnostics visibility toggle that is not persisted (`recommendations/settings-and-controls.md`) |
| Beds and selection | Beds are never selection candidates; selection-filtered facts use object selection only |

## Coverage table

### Editors and navigation modes

| Capability | SDK requirement or recommendation | Native API | Proposed support | Limitation or open question | Validation method |
| --- | --- | --- | --- | --- | --- |
| 3D view orbit, pan, zoom | Rec: support where users inspect models (`getting-started/planning.md` "What should work") | Shared `Plater::priv::camera` (`GUI/Plater.cpp:337`, `get_camera()` `:7626`); `Camera::look_at` (`GUI/Camera.cpp:544`), `set_zoom` (`:81`) | **Included** (implemented) | See the write and pivot rows for required changes | Manual: direction, scale, no drift/flicker in perspective and orthographic (`guide/validation.md` "Test in the application") |
| G-code preview navigation | Approved scope; Rec as above | Preview `GLCanvas3D` uses the same shared `Camera`; toolpaths are rendered by `GCodeViewer` (`GUI/GCodeViewer.hpp`) | **Included**. Camera navigation already runs on the preview canvas, but picking and bounds are incomplete (next rows) | Toolpaths have no mesh raycasters, so today preview picks never hit anything | Orbit, pan and zoom in preview; switch views mid-gesture |
| Standalone G-code viewer mode | Same | Same canvas and `GCodeViewer` in G-code viewer mode (`GUI_App::is_gcode_viewer`) | **Included** (follows preview) | Tags and focus must hold without a plater model | Open a `.gcode` file in viewer mode |
| Navigation during plater gizmos (move, rotate, scale, cut, paint, measure, emboss, SVG, simplify, SLA) | Req: queries and writes use the captured target (`guide/validation.md` "Implementation") | `GLGizmosManager`; native camera input remains available while gizmos are active (`GUI/GLCanvas3D.cpp:4112`, `any_gizmo_active`) | **Included** (camera only) | Picks ignore gizmo clipping planes (see picking). Gizmo raycasters are already excluded | Navigate during each gizmo; check that handles, paint cursor and cut plane stay aligned |
| Object manipulation | Rec: needs accept, cancel, undo (`guide/object-manipulation.mdx`) | Move/rotate gizmos and undo/redo stack | **Deferred** (approved) | No object interaction tags are published | Confirm that only camera tags are published |
| Free camera | Rec: fits moving through a scene (`guide/free-camera.mdx`) | None. 2.9's "Use free camera" preference is an unconstrained **trackball**, not free flight | **Inapplicable** (approved) | None | Confirm that `navigation.hint.free_camera` is never published |
| 2D pan/zoom, Axis Streaming | `guide/2d-navigation.mdx`, `getting-started/axis-streaming.mdx` | No camera-driven 2D editor | **Inapplicable** | None | N/A |

### Camera facts and native input

| Capability | SDK requirement or recommendation | Native API | Proposed support | Limitation or open question | Validation method |
| --- | --- | --- | --- | --- | --- |
| `camera.pose` read | Req: correct axes, projection, aspect (`concepts/coordinates.md` "Camera axes", "Projection describes the rendered view") | `Camera::get_dir_right/up/forward`, `get_position`, `get_projection_matrix`. The perspective frustum is sized at orbit distance: half-height `0.5·vp_h/zoom` at `m_distance` (`GUI/Camera.cpp:204-244`) | **Included** (`GUI/OpenAxisController.cpp:197-213`). `fov = 2·atan(1/P11)` and `ortho_extent = 2/P11` are correct for this projection | The frustum near/far planes are fitted to the scene box on every render (`calc_tight_frustrum_zs_around`) | Diagnostics: compare the reported fov and extent with the rendered view in tall and wide windows |
| Pose write | Req: return the actual pose; separate failure from unknown readback (`reference/navigation-hosts.mdx` "Writes and observations") | `look_at` sets target and `m_distance`; `set_zoom` clamps to scene-relative min/max (`GUI/Camera.cpp:81-90`) | **Change required.** The write currently places the native target at the **Rotatrix pivot distance** (`OpenAxisController.cpp:230-231`), which moves the orbit centre native mouse input uses. Proposed: keep the native `get_distance()` so mouse orbit is unchanged | Zoom clamps surface as "differs" corrections, which is intended | Zoom to each limit; after a gesture, a mouse orbit must rotate about the same target as before it |
| Projection | Approved: never switch. `Camera::set_type` also **persists** `use_perspective_camera` to app config (`GUI/Camera.cpp:40-46`) | `Camera::get_type` | **Change required.** Today the write calls `set_type(...)` from the server pose (`OpenAxisController.cpp:225`). Proposed: never call `set_type`; apply position and orientation, derive zoom from the pose field matching the native projection, and return the realized pose so the SDK rebases any mismatch (`concepts/reconciliation.md` "Projection changes") | If the server keeps sending the other projection after a rebase, the pose will show "differs" repeatedly. **Validate** that the server adopts the reported projection | Toggle projection natively (K) mid-gesture; Rotatrix never changes the preference |
| Native camera bookkeeping | Fork: mouse orbit unchanged | `Camera::rotate_on_sphere` clamps mouse tilt to �90� using stored `m_zenit` (`GUI/Camera.cpp:354-366`). `look_at` recomputes it from the actual view via `update_zenit()` (`GUI/Camera.cpp:578`, `:603`). Constrained mouse rotation calls `recover_from_free_camera()` (`GUI/GLCanvas3D.cpp:4123`), which removes roll | **Included**; no change needed. Rotatrix writes go through `look_at`, so native tilt limits follow the real view | Removing roll is native behaviour (upstream GH #3816, shared with 3Dconnexion). The first constrained mouse drag after a rolled Rotatrix view levels the camera; this is left unchanged | Tilt with Rotatrix, then mouse-drag to the top view; the limit must stop at straight down |
| `world.orientation` | Req (`concepts/coordinates.md`) | Bed frame, +Z up, right-handed; `Camera::select_view("front")` | **Included** (`OpenAxisController.cpp:248`) | Verify that "forward +Y" matches the Front view semantics | Rotatrix front view against View > Front |
| `viewport.aspect`, `viewport.cursor` | Req: normalized cursor, unavailable outside, not clamped (`concepts/coordinates.md` "Screen coordinates") | `get_canvas_size()`; `ScreenToClient(wxGetMousePosition())` with Retina scale (`GUI/GLCanvas3D.cpp:1372-1388`); ImGui `WantCaptureMouse` | **Included** (`OpenAxisController.cpp:250-253`) | **Open:** `wxGetMousePosition` is unreliable on Wayland; consider using the canvas's last mouse-motion position (`m_mouse.position`) | Cursor at the corners, over ImGui panels, outside; Windows, macOS Retina, Linux X11 and Wayland |
| `camera.view_target`, `document.id` | Optional | `Camera::get_target`; `Model::id()` | **Included** | `document.id` has no meaning in G-code viewer mode; report unavailable there | Diagnostics rows |
| Native input reconciliation | Req: notify native camera changes (`guide/concurrent-input.mdx`); poll when no event exists (`navigation-hosts.mdx` "Application events") | No camera listener in 2.9. The view matrix, zoom and type are compared on render and before each dispatch (`OpenAxisController.cpp:97-130`) | **Included** (polling on existing events) | Native changes appear only on the next render or SDK dispatch, which is acceptable because camera input triggers a render. The 3Dconnexion `Mouse3DController::apply` (`GUI/Mouse3DController.hpp:213`) is a third input source and is detected the same way | Mouse and Rotatrix together; release while moving; SpaceMouse during a gesture |

### Picking and bounds

| Capability | SDK requirement or recommendation | Native API | Proposed support | Limitation or open question | Validation method |
| --- | --- | --- | --- | --- | --- |
| Plater picks (`pick.cursor`, `pick.viewport_center`) | Req: pick only when requested; misses return only `markerPosition` (`guide/picking-pivots.mdx` "Query handling"; `reference/diagnostics.md` "Pick evidence") | `SceneRaycaster` volume and bed items; `MeshRaycaster::closest_hit` (`GUI/MeshUtils.cpp:531`) | **Included** (`OpenAxisController.cpp:263-296`): nearest front-facing hit, back faces honoured per item, gizmo raycasters excluded | **Change required:** `closest_hit` is called with no clipping plane, so clipped-away geometry in cut, paint and SLA gizmos can win. Pass the active gizmo or camera clipping plane (`GLGizmosManager::get_clipping_plane`, `GUI/Gizmos/GLGizmosManager.cpp:311`) | Hits and misses over objects, beds and empty space; cursor outside; picks inside a clipped cut or paint view |
| `.selection` picks | Req: restrict to selected geometry; unselected geometry does not occlude (`picking-pivots.mdx` "Choosing a native picking operation") | `Selection::get_volume_idxs` | **Included**. Beds are already excluded from selection picks (`OpenAxisController.cpp:287-288`), matching the approved decision | None | Select an object behind another |
| Bed picks | Approved: ordinary picks only | Bed raycaster, translated per `s_multiple_beds` bed | **Included** when the camera looks downward (`OpenAxisController.cpp:288`), matching native mouse behaviour | None | Pick the bed from above and below |
| Preview picks on toolpaths | Req: world-space surface hit, or unavailable for server fallback | `GCodeViewer`/libvgcode has no ray-hit API. The depth buffer of the rendered preview is available | **Proposed:** pick preview toolpaths by reading scene depth at the sample pixel and unprojecting through the captured camera. It uses the actual rendered geometry, including layer-range and visibility filters. Selection variants are unavailable in preview | Needs the canvas GL context current during the query, and a depth read that reflects the current camera (not stale). **Investigate** a small native experiment before committing to it; the fallback is bounds and the viewport centre only | Pick over visible toolpaths, hidden layers (must miss) and empty space; compare with the diagnostics crosshair |
| Hit `bounds` | Req: bounds of the hit object or body (`picking-pivots.mdx`) | `GLVolume::transformed_bounding_box` (volume = part) | **Included** at volume/part granularity (approved). Preview depth picks have no hit bounds | None | Multi-part object: compare the drawn bounds |
| `model.bounds` | Req: world-space scene bounds | Active, enabled `GLVolume`s (`OpenAxisController.cpp:256-262`) | **Change required for preview:** use `GCodeViewer::get_paths_bounding_box()` (`GUI/GCodeViewer.hpp:305`) there. Plater keeps volumes | Beds are excluded (empty plate is unavailable); multiple beds are merged | Empty plate; several beds; preview with a layer range |
| `selection.bounds` | Req: selected geometry bounds | Selected volume indices | **Included** in the 3D view; **unavailable** in preview | None | Selection versus none |

### Pivot, diagnostics and controls

| Capability | SDK requirement or recommendation | Native API | Proposed support | Limitation or open question | Validation method |
| --- | --- | --- | --- | --- | --- |
| Pivot marker | Fork: initial deliverable; Rec: 4 px lime disc, 5.5 px black rim, ≥32 segments, **per-fragment** depth test, about 20–25% show-through, no depth write, never fade the whole marker by its centre (`recommendations/pivot-appearance.md`) | Legacy GL render pass in `GLCanvas3D::render` after transparent objects (`GUI/GLCanvas3D.cpp:2215`); `GLModel` and the flat shader | **Change required.** It is currently an ImGui overlay whose opacity comes from a single `glReadPixels` at the centre (`OpenAxisController.cpp:299-312`), which the recommendation forbids. It also stalls the pipeline. Proposed: draw the fill and annulus as a screen-facing `GLModel` with complementary `GL_LEQUAL`/`GL_GREATER` passes, then restore GL state | Must draw before gizmo and overlay passes that clear depth. Preview depth also includes toolpaths, which is correct | Surface-crossing, fully visible and fully occluded markers in 3D view and preview; DPI 100/150/200% |
| Pivot lifetime | Req: clear on cancel, disconnect, `motion_end`, target replacement, unload | SDK `show_pivot(nullopt)` plus the context check | **Included** | Currently the marker only displays the pivot; with the write change it no longer affects native orbit | Gesture end; disconnect mid-gesture; view switch |
| Viewport diagnostics | Fork: initial deliverable; Req: rows, tones, crosshairs, segment opacity, context filtering, expiry, no pick rays (`reference/diagnostic-rendering.md`) | ImGui background draw list (`OpenAxisController.cpp:313-373`) | **Included**; matches the crosshair geometry and opacity. **Change:** add connection status (state, retry) as a viewport row, because the settings guidance places status in the diagnostic presentation | Text is offset 120 px for the view cube | `diagnostic-rendering.md` "Acceptance check"; diagnostics on/off gives identical navigation |
| Diagnostics toggle | Rec: in the host's usual place for view options; not persisted (approved) | View menu (`GUI/MainFrame.cpp:1729-1734`, next to "Show Labels") | **Change required.** It is currently a Help-menu item (`GUI/MainFrame.cpp:1342`) toggling a per-canvas flag and a small panel. Proposed: a checkable **View › OpenAxis Diagnostics** item with one session-only flag shared by both canvases; remove the panel | None | Toggle in the 3D view, switch to preview (state carries over), restart (off) |
| Enable/connect control | Approved: none; always connected | — | **Change required:** remove the panel's "Enable navigation" checkbox (`OpenAxisController.cpp:135-138`) and `m_enabled` | None | Start before and after Rotatrix; restart Rotatrix |
| Session logs, version metadata | Rec: configure with client version; close after the session (`guide/session-logs.mdx`); send `client_version`/`target.app_version` (`reference/language-support.mdx` "Connection version metadata") | `SLIC3R_VERSION` | **Change required:** `DiagnosticLog::configure("prusaslicer")` (`OpenAxisController.cpp:23`) passes no version, and nothing closes the logger. Add both versions and close at shutdown | None | Session file header in `%LOCALAPPDATA%/Rotatrix/logs` |

### Threading and lifecycle

| Capability | SDK requirement or recommendation | Native API | Proposed support | Limitation or open question | Validation method |
| --- | --- | --- | --- | --- | --- |
| One client per integration | Req: keep one client, one connection manager and at most one `NavigationSession` together (`reference/connection-lifecycle.mdx` "Responsibilities") | Each `GLCanvas3D` owns its own `OpenAxisController` (`GUI/GLCanvas3D.hpp`, `m_openaxis`), so the 3D view and preview open **two** connections | **Change required.** Proposed: one controller owned by the plater, created after the main frame exists; the current canvas is part of the captured context | Rotatrix currently sees two same-PID clients, one of them unfocused | One connection in Rotatrix across view switches and reconnects |
| Scheduler | Req: deferred, thread-safe, never inline (`navigation-hosts.mdx` "Scheduler contract") | `CallAfter` plus a one-shot `wxTimer` (`GUI/OpenAxisScheduler.hpp`) | **Included** | None | SDK-level; app unfocused |
| Context capture and invalidation | Req: replacing the target invalidates; never redirect (`guide/validation.md` "Target") | Key: model id, scene revision, canvas address, active bed, size, DPI (`OpenAxisController.cpp:151-158`). The revision increments in `reset_volumes` and `reload_scene` | **Included**; the key gains the view (3D or preview) | `reload_scene` runs on many model and G-code updates, so gestures may cancel during background slicing or G-code loading. Accepted as approved | Navigate while slicing finishes and while the preview reloads |
| Focus and modal dialogs | Req: announce focus (`guide/dynamic-tags.mdx` "Report focus") | `wxTheApp->IsActive`, `IsShownOnScreen`, current canvas (`GUI/GLCanvas3D.cpp:1372-1388`) | **Change required:** add modal-dialog detection. Modal dialogs belong to the active app, so focus currently stays true | None | Open the preferences and print-settings dialogs mid-gesture |
| Tags | Rec: tags describe what the app is doing (`guide/dynamic-tags.mdx`) | View switch (`Plater::select_view_3D`) | **Change:** publish only `app.prusaslicer` in every view. Workspace tags have no standardized definition, so drop the invented `workspace.modeling` (`OpenAxisController.cpp:48`) | The profile trigger uses `tag:app.prusaslicer` only (`doc/OpenAxis.rotatrix.yaml`), so nothing depends on the dropped tag | Tags in Rotatrix when switching views |
| Shutdown | Req: stop producers, stop the connection, close the session while native resources exist (`connection-lifecycle.mdx` "Shutdown order") | `~GLCanvas3D` resets the controller (`GUI/GLCanvas3D.cpp:1358-1362`) | **Included**; the order moves with the single controller | Also close the logger | Close the app mid-gesture and during retry |
| Platforms | Req: CI then native tests (`planning.md` "How will you check") | CI covers Windows x64, macOS ARM64 and Ubuntu 24.04 | **Included** in CI | Native validation is still pending on every OS | Physical-device checklist per OS |

## Implementation summary

In priority order:

1. One controller per application; two-view context; `app.prusaslicer` as the only tag.
2. The camera write keeps the native orbit distance and never calls `set_type`.
3. A depth-tested GL pivot marker.
4. Preview bounds from `GCodeViewer`; preview surface picks by depth readback (after a native experiment).
5. Clipping-plane-aware picks.
6. View-menu diagnostics toggle; remove the panel and enable checkbox; connection status as a viewport row.
7. Modal-dialog focus; logger version and close.

## Open questions

- Wayland cursor position (native check on Linux).
