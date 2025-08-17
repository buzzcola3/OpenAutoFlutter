import 'package:flutter_test/flutter_test.dart';
import 'package:openautoflutter/openautoflutter.dart';
import 'package:openautoflutter/openautoflutter_platform_interface.dart';
import 'package:openautoflutter/openautoflutter_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockOpenautoflutterPlatform
    with MockPlatformInterfaceMixin
    implements OpenautoflutterPlatform {

  @override
  Future<String?> getPlatformVersion() => Future.value('42');
}

void main() {
  final OpenautoflutterPlatform initialPlatform = OpenautoflutterPlatform.instance;

  test('$MethodChannelOpenautoflutter is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelOpenautoflutter>());
  });

  test('getPlatformVersion', () async {
    Openautoflutter openautoflutterPlugin = Openautoflutter();
    MockOpenautoflutterPlatform fakePlatform = MockOpenautoflutterPlatform();
    OpenautoflutterPlatform.instance = fakePlatform;

    expect(await openautoflutterPlugin.getPlatformVersion(), '42');
  });
}
