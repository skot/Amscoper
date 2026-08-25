# AmScope Microscope

A lightweight Linux desktop application for viewing and capturing images from
an AmScope DM756-U830 USB microscope.

The app discovers the microscope through Video4Linux rather than relying on a
fixed `/dev/videoN` number. It provides an embedded live preview, resolution
selection, automatic USB reconnection, image rotation and mirroring, and
full-resolution JPEG capture.

## Features

- Automatically finds the DM756-U830 among connected V4L2 devices.
- Reconnects after the microscope is unplugged and reattached.
- Displays the live feed in a native GTK window.
- Reads the available MJPEG resolutions directly from the camera.
- Rotates the displayed image clockwise in 90-degree steps.
- Horizontally mirrors the displayed image.
- Saves the current view as a timestamped, full-resolution JPEG.
- Opens the capture directory from inside the app.
- Integrates with Linux application launchers through a desktop entry.

Rotation and mirroring are included in saved images exactly as displayed.
Preview transforms are performed on a scaled render, while the full-resolution
frame is transformed only when a JPEG is saved. The camera continues streaming
at its configured frame rate.

## Supported hardware

The scanner currently identifies this camera model:

```text
AmScope DM756-U830
USB ID aa47:830e
```

The tested camera advertises these MJPEG modes at 30 fps:

- 3840×2160
- 1920×1080
- 1280×720
- 960×540

The application defaults to 1920×1080 for a responsive live view.

## Requirements

The build uses:

- A C++20 compiler
- GNU Make
- GTK 3
- OpenCV 5 with `core`, `imgproc`, `imgcodecs`, and `videoio`
- Video4Linux 2 headers
- `pkg-config`
- `update-desktop-database` for launcher installation

On Arch Linux and Omarchy, the relevant packages are generally provided by
`base-devel`, `gtk3`, `opencv`, `v4l-utils`, and `desktop-file-utils`.

## Build

```sh
make
```

The executable is written to `build/amscope-app`.

A CMake project is also included for environments where CMake and OpenCV's
CMake metadata are available:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Install for the current user

```sh
make install-user
```

This installs:

```text
~/.local/bin/amscope-app
~/.local/bin/amscope-launcher
~/.local/share/applications/com.skot.AmScope.desktop
```

Open the desktop application launcher and search for **AmScope Microscope**.
The `amscope-launcher` command remains as a compatibility wrapper around the
native application.

## Controls

| Control | Shortcut | Action |
| --- | --- | --- |
| Capture JPG | `Space` or `Ctrl+S` | Save the current full-resolution view. |
| Rotate 90° | `R` | Rotate clockwise through 0°, 90°, 180°, and 270°. |
| Mirror | `M` | Toggle horizontal mirroring. |
| Resolution | — | Reopen the camera at another supported size. |
| Open Captures | — | Open the capture directory in the file manager. |
| Reconnect | — | Immediately rescan USB video devices. |

Orientation settings apply for the current app session and do not alter the
microscope's hardware configuration.

## Captures

Images are saved under:

```text
~/Pictures/AmScope Captures/
```

Filenames include millisecond-resolution timestamps:

```text
DM756_2026-08-24_21-09-20-996.jpg
```

JPEG quality is set to 95. A 90° or 270° rotation swaps the saved width and
height, so a rotated 1920×1080 capture is saved as 1080×1920.

## Command line

Launch at the recommended resolution:

```sh
amscope-app --size 1920x1080
```

Launch at 4K:

```sh
amscope-app --size 3840x2160
```

Probe for the camera without opening the graphical interface:

```sh
amscope-app --probe
```

Example probe output:

```text
Device: /dev/video2
Sizes:
  1920x1080
  3840x2160
  1280x720
  960x540
```

## Troubleshooting

### The app is waiting for the microscope

- Confirm that the USB data plug is connected directly to the computer.
- If using the supplied split cable, connect its auxiliary plug to a powered
  USB port or the supplied adapter.
- Close MPV, qv4l2, browser camera tabs, or other applications that may already
  have the microscope open.
- Press **Reconnect** after closing another camera application.

### The preview is black

The DM756 ring light remains off until capture software opens the camera. With
the app running, flick the microscope's spring-loaded front switch to the right
several times to increase illumination. Also verify that the magnetic ring
light's electrical contacts are aligned.

### Permission is denied for `/dev/videoN`

Confirm that the desktop session has granted access to the camera device. On a
typical systemd/udev desktop, the active user receives access automatically.
Membership in the `video` group may be required on other distributions.

## Project layout

```text
.
├── CMakeLists.txt
├── Makefile
├── README.md
├── amscope-launcher
├── amscope-microscope.desktop
└── src/
    └── amscope-app.cpp
```

- `src/amscope-app.cpp` contains USB discovery, capture, rendering, and the GTK
  interface.
- `amscope-microscope.desktop` provides application-launcher integration.
- `amscope-launcher` preserves the command name used by the original MPV-based
  launcher.
