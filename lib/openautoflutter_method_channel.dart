import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'openautoflutter_platform_interface.dart';

/// An implementation of [OpenautoflutterPlatform] that uses method channels.
class MethodChannelOpenautoflutter extends OpenautoflutterPlatform {
  /// The method channel used to interact with the native platform.
  @visibleForTesting
  final methodChannel = const MethodChannel('openautoflutter');

  @override
  Future<String?> getPlatformVersion() async {
    final version = await methodChannel.invokeMethod<String>('getPlatformVersion');
    return version;
  }

  @override
  Future<int?> getVideoTextureId() async {
    final id = await methodChannel.invokeMethod<int>('getVideoTextureId');
    return id;
  }
}
