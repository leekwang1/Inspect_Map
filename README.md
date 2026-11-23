# InspectMap bootstrap

Build with Qt 6.10.3 MSVC x64, OpenCV C++ and CMake. Set `OpenCV_ROOT` to a
matching MSVC x64 OpenCV installation containing `include`, `lib`, `bin`,
`debug/lib`, and `debug/bin`. For example, pass `-DOpenCV_ROOT=<installation>`
to CMake. The Project Manager follows the
phase-5 GUI outline with project selection, work status, facility/inspection,
pipeline, and artifact/history views. It supports:

- Create or open a project (`project.sqlite` in each project directory).
- Open a recent project or reconnect its moved database path.
- Create and edit facilities and inspections, then select their working scope.
- Open the five processing programs from the selected project. The manager
  passes `--project <project.sqlite>` and, when selected, `--facility <id>` and
  `--inspection <id>`. Programs remain independently runnable.
- Edit each module's AUTO/REVIEW/MANUAL/SKIP pipeline setting and inspect
  stored run and artifact records.

The full-run control is visible but disabled until InspectionWorker and the
processing engines implement the shared pipeline contract. It does not claim
to process data or create run records today. Backup/restore and database repair
remain deferred. A program whose executable is not yet installed reports that
clearly when selected. Existing
database tables and core APIs remain for compatibility with projects already
created and for later processing programs. Opening an older project does not
trigger a migration merely to display it.

Run `ctest --test-dir build -C Debug --output-on-failure` to verify Qt SQLite
loading and basic project data persistence. `InspectionCli` provides the
existing command-line smoke test and basic project commands.

## Data Converter (first working slice)

`DataConverter` opens independently or from Project Manager. Device-specific
scanners now implement a common `ScanResult` contract. The Elios adapter reads
snapshot JSON for image/initial-pose links and map JSON for LAS references.
The generic image-folder adapter also accepts DJI photos, but currently reads
pixels only: DJI EXIF/XMP, flight logs, camera metadata and pose are not yet
interpreted. AUTO chooses Elios by its directory signature and otherwise an
image folder when images exist. The legacy `DJI_IMAGE_FOLDER` adapter name is
still accepted for CLI compatibility. Elios pose and LAS remain review inputs,
not verified model-aligned data. No SfM processing runs in DataConverter.

Raw images are copied to `source/images/<dataset-id>/`; LAS remains an external
reference. The optional, adapter-independent correction command stores its profile by SHA-256 in
`source/calibration/` and corrected JPEGs in
`derived/images/device_undistorted/<dataset-id>/<profile-hash>/`. SQLite keeps
RAW and DEVICE_UNDISTORTED image variants and their exact file paths. It does
not store JPEG blobs. The correction command can run again without rerunning
the import; using a different profile produces a separate output directory.

Correction runs in C++ using OpenCV fisheye K/D/P maps and writes JPEG quality
95 with source EXIF orientation preserved. No Python runtime is required.
Select a camera-matching fisheye K/D/P JSON explicitly in the GUI. The
640x400 VIO YAML is not suitable for 4000x3000 Elios photos. The engine
preserves raw-sensor pixel coordinates and source EXIF orientation; a future
AI request must use that exact image variant and orientation policy. Import
does not itself submit images to the AI service.

Run `InspectionCli convert-scan <source-directory> AUTO` for a read-only scan,
or select `IMAGE_FOLDER` explicitly for an image-only folder.
`InspectionCli convert-correct <project.sqlite> <dataset-id> <profile.json>`
runs the same correction engine without the GUI. `converter_smoke` tests import,
correction and image-variant registration.

## Registration Tool (first working slice)

`RegistrationTool` opens from Project Manager or independently. Load a directory
containing `model/<part>/<part>.obj` (or select `model/` itself), then select a
LAS cloud from the project or a file. Each OBJ's `meta.gxxml` scale, translation
and rotation are applied before registration. When metadata is absent, choose
the fallback model unit (mm or m) before loading. The adjacent `map/` directory
is detected but DXF-to-face mapping is not implemented in this slice.

For manual registration, select corresponding LiDAR and model points in the two
VTK views and compute a rigid transform from at least three non-collinear pairs;
four or more widely separated pairs are recommended. The optional similarity
mode estimates one uniform scale for unscaled SfM reconstructions. ICP can
refine this transform. Without a manual result, ICP starts by matching centroids
and is only a candidate, not reliable global registration for partial or
axis-misaligned scans. Review the surface RMSE and fraction of sampled cloud
points within 0.5 m of the model before accepting it.

Accepted transforms are versioned in the project's `registration_result` table.
The original LAS and `INITIAL` camera poses remain unchanged. `MODEL_ALIGNED`
camera poses are written only when the selected LAS belongs to a project dataset
and the operator confirms the camera poses share the LAS coordinate frame.
The current viewer reads uncompressed LAS; PLY and other SfM point-cloud formats
are still to be added. `registration_engine` tests rigid/similarity fitting,
camera orientation, ICP and the sample model's unit transform.
