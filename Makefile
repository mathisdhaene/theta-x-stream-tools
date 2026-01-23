# 
# Portions of this build workflow are inspired by RICOH THETA libuvc-theta-sample.
# See NOTICE for attributions and upstream licenses.
#

CC      := gcc
CSTD    := -std=gnu11
CFLAGS  := -O2 -Wall -Wextra $(CSTD)
LDFLAGS :=

# GStreamer
GST_PKG := gstreamer-1.0 gstreamer-app-1.0
GST_CFLAGS := $(shell pkg-config --cflags $(GST_PKG))
GST_LIBS   := $(shell pkg-config --libs   $(GST_PKG))

# Common libs
LIBS_COMMON := -luvc -lusb-1.0
LIBS_PTHREAD := -lpthread

# Targets
TARGETS := min_latency_from_uvc gst_viewer_vicon

# Local thetauvc helper
THETAUVC_OBJ := thetauvc.o
THETAUVC_HDR := thetauvc.h
THETAUVC_SRC := thetauvc.c

.PHONY: all
all: $(TARGETS)

# Build rules live in src/
$(THETAUVC_OBJ): src/$(THETAUVC_SRC) src/$(THETAUVC_HDR)
	$(CC) $(CFLAGS) -c $< -o $@

min_latency_from_uvc: src/min_latency_from_uvc.c $(THETAUVC_OBJ)
	$(CC) $(CFLAGS) $(GST_CFLAGS) $^ -o $@ $(GST_LIBS) $(LIBS_COMMON) $(LDFLAGS)

gst_viewer_vicon: src/gst_viewer_vicon.c $(THETAUVC_OBJ)
	$(CC) $(CFLAGS) $(GST_CFLAGS) $^ -o $@ $(GST_LIBS) $(LIBS_COMMON) $(LIBS_PTHREAD) $(LDFLAGS)

.PHONY: clean veryclean
clean:
	rm -f *.o

veryclean: clean
	rm -f $(TARGETS)
