# THETA X streaming tools (libuvc-theta + GStreamer)

Minimal tools to stream **RICOH THETA X** over USB (H.264 over UVC) with **low latency**.
The camera is accessed via **Ricoh’s patched libuvc (`libuvc-theta`)**, and H.264 frames are fed into GStreamer via `appsrc`.

This repository is intended for **research and prototyping** use cases where:
- latency matters,
- fine control over buffering is required,
- integration with OpenCV / Python pipelines is needed,
- external synchronization (e.g. Vicon, UDP streams) is involved.

---

## What this repo is (and is not)

### ✔ What it is
- Uses **libuvc-theta** directly to open the THETA X
- Receives **H.264 NAL units** via libuvc callbacks
- Pushes frames into **custom GStreamer pipelines** via `appsrc`
- Outputs decoded **BGR frames over shared memory** (for OpenCV, Python, etc.)

### ✘ What it is NOT
- This repo does **not** use the `gstthetauvc` GStreamer plugin
- This repo does **not** create `/dev/video*` devices
- This repo does **not** bundle libuvc-theta itself

This design choice gives lower latency and more control than plugin-based approaches.

---

## Tools

### `min_latency_from_uvc`
Ultra–low-latency pipeline:

```
THETA X (H.264 over UVC)
 → libuvc callback
 → appsrc
 → h264parse
 → decoder
 → videoconvert
 → BGR frames
 → shmsink (/tmp/theta_bgr.sock)
```

Intended for:
- OpenCV / Python consumers
- Real-time processing
- Research instrumentation

---

### `gst_viewer_vicon`
Live viewer + recorder:
- GStreamer preview
- MP4 recording
- Optional UDP logging (e.g. Vicon per-frame timestamps)

---

## Requirements

Tested on **Ubuntu 20.04 / 22.04**.

### 1) Install Ricoh’s patched libuvc

```bash
git clone https://github.com/ricohapi/libuvc-theta.git
cd libuvc-theta
mkdir build && cd build
cmake ..
make
sudo make install
```

---

### 2) Install dependencies

```bash
sudo apt-get install \
  libusb-1.0-0-dev \
  libjpeg-dev \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  gstreamer1.0-tools
```

---

## Build

```bash
make
```

---

## Run

### Low-latency shared memory stream

```bash
./min_latency_from_uvc
```

Shared memory socket:
```
/tmp/theta_bgr.sock
```

---

## Attribution

Based on:
- Ricoh API: https://github.com/ricohapi/libuvc-theta
- Ricoh samples: https://github.com/ricohapi/libuvc-theta-sample
- thetauvc helper by K. Takeo (BSD-style license)
