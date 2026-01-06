import 'package:flutter/material.dart';
import 'dart:async';

import 'package:flutter/services.dart';
import 'package:openautoflutter/openautoflutter.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  String _platformVersion = 'Unknown';
  final _openautoflutterPlugin = Openautoflutter();
  int? _videoTextureId;

  @override
  void initState() {
    super.initState();
    initPlatformState();
  }

  // Platform messages are asynchronous, so we initialize in an async method.
  Future<void> initPlatformState() async {
    String platformVersion;
  int? textureId;
    // Platform messages may fail, so we use a try/catch PlatformException.
    // We also handle the message potentially returning null.
    try {
      platformVersion =
          await _openautoflutterPlugin.getPlatformVersion() ?? 'Unknown platform version';
  textureId = await _openautoflutterPlugin.getVideoTextureId();
    } on PlatformException {
      platformVersion = 'Failed to get platform version.';
  textureId = null;
    }

    // If the widget was removed from the tree while the asynchronous platform
    // message was in flight, we want to discard the reply rather than calling
    // setState to update our non-existent appearance.
    if (!mounted) return;

    setState(() {
      _platformVersion = platformVersion;
  _videoTextureId = textureId;
    });
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(
          title: const Text('Plugin example app'),
        ),
        body: Center(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.center,
              children: [
                Text('Running on: $_platformVersion'),
                const SizedBox(height: 12),
                if (_videoTextureId == null)
                  const Text('Texture not available')
                else ...[
                  Text('Texture ID: $_videoTextureId'),
                  const SizedBox(height: 8),
                  // Render the native GL video texture with flex to avoid overflow.
                  Flexible(
                    child: AspectRatio(
                      aspectRatio: 16 / 9,
                      child: Texture(textureId: _videoTextureId!),
                    ),
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}
