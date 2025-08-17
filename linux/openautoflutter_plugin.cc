#include "include/openautoflutter/openautoflutter_plugin.h"
#include "av/oa_video_texture.h"
#include "av/av_consumer.h"
#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <glib-object.h>
#include <glib.h>
#include <sys/utsname.h>

#include <cstring>

#include "openautoflutter_plugin_private.h"

#define OPENAUTOFLUTTER_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), openautoflutter_plugin_get_type(), \
                              OpenautoflutterPlugin))

struct _OpenautoflutterPlugin {
  GObject parent_instance;
  OAVideoTexture* video_texture;
  int64_t texture_id;
  AVConsumer* av_consumer; // runs background SHM consumers
  FlTextureRegistrar* texture_registrar; // to mark frames available
  guint frame_timer_id; // periodic pump for decoded frames
};

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
  if (self->video_texture != nullptr) {
    g_clear_object(&self->video_texture);
  }
  // Intentionally do not delete av_consumer here: its threads block on semaphores
  // and we don't have a stop signal. Let the process teardown reclaim it.
  G_OBJECT_CLASS(openautoflutter_plugin_parent_class)->dispose(object);
}

static void openautoflutter_plugin_class_init(OpenautoflutterPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = openautoflutter_plugin_dispose;
}

static void openautoflutter_plugin_init(OpenautoflutterPlugin* self) {
  self->video_texture = nullptr;
  self->texture_id = 0;
  self->av_consumer = new AVConsumer();
  self->av_consumer->start();
  self->texture_registrar = nullptr;
  self->frame_timer_id = 0;
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  OpenautoflutterPlugin* plugin = OPENAUTOFLUTTER_PLUGIN(user_data);
  openautoflutter_plugin_handle_method_call(plugin, method_call);
}

// Periodically pump decoded frames from AVConsumer into the Flutter texture.
static gboolean pump_video_frame_cb(gpointer user_data) {
  OpenautoflutterPlugin* self = OPENAUTOFLUTTER_PLUGIN(user_data);
  if (!self || !self->av_consumer || !self->video_texture || !self->texture_registrar) {
    return TRUE; // keep the timer; environment not ready yet
  }
  if (self->av_consumer->is_new_frame_available()) {
    const uint8_t* data = nullptr; int w = 0; int h = 0; size_t size = 0;
    if (self->av_consumer->get_last_yuv420p(data, w, h, size) && data && size > 0) {
      oa_video_texture_set_yuv420p_frame(self->video_texture, reinterpret_cast<const guint8*>(data), (gsize)size, w, h);
      oa_video_texture_mark_frame_available(self->video_texture, self->texture_registrar);
    }
    self->av_consumer->mark_frame_consumed();
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
