#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include "libuvc/libuvc.h"
#include "thetauvc.h"
#include <time.h>
#include <signal.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <stdint.h>

#define VICON_PORT 5005
#define VICON_SYNC_PORT 5006
#define MAX_PIPELINE_LEN 1024
#define VICON_MAX_PKT  2048

static gboolean first_frame = TRUE;

/* ---------- Structures / état GStreamer ---------- */
struct gst_src {
    GstElement *pipeline;
    GstElement *appsrc;
    GMainLoop *loop;
    GTimer *timer;
    guint framecount;
    guint id;
    guint bus_watch_id;
    uint32_t dwFrameInterval;
    uint32_t dwClockFrequency;
};
static struct gst_src src;

/* ---------- Sockets & fichiers ---------- */
static int vicon_sock = -1;

static int latency_sock = -1;
static struct sockaddr_in latency_dest;

static char output_filename[256];      /* vidéo MP4 */
static char vicon_frame_csv[256];      /* (optionnel) CSV “par frame vidéo” */
static char vicon_100hz_csv[256];      /* CSV 100 Hz (toutes les trames) */

/* ---------- Buffer partagé pour la “dernière trame Vicon” ---------- */
static pthread_mutex_t last_pkt_mtx = PTHREAD_MUTEX_INITIALIZER;
static size_t          last_pkt_len = 0;
static char            last_pkt_buf[VICON_MAX_PKT];

/* ---------- Contrôle du thread Vicon ---------- */
static pthread_t vicon_thr;
static volatile int vicon_run = 0;

/* ---------- Signal handling ---------- */
static gboolean got_sigint = FALSE;
static void handle_sigint(int sig) {
    (void)sig;
    got_sigint = TRUE;
    vicon_run = 0;
    if (src.loop) g_main_loop_quit(src.loop);
}

/* ---------- Bus callback ---------- */
static gboolean gst_bus_cb(GstBus *bus, GstMessage *message, gpointer data) {
    (void)bus; (void)data;
    GError *err = NULL; gchar *dbg = NULL;
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(message, &err, &dbg);
            g_printerr("Error: %s\n", err ? err->message : "(unknown)");
            if (err) g_error_free(err);
            if (dbg) g_free(dbg);
            if (src.loop) g_main_loop_quit(src.loop);
            break;
        default: break;
    }
    return TRUE;
}

/* ---------- Utilitaires ---------- */
static void generate_timestamp_suffix(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, size, "%Y%m%d_%H%M%S", tm_info);
}

/* Parsing et écriture CSV d’un paquet Vicon (timestamp ISO + floats) */
static void csv_write_parsed_packet(FILE *f, const char *data, ssize_t len) {
    if (!f || len <= 0) return;
    const char *comma = memchr(data, ',', len);
    if (!comma) return;
    size_t ts_len = (size_t)(comma - data);
    char timestamp[128] = {0};
    if (ts_len >= sizeof(timestamp)) ts_len = sizeof(timestamp) - 1;
    memcpy(timestamp, data, ts_len);

    const char *float_data = comma + 1;
    ssize_t float_len = len - (ssize_t)(ts_len + 1);
    if (float_len <= 0) return;

    int nf = (int)(float_len / (ssize_t)sizeof(float));
    if (nf <= 0) return;

    /* copie sûre */
    float *coords = (float*)malloc((size_t)nf * sizeof(float));
    if (!coords) return;
    memcpy(coords, float_data, (size_t)nf * sizeof(float));

    fprintf(f, "%s", timestamp);
    for (int i = 0; i < nf; ++i) fprintf(f, ",%.6f", coords[i]);
    fprintf(f, "\n");
    free(coords);
}

/* ---------- Initialisation pipeline GStreamer ----------
   appsrc (H.264 byte-stream) → h264parse → tee
   - branche 1: décodage → v4l2sink (temps réel)
   - branche 2: MP4 (mp4mux → filesink)
*/
static int gst_src_init(int *argc, char ***argv, const char *output_file) {
    GstCaps *caps;
    GstBus *bus;
    char pipeline_str[MAX_PIPELINE_LEN];

    snprintf(pipeline_str, MAX_PIPELINE_LEN,
        "appsrc name=ap is-live=true block=false format=time ! "
        "queue max-size-buffers=1 leaky=downstream ! "
        "h264parse config-interval=-1 ! tee name=t "
        /* Aperçu temps réel */
        "t. ! queue ! avdec_h264 ! videoconvert ! "
        "video/x-raw,format=YUY2,width=3840,height=1920,framerate=30/1 ! "
        "v4l2sink device=/dev/video2 sync=false "
        /* Enregistrement MP4 (sans ré-encoder) */
        "t. ! queue ! video/x-h264,stream-format=avc,alignment=au ! "
        "mp4mux faststart=true name=mux ! "
        "filesink location=\"%s\" async=false sync=false",
        output_file
    );

    gst_init(argc, argv);

    src.timer = g_timer_new();
    src.loop  = g_main_loop_new(NULL, TRUE);
    src.pipeline = gst_parse_launch(pipeline_str, NULL);
    if (!src.pipeline) { g_printerr("Pipeline GStreamer invalide\n"); return FALSE; }

    gst_pipeline_set_clock(GST_PIPELINE(src.pipeline), gst_system_clock_obtain());

    src.appsrc = gst_bin_get_by_name(GST_BIN(src.pipeline), "ap");
    if (!src.appsrc) { g_printerr("appsrc introuvable\n"); return FALSE; }

    caps = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment",     G_TYPE_STRING, "au",
        "width",         G_TYPE_INT,    3840,
        "height",        G_TYPE_INT,    1920,
        "framerate",     GST_TYPE_FRACTION, 30, 1,
        NULL);
    gst_app_src_set_caps(GST_APP_SRC(src.appsrc), caps);
    gst_caps_unref(caps);

    bus = gst_pipeline_get_bus(GST_PIPELINE(src.pipeline));
    src.bus_watch_id = gst_bus_add_watch(bus, gst_bus_cb, NULL);
    gst_object_unref(bus);

    return TRUE;
}

/* ---------- Thread Vicon : lit 100% des paquets et écrit vicon_100hz_*.csv ---------- */
static void* vicon_thread_fn(void *arg) {
    (void)arg;
    FILE *f100 = fopen(vicon_100hz_csv, "w");
    if (!f100) {
        perror("open vicon_100hz_csv");
        return NULL;
    }
    /* En-tête simple (facultatif) */
    fprintf(f100, "vicon_timestamp,values...\n");
    fflush(f100);

    char buf[VICON_MAX_PKT];
    struct sockaddr_in srcaddr;
    socklen_t slen = sizeof(srcaddr);

    while (vicon_run) {
        ssize_t n = recvfrom(vicon_sock, buf, sizeof(buf), 0, (struct sockaddr*)&srcaddr, &slen);
        if (n <= 0) {
            if (!vicon_run) break;
            /* petite pause pour ne pas surcharger si erreur transitoire */
            usleep(1000);
            continue;
        }

        /* Écrit TOUTES les trames Vicon reçues (100 Hz typ.) */
        csv_write_parsed_packet(f100, buf, n);

        /* Met à disposition la dernière trame pour le callback vidéo */
        pthread_mutex_lock(&last_pkt_mtx);
        if ((size_t)n > sizeof(last_pkt_buf)) n = sizeof(last_pkt_buf);
        memcpy(last_pkt_buf, buf, (size_t)n);
        last_pkt_len = (size_t)n;
        pthread_mutex_unlock(&last_pkt_mtx);
    }

    fclose(f100);
    return NULL;
}

/* ---------- Socket latence (optionnel) ---------- */
static void setup_latency_socket(void) {
    latency_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (latency_sock < 0) { perror("latency socket"); return; }
    memset(&latency_dest, 0, sizeof(latency_dest));
    latency_dest.sin_family = AF_INET;
    latency_dest.sin_port = htons(9009);
    latency_dest.sin_addr.s_addr = inet_addr("127.0.0.1");
}

/* ---------- Callback UVC : pousse la vidéo; lit la DERNIÈRE trame Vicon (optionnel) ---------- */
static void cb(uvc_frame_t *frame, void *ptr) {
    struct gst_src *s = (struct gst_src *)ptr;

    /* Timestamp latence (optionnel) */
    if (latency_sock < 0) setup_latency_socket();
    struct timespec ts_latency;
    clock_gettime(CLOCK_MONOTONIC, &ts_latency);
    uint64_t timestamp_us = (uint64_t)ts_latency.tv_sec * 1000000ULL + (uint64_t)ts_latency.tv_nsec / 1000ULL;
    sendto(latency_sock, &timestamp_us, sizeof(timestamp_us), 0,
           (struct sockaddr *)&latency_dest, sizeof(latency_dest));

    /* ----- (Optionnel) Log “par frame vidéo” : on ne lit pas le socket !
       On prend juste la DERNIÈRE trame que le thread Vicon a déposée. ----- */
    size_t copy_len = 0;
    char   copy_buf[VICON_MAX_PKT];
    pthread_mutex_lock(&last_pkt_mtx);
    if (last_pkt_len > 0) {
        copy_len = last_pkt_len;
        if (copy_len > sizeof(copy_buf)) copy_len = sizeof(copy_buf);
        memcpy(copy_buf, last_pkt_buf, copy_len);
    }
    pthread_mutex_unlock(&last_pkt_mtx);

    if (copy_len > 0) {
        FILE *ff = fopen(vicon_frame_csv, "a");
        if (ff) {
            csv_write_parsed_packet(ff, copy_buf, (ssize_t)copy_len);
            fclose(ff);
        }
    }

    /* Push H.264 vers appsrc */
    GstBuffer *buffer;
    GstFlowReturn ret;
    GstMapInfo map;

    buffer = gst_buffer_new_allocate(NULL, frame->data_bytes, NULL);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    if (first_frame) {
        printf("🟢 Première frame vidéo @ %ld.%09ld (REALTIME)\n", ts.tv_sec, ts.tv_nsec);
        first_frame = FALSE;
    }

    gdouble elapsed = g_timer_elapsed(s->timer, NULL);
    GST_BUFFER_PTS(buffer)       = (GstClockTime)(elapsed * GST_SECOND);
    GST_BUFFER_DTS(buffer)       = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer)  = (GstClockTime)(1.0 / 30.0 * GST_SECOND);
    GST_BUFFER_OFFSET(buffer)    = frame->sequence;

    gst_buffer_map(buffer, &map, GST_MAP_WRITE);
    memcpy(map.data, frame->data, frame->data_bytes);
    gst_buffer_unmap(buffer, &map);

    g_signal_emit_by_name(s->appsrc, "push-buffer", buffer, &ret);
    gst_buffer_unref(buffer);

    if (ret != GST_FLOW_OK) fprintf(stderr, "push-buffer error: %d\n", ret);
}

static void* keywait(void *arg) {
    (void)arg;
    printf("Press any key to stop...\n");
    char c;
    (void)read(0, &c, 1);
    if (src.loop) g_main_loop_quit(src.loop);
    return NULL;
}




/* ---------- main ---------- */
int main(int argc, char **argv) {
    signal(SIGINT, handle_sigint);

    char ts_suffix[64];
    generate_timestamp_suffix(ts_suffix, sizeof(ts_suffix));

    snprintf(output_filename,   sizeof(output_filename),   "output_%s.mp4", ts_suffix);
    snprintf(vicon_frame_csv,   sizeof(vicon_frame_csv),   "vicon_log_%s.csv", ts_suffix);
    snprintf(vicon_100hz_csv,   sizeof(vicon_100hz_csv),   "vicon_100hz_%s.csv", ts_suffix);

    printf("Vidéo (MP4)           : %s\n", output_filename);
    printf("Vicon par frame vidéo : %s\n", vicon_frame_csv);
    printf("Vicon 100 Hz          : %s\n", vicon_100hz_csv);

    /* Init GStreamer */
    if (!gst_src_init(&argc, &argv, output_filename)) return -1;

    /* Init UVC / THETA */
    uvc_context_t *ctx = NULL;
    uvc_device_t *dev = NULL;
    uvc_device_t **devlist = NULL;
    uvc_device_handle_t *devh = NULL;
    uvc_stream_ctrl_t ctrl;
    uvc_error_t res;

    res = uvc_init(&ctx, NULL);
    if (res != UVC_SUCCESS) { uvc_perror(res, "uvc_init"); return -1; }

    /* Socket UDP Vicon (réception) — bind une seule fois, thread dédié lira tout */
    struct sockaddr_in vaddr;
    vicon_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (vicon_sock < 0) { perror("socket vicon"); goto exit_fail; }
    memset(&vaddr, 0, sizeof(vaddr));
    vaddr.sin_family = AF_INET;
    vaddr.sin_port   = htons(VICON_PORT);
    vaddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(vicon_sock, (struct sockaddr *)&vaddr, sizeof(vaddr)) < 0) {
        perror("bind vicon");
        goto exit_fail;
    }

    /* (Optionnel) Lister devices */
    if (argc > 1 && strcmp("-l", argv[1]) == 0) {
        if (thetauvc_find_devices(ctx, &devlist) == UVC_SUCCESS) {
            int idx = 0;
            while (devlist[idx] != NULL) {
                uvc_device_descriptor_t *desc;
                if (uvc_get_device_descriptor(devlist[idx], &desc) == UVC_SUCCESS) {
                    printf("%2d : %-18s : %-10s\n", idx, desc->product, desc->serialNumber);
                    uvc_free_device_descriptor(desc);
                }
                idx++;
            }
            uvc_free_device_list(devlist, 1);
        }
        uvc_exit(ctx);
        close(vicon_sock);
        return 0;
    }

    /* Ouverture THETA */
    res = thetauvc_find_device(ctx, &dev, 0);
    if (res != UVC_SUCCESS) { fprintf(stderr, "THETA not found\n"); goto exit_fail; }
    res = uvc_open(dev, &devh);
    if (res != UVC_SUCCESS) { fprintf(stderr, "Can't open THETA\n"); goto exit_fail; }

    /* Démarre le thread Vicon (lit tout à 100 Hz et écrit vicon_100hz_*.csv) */
    vicon_run = 1;
    if (pthread_create(&vicon_thr, NULL, vicon_thread_fn, NULL) != 0) {
        perror("pthread_create vicon");
        goto exit_fail;
    }

    /* (Facultatif) notifier un script via UDP READY — adapter IP si besoin */
    {
        int sync_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sync_sock >= 0) {
            struct sockaddr_in py_addr;
            memset(&py_addr, 0, sizeof(py_addr));
            py_addr.sin_family = AF_INET;
            py_addr.sin_port = htons(VICON_SYNC_PORT);
            inet_pton(AF_INET, "127.0.0.1", &py_addr.sin_addr);
            sendto(sync_sock, "READY", 5, 0, (struct sockaddr *)&py_addr, sizeof(py_addr));
            close(sync_sock);
        }
    }

    /* Lancement pipeline + streaming */
    gst_element_set_state(src.pipeline, GST_STATE_PLAYING);

    pthread_t thr_key;
    pthread_create(&thr_key, NULL, keywait, NULL);


    src.framecount = 0;
    res = thetauvc_get_stream_ctrl_format_size(devh, THETAUVC_MODE_UHD_2997, &ctrl);
    src.dwFrameInterval = ctrl.dwFrameInterval;
    src.dwClockFrequency = ctrl.dwClockFrequency;
    res = uvc_start_streaming(devh, &ctrl, cb, &src, 0);

    if (res == UVC_SUCCESS) {
        fprintf(stderr, "start, hit any key to stop\n");
        g_main_loop_run(src.loop);
        fprintf(stderr, "stop\n");
        uvc_stop_streaming(devh);

        /* EOS pour finaliser MP4 */
        GstFlowReturn eos_ret;
        g_signal_emit_by_name(src.appsrc, "end-of-stream", &eos_ret);
        gst_element_send_event(src.pipeline, gst_event_new_eos());

        /* Attente EOS */
        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(src.pipeline));
        GstMessage *msg = NULL;
        do {
            msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                    (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        } while (msg && GST_MESSAGE_TYPE(msg) != GST_MESSAGE_EOS);
        if (msg) gst_message_unref(msg);
        gst_object_unref(bus);

        gst_element_set_state(src.pipeline, GST_STATE_NULL);
        if (src.bus_watch_id) g_source_remove(src.bus_watch_id);
        if (src.loop) g_main_loop_unref(src.loop);
        pthread_cancel(thr_key);
        pthread_join(thr_key, NULL);
    } else {
        uvc_perror(res, "uvc_start_streaming");
    }

    /* Arrêt propre du thread Vicon */
    vicon_run = 0;
    shutdown(vicon_sock, SHUT_RD); /* pour débloquer recvfrom si besoin */
    pthread_join(vicon_thr, NULL);

    /* Nettoyage UVC/GStreamer */
    uvc_close(devh);
    uvc_exit(ctx);
    close(vicon_sock);

    return 0;

exit_fail:
    vicon_run = 0;
    if (vicon_sock >= 0) close(vicon_sock);
    if (devh) uvc_close(devh);
    if (ctx)  uvc_exit(ctx);
    return -1;
}

