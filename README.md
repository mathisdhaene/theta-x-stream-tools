# THETA X streaming tools (libuvc-theta + GStreamer)

Low-latency **RICOH THETA X** streaming over **USB/UVC**.
`min_latency_from_uvc` opens the camera with `libuvc-theta`, receives H.264 frames, decodes them in GStreamer, and publishes BGR frames via shared memory.

## Important: this tool does not use camera IP

- Camera access is USB only (UVC via `libuvc-theta`)
- No camera Wi-Fi/Ethernet IP is configured in this repo
- Camera selection is done by USB vendor/product IDs

## Reproducible Setup

Tested on Ubuntu 20.04 / 22.04.

### 1) Install Ricoh patched `libuvc-theta`

```bash
git clone https://github.com/ricohapi/libuvc-theta.git
cd libuvc-theta
mkdir build && cd build
cmake ..
make
sudo make install
```

### 2) Install dependencies

```bash
sudo apt-get update
sudo apt-get install -y \
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

### 3) Verify THETA X USB ID

```bash
lsusb | grep -i theta
```

Expected for THETA X:

```text
ID 05ca:2717 Ricoh Co., Ltd RICOH THETA X
```

- Vendor ID: `0x05ca`
- Product ID: `0x2717`

### 4) Build this repo

```bash
make min_latency_from_uvc
```

### 5) Run

```bash
./min_latency_from_uvc
```

Shared memory output:

```text
/tmp/theta_bgr.sock
```

## How THETA X is detected

`src/thetauvc.c` filters USB devices using:

- `USBVID_RICOH 0x05ca`
- `USBPID_THETAX_UVC 0x2717`

If you are using a camera with a new/unlisted product ID, add it to the product filter in `src/thetauvc.c`, then rebuild.

## Troubleshooting `THETA not found`

1. Confirm camera appears in `lsusb` with `05ca:2717`.
2. Confirm `libuvc-theta` is installed (`sudo make install` from its build folder).
3. Rebuild this repo after any `thetauvc.c` change: `make veryclean && make`.
4. Ensure no other process is currently using the THETA UVC interface.

## Other target

- `gst_viewer_vicon`: viewer/recorder utility with optional UDP integration.

## Attribution

- Ricoh API: https://github.com/ricohapi/libuvc-theta
- Ricoh samples: https://github.com/ricohapi/libuvc-theta-sample
- thetauvc helper by K. Takeo (BSD-style license)
