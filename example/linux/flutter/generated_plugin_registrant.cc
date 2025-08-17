//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <openautoflutter/openautoflutter_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) openautoflutter_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "OpenautoflutterPlugin");
  openautoflutter_plugin_register_with_registrar(openautoflutter_registrar);
}
