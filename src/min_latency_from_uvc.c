// min_latency_from_uvc.c
// Ultra-low-latency THETA → GStreamer via libuvc/thetauvc.
// - Opens THETA with libuvc + thetauvc helper to negotiate H.264
// - Pipes H.264 bytes into GStreamer appsrc (Annex-B / byte-stream)
// - Decodes with avdec_h264 (CPU) or nvh264dec (--nvdec)
// - Uses leaky queue + appsink drop=true to always process the latest frame

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <libuvc/libuvc.h>
#include "thetauvc.h"   // local header in your repo (matches thetauvc.c)

static GMainLoop *g_loop = NULL;
static GstElement *g_pipeline = NULL;
static GstElement *g_appsrc  = NULL;

static gboolean  g_use_nvdec = FALSE;
static int       g_arg_fps   = 30;     // requested FPS for caps timing only
static int       g_arg_w     = 3840;   // requested H.264 mode (fallback to 1920x960)
static int       g_arg_h     = 1920;

static GTimer   *g_timer = NULL;
static guint64   g_frames = 0;
static guint64   g_last_report_ns = 0;

static guint64 now_monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (guint64)ts.tv_sec * 1000000000ull + (guint64)ts.tv_nsec;
}

static void on_sigint(int sig) {
  (void)sig;
  if (g_loop) g_main_loop_quit(g_loop);
}

static gboolean bus_log(GstBus *bus, GstMessage *msg, gpointer data) {
  (void)bus; (void)data;
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err = NULL; gchar *dbg = NULL;
      gst_message_parse_error(msg, &err, &dbg);
      g_printerr("ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
      if (dbg) g_printerr("  Debug: %s\n", dbg);
      g_clear_error(&err); g_free(dbg);
      if (g_loop) g_main_loop_quit(g_loop);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError *err = NULL; gchar *dbg = NULL;
      gst_message_parse_warning(msg, &err, &dbg);
      g_printerr("WARN  from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
      if (dbg) g_printerr("  Debug: %s\n", dbg);
      g_clear_error(&err); g_free(dbg);
      break;
    }
    default: break;
  }
  return TRUE;
}

static void build_pipeline(void) {
  const char *decoder = g_use_nvdec ? "nvh264dec" : "avdec_h264";

  gchar *pipeline_str = g_strdup_printf(
    "appsrc name=ap is-live=true block=true format=time "
      "caps=video/x-h264,stream-format=byte-stream,alignment=au ! "
    "queue max-size-buffers=4 leaky=no ! "
    "h264parse config-interval=-1 disable-passthrough=true ! "
    "video/x-h264,alignment=au,stream-format=avc ! "
    "%s ! "
    "videoconvert ! videoscale ! "
    "video/x-raw,format=BGR,width=3840,height=1920 ! "
    "queue max-size-buffers=1 leaky=downstream ! "
    "shmsink socket-path=/tmp/theta_bgr.sock shm-size=67108864 wait-for-connection=true sync=false",
    decoder
  );

  g_print("Pipeline:\n  %s\n", pipeline_str);

  GError *err = NULL;
  g_pipeline = gst_parse_launch(pipeline_str, &err);
  g_free(pipeline_str);
  if (!g_pipeline || err) g_error("Failed to create pipeline: %s", err ? err->message : "unknown");

  g_appsrc = gst_bin_get_by_name(GST_BIN(g_pipeline), "ap");
  g_object_set(g_appsrc, "stream-type", 0, "format", GST_FORMAT_TIME, NULL);

  GstBus *bus = gst_element_get_bus(g_pipeline);
  gst_bus_add_watch(bus, (GstBusFunc)bus_log, NULL);
  gst_object_unref(bus);
}






static void push_h264_to_gst(const uint8_t *data, size_t len) {
  if (!g_appsrc || !data || len == 0) return;

  GstBuffer *buf = gst_buffer_new_allocate(NULL, len, NULL);
  GstMapInfo map;
  gst_buffer_map(buf, &map, GST_MAP_WRITE);
  memcpy(map.data, data, len);
  gst_buffer_unmap(buf, &map);

  // Timestamp the buffer using a monotonic timer for stable cadence
  gdouble elapsed = g_timer ? g_timer_elapsed(g_timer, NULL) : 0.0;
  GST_BUFFER_PTS(buf) = (GstClockTime)(elapsed * GST_SECOND);
  GST_BUFFER_DTS(buf) = GST_CLOCK_TIME_NONE;

  GstFlowReturn ret;
  g_signal_emit_by_name(g_appsrc, "push-buffer", buf, &ret);
  gst_buffer_unref(buf);

  if (ret != GST_FLOW_OK) {
    g_printerr("push-buffer failed: %d\n", ret);
  }

  // Lightweight stats
  g_frames++;
  guint64 t = now_monotonic_ns();
  if (t - g_last_report_ns > 2ull * 1000000000ull) {
    g_print("Frames pushed: %llu\n", (unsigned long long)g_frames);
    g_last_report_ns = t;
  }
}

// libuvc callback: frame->data contains the H.264 NAL stream from THETA
static void uvc_frame_cb(uvc_frame_t *frame, void *user_ptr) {
  (void)user_ptr;
  push_h264_to_gst((const uint8_t*)frame->data, frame->data_bytes);
}

static void usage(const char *prog) {
  fprintf(stderr,
    "Usage: %s [--nvdec] [--fps N] [--w WIDTH] [--h HEIGHT]\n"
    "  --nvdec      : use NVIDIA NVDEC (nvh264dec) if available\n"
    "  --fps  N     : caps framerate for appsrc (default: 30)\n"
    "  --w    WIDTH : H.264 request to the camera (default: 3840)\n"
    "  --h    HEIGHT: H.264 request to the camera (default: 1920)\n",
    prog
  );
}

int main(int argc, char **argv) {
  // Parse very simple args
  for (int i = 1; i < argc; ++i) {
    if      (!strcmp(argv[i], "--nvdec")) g_use_nvdec = TRUE;
    else if (!strcmp(argv[i], "--fps") && i+1 < argc) g_arg_fps = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--w")   && i+1 < argc) g_arg_w   = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--h")   && i+1 < argc) g_arg_h   = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
    else {
      fprintf(stderr, "Unknown arg: %s\n", argv[i]);
      usage(argv[0]);
      return 1;
    }
  }

  signal(SIGINT, on_sigint);

  // Init GStreamer
  gst_init(&argc, &argv);
  g_timer = g_timer_new();
  g_last_report_ns = now_monotonic_ns();

  build_pipeline();

  if (gst_element_set_state(g_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_error("Failed to set pipeline to PLAYING");
  }

  // Init libuvc + THETA
  uvc_context_t *ctx = NULL;
  uvc_device_t  *dev = NULL;
  uvc_device_handle_t *devh = NULL;

  uvc_error_t res = uvc_init(&ctx, NULL);
  if (res != UVC_SUCCESS) g_error("uvc_init failed: %d", res);

  // Find/open first UVC device (you can filter by VID/PID if you prefer)
  res = uvc_find_device(ctx, &dev, 0, 0, NULL);
  if (res != UVC_SUCCESS || !dev) g_error("THETA not found via UVC");
  res = uvc_open(dev, &devh);
  if (res != UVC_SUCCESS) g_error("uvc_open failed: %d", res);

  // Negotiate an H.264 streaming profile with thetauvc helper.
  // Many thetauvc builds use a 'formatId' 0x00 for H.264; adjust if your header defines a macro.
	// --- replace the whole "Negotiate an H.264 streaming profile..." block with:

	uvc_stream_ctrl_t ctrl;
	int ok = 0;

	// Try a few mode indices exported by your thetauvc.c
	// (commonly 0 = 3840x1920@30, 1 = 1920x960@30, but it depends on your file)
	unsigned int modes_to_try[] = {0, 1, 2, 3};
	for (size_t i = 0; i < sizeof(modes_to_try)/sizeof(modes_to_try[0]); ++i) {
		if (thetauvc_get_stream_ctrl_format_size(devh, modes_to_try[i], &ctrl) == UVC_SUCCESS) {
		  g_print("thetauvc: selected mode index %u\n", modes_to_try[i]);
		  ok = 1;
		  break;
		}
	}

	if (!ok) {
		g_error("Failed to negotiate H.264 stream profile via thetauvc (tried mode indices 0..3). "
		        "Check your thetauvc.c for available modes.");
	}


  // Start stream: frames will arrive at uvc_frame_cb()
  res = uvc_start_streaming(devh, &ctrl, uvc_frame_cb, NULL, 0);
  if (res != UVC_SUCCESS) g_error("uvc_start_streaming failed: %d", res);

  g_print("Streaming… Ctrl+C to stop.\n");
  g_loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(g_loop);

  // Cleanup
  uvc_stop_streaming(devh);
  uvc_close(devh);
  uvc_exit(ctx);

  gst_element_set_state(g_pipeline, GST_STATE_NULL);
  if (g_appsrc)   gst_object_unref(g_appsrc);
  if (g_pipeline) gst_object_unref(g_pipeline);
  if (g_loop)     g_main_loop_unref(g_loop);
  if (g_timer)    g_timer_destroy(g_timer);

  return 0;
}

