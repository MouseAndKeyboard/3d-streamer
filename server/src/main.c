#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <libwebsockets.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib-unix.h>
#include <glib.h>

#include "json_util.h"

#define LOG(fmt, ...) fprintf(stderr, "[cube_server] " fmt "\n", ##__VA_ARGS__)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct WsMessage {
  char *data;
  size_t len;
  struct WsMessage *next;
} WsMessage;

typedef struct {
  int width;
  int height;
  int fps;
  int bitrate_kbps;
  int port;
  char *stun_server;

  GstElement *pipeline;
  GstElement *appsrc;
  GstElement *webrtcbin;
  GMainLoop *main_loop;
  GMainContext *main_context;

  struct lws_context *lws_context;
  struct lws *client_wsi;
  pthread_t ws_thread;
  pthread_t render_thread;

  pthread_mutex_t ws_lock;
  WsMessage *ws_head;
  WsMessage *ws_tail;

  bool running;
  bool render_running;
} AppState;

typedef struct {
  EGLDisplay display;
  EGLContext context;
  EGLSurface surface;
  GLuint program;
  GLuint vbo;
  GLuint ibo;
  GLint attr_pos;
  GLint attr_color;
  GLint uni_mvp;
  int width;
  int height;
} Renderer;

typedef struct {
  AppState *state;
  char *message;
} IncomingMessage;

static void start_streaming(AppState *state);
static void stop_streaming(AppState *state);
static void ws_drain_queue(AppState *state);

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int getenv_int(const char *key, int def_value) {
  const char *value = getenv(key);
  if (!value || !*value) {
    return def_value;
  }
  return atoi(value);
}

static char *getenv_strdup(const char *key, const char *def_value) {
  const char *value = getenv(key);
  if (!value || !*value) {
    return def_value ? strdup(def_value) : NULL;
  }
  return strdup(value);
}

static void load_config(AppState *state) {
  state->width = getenv_int("CS_WIDTH", 1280);
  state->height = getenv_int("CS_HEIGHT", 720);
  state->fps = getenv_int("CS_FPS", 30);
  state->bitrate_kbps = getenv_int("CS_BITRATE_KBPS", 2500);
  state->port = getenv_int("CS_PORT", 8080);
  state->stun_server = getenv_strdup("CS_STUN_SERVER", NULL);
}

static void ws_queue_message(AppState *state, char *msg, size_t len) {
  WsMessage *item = calloc(1, sizeof(*item));
  item->data = msg;
  item->len = len;

  pthread_mutex_lock(&state->ws_lock);
  if (state->ws_tail) {
    state->ws_tail->next = item;
    state->ws_tail = item;
  } else {
    state->ws_head = item;
    state->ws_tail = item;
  }
  pthread_mutex_unlock(&state->ws_lock);

  if (state->client_wsi) {
    lws_callback_on_writable(state->client_wsi);
  }
  if (state->lws_context) {
    lws_cancel_service(state->lws_context);
  }
}

static void ws_send_ready(AppState *state) {
  const char *payload = "{\"type\":\"ready\"}";
  char *msg = strdup(payload);
  ws_queue_message(state, msg, strlen(payload));
}

static void ws_send_sdp(AppState *state, const char *type, const char *sdp) {
  char *escaped = json_escape(sdp);
  size_t len = (size_t)snprintf(NULL, 0, "{\"type\":\"%s\",\"sdp\":\"%s\"}", type, escaped);
  char *msg = malloc(len + 1);
  snprintf(msg, len + 1, "{\"type\":\"%s\",\"sdp\":\"%s\"}", type, escaped);
  free(escaped);
  ws_queue_message(state, msg, len);
}

static void ws_send_ice(AppState *state, const char *candidate, int mline) {
  char *escaped = json_escape(candidate);
  size_t len = (size_t)snprintf(NULL, 0,
                                "{\"type\":\"ice\",\"candidate\":\"%s\",\"sdpMLineIndex\":%d}",
                                escaped, mline);
  char *msg = malloc(len + 1);
  snprintf(msg, len + 1,
           "{\"type\":\"ice\",\"candidate\":\"%s\",\"sdpMLineIndex\":%d}",
           escaped, mline);
  free(escaped);
  ws_queue_message(state, msg, len);
}

static gboolean bus_message_cb(GstBus *bus, GstMessage *message, gpointer user_data) {
  (void)bus;
  AppState *state = user_data;

  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
      GError *err = NULL;
      gchar *debug = NULL;
      gst_message_parse_error(message, &err, &debug);
      LOG("GStreamer error: %s", err ? err->message : "unknown");
      if (debug) {
        LOG("Debug info: %s", debug);
      }
      g_clear_error(&err);
      g_free(debug);
      g_main_loop_quit(state->main_loop);
      break;
    }
    case GST_MESSAGE_EOS:
      LOG("GStreamer EOS");
      g_main_loop_quit(state->main_loop);
      break;
    default:
      break;
  }
  return TRUE;
}

static bool renderer_init(Renderer *renderer, int width, int height) {
  renderer->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (renderer->display == EGL_NO_DISPLAY) {
    LOG("EGL display not available");
    return false;
  }
  if (!eglInitialize(renderer->display, NULL, NULL)) {
    LOG("Failed to initialize EGL");
    return false;
  }

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    LOG("Failed to bind OpenGL ES API");
    return false;
  }

  EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 16,
      EGL_NONE};
  EGLConfig config;
  EGLint num_configs = 0;
  if (!eglChooseConfig(renderer->display, config_attribs, &config, 1, &num_configs) ||
      num_configs == 0) {
    LOG("Failed to choose EGL config");
    return false;
  }

  EGLint pbuffer_attribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
  renderer->surface = eglCreatePbufferSurface(renderer->display, config, pbuffer_attribs);
  if (renderer->surface == EGL_NO_SURFACE) {
    LOG("Failed to create EGL surface");
    return false;
  }

  EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  renderer->context = eglCreateContext(renderer->display, config, EGL_NO_CONTEXT, context_attribs);
  if (renderer->context == EGL_NO_CONTEXT) {
    LOG("Failed to create EGL context");
    return false;
  }

  if (!eglMakeCurrent(renderer->display, renderer->surface, renderer->surface, renderer->context)) {
    LOG("Failed to make EGL context current");
    return false;
  }

  const char *vs_src =
      "attribute vec3 a_pos;"
      "attribute vec3 a_color;"
      "uniform mat4 u_mvp;"
      "varying vec3 v_color;"
      "void main() {"
      "  v_color = a_color;"
      "  gl_Position = u_mvp * vec4(a_pos, 1.0);"
      "}";
  const char *fs_src =
      "precision mediump float;"
      "varying vec3 v_color;"
      "void main() {"
      "  gl_FragColor = vec4(v_color, 1.0);"
      "}";

  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_src, NULL);
  glCompileShader(vs);

  GLint status = 0;
  glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
  if (status != GL_TRUE) {
    LOG("Vertex shader compilation failed");
    glDeleteShader(vs);
    return false;
  }

  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_src, NULL);
  glCompileShader(fs);
  glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
  if (status != GL_TRUE) {
    LOG("Fragment shader compilation failed");
    glDeleteShader(vs);
    glDeleteShader(fs);
    return false;
  }

  renderer->program = glCreateProgram();
  glAttachShader(renderer->program, vs);
  glAttachShader(renderer->program, fs);
  glBindAttribLocation(renderer->program, 0, "a_pos");
  glBindAttribLocation(renderer->program, 1, "a_color");
  glLinkProgram(renderer->program);
  glGetProgramiv(renderer->program, GL_LINK_STATUS, &status);
  glDeleteShader(vs);
  glDeleteShader(fs);

  if (status != GL_TRUE) {
    LOG("Shader program link failed");
    return false;
  }

  renderer->attr_pos = 0;
  renderer->attr_color = 1;
  renderer->uni_mvp = glGetUniformLocation(renderer->program, "u_mvp");

  typedef struct {
    float pos[3];
    float color[3];
  } Vertex;

  Vertex vertices[] = {
      {{-1.0f, -1.0f, -1.0f}, {1.0f, 0.0f, 0.0f}},
      {{1.0f, -1.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
      {{1.0f, 1.0f, -1.0f}, {0.0f, 0.0f, 1.0f}},
      {{-1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 0.0f}},
      {{-1.0f, -1.0f, 1.0f}, {1.0f, 0.0f, 1.0f}},
      {{1.0f, -1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}},
      {{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
      {{-1.0f, 1.0f, 1.0f}, {0.2f, 0.8f, 0.3f}},
  };

  uint16_t indices[] = {
      0, 1, 2, 2, 3, 0,
      4, 5, 6, 6, 7, 4,
      0, 4, 7, 7, 3, 0,
      1, 5, 6, 6, 2, 1,
      3, 2, 6, 6, 7, 3,
      0, 1, 5, 5, 4, 0,
  };

  glGenBuffers(1, &renderer->vbo);
  glBindBuffer(GL_ARRAY_BUFFER, renderer->vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

  glGenBuffers(1, &renderer->ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

  renderer->width = width;
  renderer->height = height;

  glEnable(GL_DEPTH_TEST);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);

  return true;
}

static void renderer_shutdown(Renderer *renderer) {
  if (renderer->vbo) {
    glDeleteBuffers(1, &renderer->vbo);
  }
  if (renderer->ibo) {
    glDeleteBuffers(1, &renderer->ibo);
  }
  if (renderer->program) {
    glDeleteProgram(renderer->program);
  }
  if (renderer->display != EGL_NO_DISPLAY) {
    eglMakeCurrent(renderer->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (renderer->context != EGL_NO_CONTEXT) {
    eglDestroyContext(renderer->display, renderer->context);
  }
  if (renderer->surface != EGL_NO_SURFACE) {
    eglDestroySurface(renderer->display, renderer->surface);
  }
  if (renderer->display != EGL_NO_DISPLAY) {
    eglTerminate(renderer->display);
  }
  memset(renderer, 0, sizeof(*renderer));
}

static void mat4_identity(float *m) {
  memset(m, 0, sizeof(float) * 16);
  m[0] = 1.0f;
  m[5] = 1.0f;
  m[10] = 1.0f;
  m[15] = 1.0f;
}

static void mat4_mul(float *out, const float *a, const float *b) {
  float r[16];
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      r[col + row * 4] =
          a[row * 4 + 0] * b[col + 0] +
          a[row * 4 + 1] * b[col + 4] +
          a[row * 4 + 2] * b[col + 8] +
          a[row * 4 + 3] * b[col + 12];
    }
  }
  memcpy(out, r, sizeof(r));
}

static void mat4_perspective(float *m, float fovy_rad, float aspect, float znear, float zfar) {
  float f = 1.0f / tanf(fovy_rad / 2.0f);
  mat4_identity(m);
  m[0] = f / aspect;
  m[5] = f;
  m[10] = (zfar + znear) / (znear - zfar);
  m[11] = -1.0f;
  m[14] = (2.0f * zfar * znear) / (znear - zfar);
  m[15] = 0.0f;
}

static void mat4_translate(float *m, float x, float y, float z) {
  mat4_identity(m);
  m[12] = x;
  m[13] = y;
  m[14] = z;
}

static void mat4_rotate_y(float *m, float angle) {
  mat4_identity(m);
  float c = cosf(angle);
  float s = sinf(angle);
  m[0] = c;
  m[2] = s;
  m[8] = -s;
  m[10] = c;
}

static void mat4_rotate_x(float *m, float angle) {
  mat4_identity(m);
  float c = cosf(angle);
  float s = sinf(angle);
  m[5] = c;
  m[6] = -s;
  m[9] = s;
  m[10] = c;
}

static void renderer_draw(Renderer *renderer, float angle, uint8_t *pixels) {
  float proj[16];
  float view[16];
  float model_y[16];
  float model_x[16];
  float model[16];
  float mv[16];
  float mvp[16];

  mat4_perspective(proj, (float)(60.0 * M_PI / 180.0),
                   (float)renderer->width / (float)renderer->height, 0.1f, 100.0f);
  mat4_translate(view, 0.0f, 0.0f, -4.0f);
  mat4_rotate_y(model_y, angle);
  mat4_rotate_x(model_x, angle * 0.6f);
  mat4_mul(model, model_y, model_x);
  mat4_mul(mv, view, model);
  mat4_mul(mvp, proj, mv);

  glViewport(0, 0, renderer->width, renderer->height);
  glClearColor(0.05f, 0.07f, 0.12f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glUseProgram(renderer->program);
  glUniformMatrix4fv(renderer->uni_mvp, 1, GL_FALSE, mvp);

  glBindBuffer(GL_ARRAY_BUFFER, renderer->vbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->ibo);

  glEnableVertexAttribArray(renderer->attr_pos);
  glVertexAttribPointer(renderer->attr_pos, 3, GL_FLOAT, GL_FALSE,
                        sizeof(float) * 6, (void *)0);
  glEnableVertexAttribArray(renderer->attr_color);
  glVertexAttribPointer(renderer->attr_color, 3, GL_FLOAT, GL_FALSE,
                        sizeof(float) * 6, (void *)(sizeof(float) * 3));

  glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
  glReadPixels(0, 0, renderer->width, renderer->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

static void *render_thread_main(void *data) {
  AppState *state = data;
  Renderer renderer = {0};

  if (!renderer_init(&renderer, state->width, state->height)) {
    LOG("Renderer initialization failed");
    state->render_running = false;
    return NULL;
  }

  uint64_t frame_duration = 1000000000ull / (uint64_t)state->fps;
  uint64_t start_time = now_ns();
  uint64_t next_time = start_time;

  size_t frame_size = (size_t)state->width * (size_t)state->height * 4;

  while (state->render_running) {
    uint64_t now = now_ns();
    if (now < next_time) {
      uint64_t sleep_ns = next_time - now;
      struct timespec ts;
      ts.tv_sec = (time_t)(sleep_ns / 1000000000ull);
      ts.tv_nsec = (long)(sleep_ns % 1000000000ull);
      nanosleep(&ts, NULL);
      continue;
    }
    next_time += frame_duration;

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, frame_size, NULL);
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
      gst_buffer_unref(buffer);
      continue;
    }

    float seconds = (float)((now - start_time) / 1000000000.0);
    renderer_draw(&renderer, seconds * 0.6f, map.data);
    gst_buffer_unmap(buffer, &map);

    GST_BUFFER_PTS(buffer) = (GstClockTime)(now - start_time);
    GST_BUFFER_DURATION(buffer) = (GstClockTime)frame_duration;

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(state->appsrc), buffer);
    if (ret != GST_FLOW_OK) {
      LOG("appsrc push failed: %d", ret);
      gst_buffer_unref(buffer);
      break;
    }
  }

  renderer_shutdown(&renderer);
  return NULL;
}

static void on_ice_candidate(GstElement *webrtcbin, guint mlineindex, gchar *candidate,
                             AppState *state) {
  (void)webrtcbin;
  ws_send_ice(state, candidate, (int)mlineindex);
}

static void on_answer_created(GstPromise *promise, gpointer user_data) {
  AppState *state = user_data;
  const GstStructure *reply = gst_promise_get_reply(promise);
  GstWebRTCSessionDescription *answer = NULL;

  gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
  gst_promise_unref(promise);

  if (!answer) {
    LOG("Failed to create WebRTC answer");
    return;
  }

  g_signal_emit_by_name(state->webrtcbin, "set-local-description", answer, NULL);
  char *sdp_text = gst_sdp_message_as_text(answer->sdp);
  ws_send_sdp(state, "answer", sdp_text);
  g_free(sdp_text);
  gst_webrtc_session_description_free(answer);
}

static void handle_offer(AppState *state, const char *sdp_text) {
  GstSDPMessage *sdp = NULL;
  if (gst_sdp_message_new(&sdp) != GST_SDP_OK) {
    LOG("Failed to allocate SDP");
    return;
  }
  if (gst_sdp_message_parse_buffer((const guint8 *)sdp_text, strlen(sdp_text), sdp) != GST_SDP_OK) {
    LOG("Failed to parse SDP offer");
    gst_sdp_message_free(sdp);
    return;
  }

  GstWebRTCSessionDescription *offer =
      gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);

  g_signal_emit_by_name(state->webrtcbin, "set-remote-description", offer, NULL);
  gst_webrtc_session_description_free(offer);

  GstPromise *promise = gst_promise_new_with_change_func(on_answer_created, state, NULL);
  g_signal_emit_by_name(state->webrtcbin, "create-answer", NULL, promise);
}

static void handle_ice(AppState *state, const char *candidate, int mline) {
  g_signal_emit_by_name(state->webrtcbin, "add-ice-candidate", mline, candidate);
}

static gboolean handle_ws_message(gpointer data) {
  IncomingMessage *incoming = data;
  AppState *state = incoming->state;

  char *type = json_get_string(incoming->message, "type");
  if (!type) {
    free(incoming->message);
    free(incoming);
    return G_SOURCE_REMOVE;
  }

  if (strcmp(type, "offer") == 0) {
    char *sdp = json_get_string(incoming->message, "sdp");
    if (sdp) {
      handle_offer(state, sdp);
      free(sdp);
    }
  } else if (strcmp(type, "ice") == 0) {
    char *candidate = json_get_string(incoming->message, "candidate");
    int mline = json_get_int(incoming->message, "sdpMLineIndex", 0);
    if (candidate) {
      handle_ice(state, candidate, mline);
      free(candidate);
    }
  }

  free(type);
  free(incoming->message);
  free(incoming);
  return G_SOURCE_REMOVE;
}

static gboolean handle_client_connected(gpointer data) {
  AppState *state = data;
  start_streaming(state);
  ws_send_ready(state);
  return G_SOURCE_REMOVE;
}

static gboolean handle_client_closed(gpointer data) {
  AppState *state = data;
  stop_streaming(state);
  ws_drain_queue(state);
  return G_SOURCE_REMOVE;
}

static bool create_pipeline(AppState *state) {
  GstCaps *caps = NULL;
  GstBus *bus = NULL;

  state->pipeline = gst_pipeline_new("cube-pipeline");
  state->appsrc = gst_element_factory_make("appsrc", "src");
  GstElement *queue = gst_element_factory_make("queue", "queue");
  GstElement *convert = gst_element_factory_make("videoconvert", "convert");
  GstElement *encoder = gst_element_factory_make("x264enc", "encoder");
  GstElement *parser = gst_element_factory_make("h264parse", "parser");
  GstElement *pay = gst_element_factory_make("rtph264pay", "pay");
  state->webrtcbin = gst_element_factory_make("webrtcbin", "webrtc");

  if (!state->pipeline || !state->appsrc || !queue || !convert || !encoder || !parser || !pay ||
      !state->webrtcbin) {
    LOG("Failed to create GStreamer elements");
    return false;
  }

  gst_bin_add_many(GST_BIN(state->pipeline), state->appsrc, queue, convert, encoder, parser, pay,
                   state->webrtcbin, NULL);

  caps = gst_caps_new_simple("video/x-raw",
                             "format", G_TYPE_STRING, "RGBA",
                             "width", G_TYPE_INT, state->width,
                             "height", G_TYPE_INT, state->height,
                             "framerate", GST_TYPE_FRACTION, state->fps, 1,
                             NULL);

  g_object_set(state->appsrc,
               "is-live", TRUE,
               "format", GST_FORMAT_TIME,
               "do-timestamp", TRUE,
               "block", TRUE,
               "caps", caps,
               NULL);
  gst_caps_unref(caps);

  g_object_set(queue,
               "leaky", 2,
               "max-size-buffers", 2,
               "max-size-time", (guint64)0,
               "max-size-bytes", (guint64)0,
               NULL);

  g_object_set(encoder,
               "tune", "zerolatency",
               "speed-preset", "ultrafast",
               "bitrate", state->bitrate_kbps,
               "key-int-max", state->fps,
               NULL);

  g_object_set(pay,
               "pt", 96,
               "config-interval", -1,
               NULL);

  if (state->stun_server) {
    g_object_set(state->webrtcbin, "stun-server", state->stun_server, NULL);
  }
  g_object_set(state->webrtcbin, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);

  if (!gst_element_link_many(state->appsrc, queue, convert, encoder, parser, pay, NULL)) {
    LOG("Failed to link GStreamer elements");
    return false;
  }

  GstPad *srcpad = gst_element_get_static_pad(pay, "src");
  GstPad *sinkpad = gst_element_get_request_pad(state->webrtcbin, "send_rtp_sink_0");
  if (!srcpad || !sinkpad) {
    LOG("Failed to get RTP pads");
    if (srcpad) {
      gst_object_unref(srcpad);
    }
    if (sinkpad) {
      gst_object_unref(sinkpad);
    }
    return false;
  }

  if (gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK) {
    LOG("Failed to link RTP to webrtcbin");
    gst_object_unref(srcpad);
    gst_object_unref(sinkpad);
    return false;
  }

  gst_object_unref(srcpad);
  gst_object_unref(sinkpad);

  g_signal_connect(state->webrtcbin, "on-ice-candidate", G_CALLBACK(on_ice_candidate), state);

  bus = gst_element_get_bus(state->pipeline);
  gst_bus_add_watch(bus, bus_message_cb, state);
  gst_object_unref(bus);

  return true;
}

static void destroy_pipeline(AppState *state) {
  if (!state->pipeline) {
    return;
  }
  gst_element_set_state(state->pipeline, GST_STATE_NULL);
  gst_object_unref(state->pipeline);
  state->pipeline = NULL;
  state->appsrc = NULL;
  state->webrtcbin = NULL;
}

static void start_streaming(AppState *state) {
  if (!state->pipeline) {
    if (!create_pipeline(state)) {
      LOG("Pipeline creation failed");
      return;
    }
  }

  GstStateChangeReturn ret = gst_element_set_state(state->pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    LOG("Failed to start pipeline");
    return;
  }

  if (!state->render_running) {
    state->render_running = true;
    pthread_create(&state->render_thread, NULL, render_thread_main, state);
  }
}

static void stop_streaming(AppState *state) {
  if (state->render_running) {
    state->render_running = false;
    pthread_join(state->render_thread, NULL);
  }
  destroy_pipeline(state);
}

static void ws_drain_queue(AppState *state) {
  WsMessage *current = NULL;
  pthread_mutex_lock(&state->ws_lock);
  current = state->ws_head;
  state->ws_head = NULL;
  state->ws_tail = NULL;
  pthread_mutex_unlock(&state->ws_lock);

  while (current) {
    WsMessage *next = current->next;
    free(current->data);
    free(current);
    current = next;
  }
}

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
  (void)user;
  AppState *state = lws_context_user(lws_get_context(wsi));

  switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
      if (state->client_wsi) {
        const char *msg = "busy";
        lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION,
                         (unsigned char *)msg, strlen(msg));
        return -1;
      }
      state->client_wsi = wsi;
      g_main_context_invoke(state->main_context, handle_client_connected, state);
      break;
    case LWS_CALLBACK_RECEIVE: {
      char *payload = malloc(len + 1);
      memcpy(payload, in, len);
      payload[len] = '\0';

      IncomingMessage *incoming = calloc(1, sizeof(*incoming));
      incoming->state = state;
      incoming->message = payload;
      g_main_context_invoke(state->main_context, handle_ws_message, incoming);
      break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE: {
      WsMessage *msg = NULL;
      pthread_mutex_lock(&state->ws_lock);
      if (state->ws_head) {
        msg = state->ws_head;
        state->ws_head = msg->next;
        if (!state->ws_head) {
          state->ws_tail = NULL;
        }
      }
      pthread_mutex_unlock(&state->ws_lock);

      if (msg) {
        unsigned char *buffer = malloc(LWS_PRE + msg->len);
        memcpy(buffer + LWS_PRE, msg->data, msg->len);
        lws_write(wsi, buffer + LWS_PRE, msg->len, LWS_WRITE_TEXT);
        free(buffer);
        free(msg->data);
        free(msg);
        if (state->ws_head) {
          lws_callback_on_writable(wsi);
        }
      }
      break;
    }
    case LWS_CALLBACK_CLOSED:
      if (state->client_wsi == wsi) {
        state->client_wsi = NULL;
        g_main_context_invoke(state->main_context, handle_client_closed, state);
      }
      break;
    default:
      break;
  }

  return 0;
}

static void *ws_thread_main(void *data) {
  AppState *state = data;
  while (state->running) {
    lws_service(state->lws_context, 100);
  }
  return NULL;
}

static gboolean handle_sigint(gpointer data) {
  AppState *state = data;
  state->running = false;
  if (state->main_loop) {
    g_main_loop_quit(state->main_loop);
  }
  if (state->lws_context) {
    lws_cancel_service(state->lws_context);
  }
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  AppState state = {0};
  gst_init(&argc, &argv);

  load_config(&state);
  LOG("Starting cube server on port %d (%dx%d @ %dfps)", state.port, state.width, state.height,
      state.fps);

  state.main_context = g_main_context_default();
  state.main_loop = g_main_loop_new(state.main_context, FALSE);

  pthread_mutex_init(&state.ws_lock, NULL);

  struct lws_protocols protocols[] = {
      {"cube-ws", ws_callback, 0, 64 * 1024},
      {NULL, NULL, 0, 0}
  };

  struct lws_context_creation_info info = {0};
  info.port = state.port;
  info.protocols = protocols;
  info.gid = -1;
  info.uid = -1;
  info.user = &state;

  state.lws_context = lws_create_context(&info);
  if (!state.lws_context) {
    LOG("Failed to create WebSocket context");
    return 1;
  }

  state.running = true;
  pthread_create(&state.ws_thread, NULL, ws_thread_main, &state);

  g_unix_signal_add(SIGINT, handle_sigint, &state);
  g_unix_signal_add(SIGTERM, handle_sigint, &state);

  g_main_loop_run(state.main_loop);

  state.running = false;
  lws_cancel_service(state.lws_context);
  pthread_join(state.ws_thread, NULL);

  stop_streaming(&state);
  ws_drain_queue(&state);

  if (state.lws_context) {
    lws_context_destroy(state.lws_context);
  }

  pthread_mutex_destroy(&state.ws_lock);
  if (state.stun_server) {
    free(state.stun_server);
  }

  g_main_loop_unref(state.main_loop);

  return 0;
}
