# OpenAxis integration (3.0 alpha)

The integration targets 3.0.0-alpha11's shared scene architecture. It currently
controls the plater camera, with perspective/orthographic projection, native
mouse reconciliation, focus reporting, scene/selection bounds, surface picking
and a transient pivot indicator. It does not advertise object editing yet.

## Build

```powershell
cmake --preset default -DSLIC3R_OPENAXIS=ON `
  -DCMAKE_PREFIX_PATH=C:/path/to/PrusaSlicer/dependencies
cmake --build build-default --config Release
```

With OpenAxis enabled, CMake 3.24 or newer and Git are required. CMake FetchContent
downloads `cpp/v1.0.0-rc.1` from `https://github.com/rotatrix/openaxis.git` into the
build directory and builds the C++ SDK with the application. No submodule
initialization is needed. See the [SDK installation guide](https://openaxis.rotatrix.com/guide/sdk-installation/).

For SDK development, add `-DOPENAXIS_SOURCE_DIR=C:/path/to/openaxis` to use an
existing checkout instead of fetching the release. Keep local absolute paths in
ignored `CMakeUserPresets.json`, not in tracked presets. If an existing build cache
still points at the old submodule, pass `-DOPENAXIS_SOURCE_DIR=` to use FetchContent.
The SDK supplies the adapter-based C++ API, including `OpenAxisClient` and
`OpenAxisConnectionManager`. This build uses the SDK under its GPLv3 license option.
The feature is optional and defaults off. It requires a native GUI build. Initial
configuration needs network access to fetch the SDK and any missing dependencies.

## Runtime

Start Rotatrix and use a profile matching `app.prusaslicer`; the connection also
reports `workspace.modeling`, the target PID, and application navigation focus. No device
axis mapping or navigation policy lives in PrusaSlicer. A native wx scheduler
dispatches SDK work through deferred UI callbacks and a one-shot timer for the
next deadline. There is no recurring integration polling timer. Camera writes request a
redraw and update the trackball's bookkeeping. Switching to preview stops the
plater connection; returning reconnects. Modal dialogs and focus loss cancel
camera control. All scene operations execute on the UI thread. Native camera
listeners notify the SDK after application input; adapter writes suppress that
notification because they already supply readback. Existing UI events refresh
focus, cursor and context. The scheduler outlives the client/session, and queued
callbacks are invalidated during shutdown.

The controller implements `NavigationAdapter` and creates a `NavigationCapture`
for each query, pinning the viewport identity and initial camera observation.
`NavigationSession` attaches directly to `OpenAxisClient`;
`OpenAxisConnectionManager` owns retries and replays tags, capabilities and focus
on reconnect. The SDK owns gesture state, comparison and reconciliation. The
adapter reads the camera actually realized by PrusaSlicer after a write, including
native zoom limits, so corrections report the host result. Perspective uses
vertical field of view in radians; orthographic uses full vertical extent in
model units. The world is right-handed, +Z up and +Y forward. Native mouse input
continues from the latest applied pose through the trackball synchronization hook.

A missing or zero-size viewport is unavailable. Project/view replacement, resize
and DPI changes invalidate the gesture and retained graphical evidence. Modal
dialogs and application deactivation cancel the current gesture. Side-panel
keyboard focus does not disable navigation. A cursor over a panel or outside the
viewport makes cursor facts unavailable, allowing Rotatrix's configured center
pivot fallback; it does not release navigation focus.
No facts, bounds or picks are queried asynchronously: query callbacks run to
completion on the UI thread against the current scene. Selection-filtered and
ordinary picks reuse the native nearest-hit picking path; unavailable optional
facts remain unavailable rather than using reference-app defaults.

Print-bed plates and models participate in surface picks. Selection-filtered
picks respect native bed selection as well as object selection, so an active bed
under the cursor can anchor navigation before a selected object at screen center.
Unselected beds remain eligible for ordinary picks. Bed axes, labels and virtual
placement previews are excluded. Beds do not contribute to model/selection bounds.

The companion Rotatrix default configuration includes a `prusaslicer` profile.
For an existing configuration that has not picked up new defaults, import
`doc/OpenAxis.rotatrix.yaml` into a profile using **Edit as YAML**. Its controls
reuse the shared `orbit_momentary` template.

## Diagnostics

In the plater, open **Menu > Help > OpenAxis Diagnostics...** (or **Help** in
the native menu bar). The non-modal panel shows connection (including retry
countdown and errors), navigation focus and gesture status, a camera-write count,
SDK query evidence, corrections and transport errors. **Enable OpenAxis
navigation** starts/stops the connection and clears the active gesture when
disabled. This control and the diagnostic display options are session-only.
It works while Rotatrix is unavailable, so connection failures can be inspected.

The log retains the latest 250 entries, with elapsed timestamps and a 4096-character
limit per entry. **Copy log** copies the retained text; **Clear log** clears it.
**Pause logging** freezes collection without stopping navigation. Enable
**Include camera writes** for per-pose events; these are off by default to preserve
queries and errors. **Auto-scroll** follows incoming entries and can be disabled
to inspect older events. Closing the panel leaves collection active. Preview
hides the panel and stops the plater connection; returning restores its visibility.

The adapter passes the SDK's shared `NavigationDiagnostics` collector to
`NavigationSession`, uses its logger for the panel and renders its presentation
through **Viewport diagnostics**. Text is a transparent bottom-left viewport
overlay, inset 120 logical pixels from the left to leave room for the viewcube
and 24 pixels from the bottom. Rows,
semantic colors, segment widths and multiline crosshair labels come directly
from the SDK. The low-level session callback only counts camera writes.

Pick evidence records the sampled logical pixel and production ray/hit during
the original fact query, without another raycast. Screen crosshairs stay at the
sampled pixel; world rays and bounds reproject through the current camera during
both OpenAxis and native navigation. Geometry is clipped before perspective
division, confined to its viewport and invalidated with its captured context.
A one-shot refresh at the collector's next expiry updates retained status even
without new input. Logging remains independent of overlay visibility.

## Pivot feedback

The pivot follows the shared [visual feedback guidance](https://rotatrix.github.io/openaxis/experience/pivots-diagnostics/):
an unlit lime-green disc of 4 logical pixels radius, a black annulus out to 5.5
pixels, and 32 segments. It scales with DPI once and keeps constant apparent size
in both projections. Points behind the camera or outside its clip volume are hidden.

The marker draws against document depth before PrusaSlicer's on-top and gizmo
layers overwrite or clear it. Complementary depth passes make visible fragments
opaque and occluded fragments 25% opaque, without depth writes. Fill and rim use
separate geometry to avoid double blending. The draw restores graphics state and
does not insert scene nodes, affect picking/bounds, or enter undo/saved content.
It remains independent of diagnostic visibility and clears with the SDK pivot
callback on gesture end, cancellation, disconnect or view deactivation.

## Verification

Build the integration with the source override above. Focused viewport tests run
without the application's dependency build:

```powershell
cmake -S tests/openaxis -B build-default/openaxis-checks
cmake --build build-default/openaxis-checks --config Release
ctest --test-dir build-default/openaxis-checks -C Release --output-on-failure
```

These check offset and portrait viewports, fractional/high DPI, clip rejection
and segments crossing the near/eye planes. The shared SDK has separate protocol,
session and diagnostic fixture tests. Native acceptance still includes physical
device navigation, mouse continuation, zoom limits, reconnect, focus/modal/view
changes, and visual checks in both projections and at different display scales.
For pivot depth, inspect fully visible/occluded points and a disc intersecting a
surface; repeat with shadows and ambient occlusion enabled and disabled. Verify
cursor/center diagnostic samples, native-camera reprojection and cleanup after
turning overlays off. Automated projection checks do not replace these GPU/input
checks. Windows is the current validation target; macOS/Linux need native runs.
