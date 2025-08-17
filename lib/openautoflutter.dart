
import 'openautoflutter_platform_interface.dart';

class Openautoflutter {
  Future<String?> getPlatformVersion() {
    return OpenautoflutterPlatform.instance.getPlatformVersion();
  }

  Future<int?> getVideoTextureId() {
    return OpenautoflutterPlatform.instance.getVideoTextureId();
  }
}
