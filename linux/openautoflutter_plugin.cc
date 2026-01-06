#include "include/openautoflutter/openautoflutter_plugin.h"
#include "av/oa_video_texture.h"
#include "av/h264_decoder.h"
#include "transport.hpp"
#include "wire.hpp"
#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <glib-object.h>
#include <glib.h>
#include <sys/utsname.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

using OATransport = buzz::autoapp::Transport::Transport;
using OAMsgType = buzz::wire::MsgType;

#include "openautoflutter_plugin_private.h"

namespace {
// Short hex preview helper for debugging payloads.
std::string hex_head(const uint8_t* data, size_t size, size_t max_bytes = 32) {
  if (!data || size == 0) return "";
  std::ostringstream oss;
  const size_t n = std::min(size, max_bytes);
  for (size_t i = 0; i < n; ++i) {
    if (i) oss << ' ';
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
  }
  if (size > max_bytes) oss << " ...";
  return oss.str();
}
} // namespace

#define OPENAUTOFLUTTER_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), openautoflutter_plugin_get_type(), \
                              OpenautoflutterPlugin))

struct _OpenautoflutterPlugin {
  GObject parent_instance;
  OAVideoTexture* video_texture;
  int64_t texture_id;
  std::unique_ptr<OATransport> transport; // OpenAutoTransport receiver
  std::shared_ptr<H264Decoder> decoder; // shared to keep alive beyond plugin lifetime

  struct VideoFrameState {
    std::mutex mutex;
    std::vector<uint8_t> yuv; // packed YUV420P [Y][U][V]
    int width = 0;
    int height = 0;
    bool has_new = false;

    std::atomic<int> log_count{0};

    // Extract payload (optionally strip 8-byte ts + 4-byte payload header) and decode.
    void ingest_packet(const uint8_t* data, std::size_t size, H264Decoder& decoder) {
      if (!data || size == 0) return;

      const uint8_t* payload = data;
      std::size_t payload_size = size;
      bool stripped = false;
      uint32_t declared = 0;

      // Helper: find first Annex-B start code (3 or 4 bytes).
      auto find_start_code = [](const uint8_t* p, size_t n) -> size_t {
        for (size_t i = 0; i + 3 < n; ++i) {
          if (p[i] == 0 && p[i + 1] == 0 && ((p[i + 2] == 1) || (p[i + 2] == 0 && p[i + 3] == 1))) {
            return i;
          }
        }
        return n;
      };

      // Common OAT framing: [u64 ts][u32 payload_size][payload...]
      if (size >= sizeof(uint64_t) + sizeof(uint32_t)) {
        std::memcpy(&declared, data + sizeof(uint64_t), sizeof(uint32_t));
        if (declared > 0 && declared <= size - (sizeof(uint64_t) + sizeof(uint32_t))) {
          payload = data + sizeof(uint64_t) + sizeof(uint32_t);
          payload_size = declared;
          stripped = true;
        }
      }

      // If not stripped yet, try to drop any leading non-start-code bytes (e.g., 4-byte length + nonce).
      if (!stripped) {
        size_t idx = find_start_code(payload, payload_size);
        if (idx != 0 && idx < payload_size) {
          payload += idx;
          payload_size -= idx;
          stripped = true;
        }
      }

      int log_id = log_count.fetch_add(1, std::memory_order_relaxed);
      if (log_id < 8) {
        std::cout << "[VideoFrameState] in_size=" << size
                  << " payload_size=" << payload_size
                  << " stripped=" << (stripped ? 1 : 0)
                  << " head=" << hex_head(payload, payload_size, 24)
                  << std::endl;
      }

      int w = 0, h = 0;
      std::vector<uint8_t> decoded;
      if (!decoder.decode_to_yuv420p(payload, payload_size, decoded, w, h)) {
        if (log_id < 8) {
          std::cout << "[VideoFrameState] decode failed size=" << payload_size
                    << " declared=" << declared << std::endl;
        }
        return;
      }

      std::lock_guard<std::mutex> lk(mutex);
      width = w;
      height = h;
      yuv.swap(decoded);
      has_new = true;
      if (log_id < 8) {
        std::cout << "[VideoFrameState] decoded " << w << "x" << h
                  << " bytes=" << yuv.size() << std::endl;
      }
    }

    bool take_latest(std::vector<uint8_t>& out, int& w, int& h) {
      std::lock_guard<std::mutex> lk(mutex);
      if (!has_new || yuv.empty() || width <= 0 || height <= 0) return false;
      out = yuv; // copy to avoid holding lock during GL upload
      w = width;
      h = height;
      has_new = false;
      return true;
    }
  };

  std::shared_ptr<VideoFrameState> frame_state;
  FlTextureRegistrar* texture_registrar; // to mark frames available
  guint frame_timer_id; // periodic pump for decoded frames
};

// Convenience alias for the nested frame holder type.
using VideoFrameState = _OpenautoflutterPlugin::VideoFrameState;

G_DEFINE_TYPE(OpenautoflutterPlugin, openautoflutter_plugin, g_object_get_type())

// Called when a method call is received from Flutter.
static void openautoflutter_plugin_handle_method_call(
    OpenautoflutterPlugin* self,
    FlMethodCall* method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;

  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "getPlatformVersion") == 0) {
    response = get_platform_version();
  } else if (strcmp(method, "getVideoTextureId") == 0) {
    // Return the registered Flutter texture ID so Dart can render it via a Texture widget.
    g_autoptr(FlValue) result = fl_value_new_int(self->texture_id);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

FlMethodResponse* get_platform_version() {
  struct utsname uname_data = {};
  uname(&uname_data);
  g_autofree gchar *version = g_strdup_printf("Linux %s", uname_data.version);
  g_autoptr(FlValue) result = fl_value_new_string(version);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static void openautoflutter_plugin_dispose(GObject* object) {
  OpenautoflutterPlugin* self = OPENAUTOFLUTTER_PLUGIN(object);
  if (self->frame_timer_id) {
    g_source_remove(self->frame_timer_id);
    self->frame_timer_id = 0;
  }
  if (self->transport) {
    self->transport->stop();
  }
  if (self->video_texture != nullptr) {
    g_clear_object(&self->video_texture);
  }
  if (self->transport) {
    self->transport.reset();
  }
  self->decoder.reset();
  G_OBJECT_CLASS(openautoflutter_plugin_parent_class)->dispose(object);
}

static void openautoflutter_plugin_class_init(OpenautoflutterPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = openautoflutter_plugin_dispose;
}

static void openautoflutter_plugin_init(OpenautoflutterPlugin* self) {
  self->video_texture = nullptr;
  self->texture_id = 0;
  self->transport = std::make_unique<OATransport>();
  self->decoder = std::make_shared<H264Decoder>();
  self->frame_state = std::make_shared<VideoFrameState>();
  self->texture_registrar = nullptr;
  self->frame_timer_id = 0;

  // Start as Side B (joiner) with explicit 5s wait and 1ms poll.
  g_message("OAT: starting transport as Side B (wait=5000ms poll=1000us)");
  if (!self->transport->startAsB(std::chrono::milliseconds{5000}, std::chrono::microseconds{1000})) {
    g_warning("OAT: startAsB failed");
  } else {
    g_message("OAT: transport started (side=%d, running=%d)", static_cast<int>(self->transport->side()),
              self->transport->isRunning() ? 1 : 0);
    // Register handler for VIDEO messages: strip header if present, decode, stash latest.
    auto decoder = self->decoder;
    auto state = self->frame_state;
    g_object_ref(self); // keep plugin alive while transport callbacks run
    auto self_shared = std::shared_ptr<OpenautoflutterPlugin>(
      self,
      [](OpenautoflutterPlugin* p){ g_object_unref(p); });

    self->transport->addTypeHandler(static_cast<OAMsgType>(OAMsgType::VIDEO),
      [decoder, state, self_shared](uint64_t /*ts*/, const void* data, std::size_t size) {
        if (!data || size == 0 || !decoder || !state || !self_shared) return;
        state->ingest_packet(static_cast<const uint8_t*>(data), size, *decoder);
      });
  }
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  OpenautoflutterPlugin* plugin = OPENAUTOFLUTTER_PLUGIN(user_data);
  openautoflutter_plugin_handle_method_call(plugin, method_call);
}

// Periodically pump decoded frames from AVConsumer into the Flutter texture.
static gboolean pump_video_frame_cb(gpointer user_data) {
  OpenautoflutterPlugin* self = OPENAUTOFLUTTER_PLUGIN(user_data);
  if (!self || !self->video_texture || !self->texture_registrar) {
    return TRUE; // keep the timer; environment not ready yet
  }

  std::vector<uint8_t> frame;
  int w = 0, h = 0;
  if (self->frame_state && self->frame_state->take_latest(frame, w, h)) {
    const gsize need = static_cast<gsize>(w) * static_cast<gsize>(h) * 3u / 2u;
    if (frame.size() >= need) {
      oa_video_texture_set_yuv420p_frame(self->video_texture,
                                         reinterpret_cast<const guint8*>(frame.data()),
                                         static_cast<gsize>(frame.size()),
                                         w,
                                         h);
      oa_video_texture_mark_frame_available(self->video_texture, self->texture_registrar);
    }
  }
  return TRUE; // continue calling
}

void openautoflutter_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  OpenautoflutterPlugin* plugin = OPENAUTOFLUTTER_PLUGIN(
      g_object_new(openautoflutter_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "openautoflutter",
                            FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  // Register the GL texture so Flutter can render it via a Texture widget.
  FlTextureRegistrar* texture_registrar =
      fl_plugin_registrar_get_texture_registrar(registrar);
  plugin->video_texture = oa_video_texture_new(1, 1);
  plugin->texture_id = oa_video_texture_register(plugin->video_texture, texture_registrar);
  plugin->texture_registrar = texture_registrar;

  // Start a 60 FPS timer to feed frames to Flutter when available.
  plugin->frame_timer_id = g_timeout_add(16, pump_video_frame_cb, plugin);

  g_object_unref(plugin);
}
