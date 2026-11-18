// Regression probe for Player.screenshot with direct D3D11 decoding.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:media_kit/media_kit.dart';
import 'package:media_kit_video/media_kit_video.dart';

Future<void> main(List<String> arguments) async {
  WidgetsFlutterBinding.ensureInitialized();
  MediaKit.ensureInitialized();
  final sources = arguments.where((a) => !a.startsWith('--')).toList();
  final current = ValueNotifier<VideoController?>(null);
  runApp(MaterialApp(
      home: Scaffold(
          body: ValueListenableBuilder(
    valueListenable: current,
    builder: (_, controller, __) => controller == null
        ? const SizedBox()
        : Video(key: ObjectKey(controller), controller: controller),
  ))));
  final results = <Map<String, Object?>>[];
  final logs = File('screenshot-mpv.log').openWrite();
  for (var index = 0; index < sources.length; index++) {
    final errors = <String>[];
    final player = Player(
        configuration: const PlayerConfiguration(
      logLevel: MPVLogLevel.debug,
      libass: true,
    ));
    final subscriptions = <StreamSubscription<dynamic>>[
      player.stream.error.listen(errors.add),
      player.stream.log.listen((event) => logs.writeln('$index $event')),
    ];
    final controller = VideoController(player,
        configuration: const VideoControllerConfiguration(hwdec: 'd3d11va'));
    current.value = controller;
    final native = player.platform as NativePlayer;
    final result = <String, Object?>{
      'source': sources[index],
      'errors': errors
    };
    results.add(result);
    try {
      await player.setVolume(0);
      await player.setPlaylistMode(PlaylistMode.loop);
      await player.open(Media(sources[index]));
      await player.stream.position
          .firstWhere((p) => p.inMilliseconds > 500)
          .timeout(const Duration(seconds: 20));
      result['codec'] = await native.getProperty('current-tracks/video/codec');
      result['mpv'] = await native.getProperty('mpv-version');
      final width = int.parse(await native.getProperty('video-out-params/dw'));
      final height = int.parse(await native.getProperty('video-out-params/dh'));
      result['expectedSize'] = [width, height];
      result['hwdecBefore'] = await native.getProperty('hwdec-current');
      if (result['hwdecBefore'] != 'd3d11va') {
        throw StateError('Direct d3d11va not active');
      }
      if (arguments.contains('--screenshot-sw')) {
        await native.setProperty('screenshot-sw', 'yes');
      }
      final captures = <Map<String, Object?>>[];
      result['captures'] = captures;
      for (final paused in [false, true]) {
        if (paused) await player.pause();
        for (final format in ['image/jpeg', 'image/png', null]) {
          final capture = <String, Object?>{'format': format, 'paused': paused};
          captures.add(capture);
          final bytes = await player
              .screenshot(format: format)
              .timeout(const Duration(seconds: 20));
          capture['bytes'] = bytes?.length;
          if (bytes == null || bytes.isEmpty) {
            errors.add('Empty screenshot: $format, paused=$paused');
            continue;
          }
          if (format != null) {
            final codec = await ui.instantiateImageCodec(bytes);
            final frame = await codec.getNextFrame();
            capture['width'] = frame.image.width;
            capture['height'] = frame.image.height;
            if (frame.image.width != width || frame.image.height != height) {
              errors.add('Unexpected screenshot dimensions');
            }
            final pixels =
                (await frame.image.toByteData())!.buffer.asUint8List();
            final colorful = Iterable.generate(pixels.length ~/ 4, (i) => i * 4)
                .any((i) =>
                    pixels[i] != pixels[i + 1] ||
                    pixels[i + 1] != pixels[i + 2]);
            if (!colorful) errors.add('No color in decoded screenshot');
            capture['colorful'] = colorful;
            frame.image.dispose();
            codec.dispose();
            await File(
                    'screenshot-$index-${paused ? 'paused' : 'playing'}.${format == 'image/png' ? 'png' : 'jpg'}')
                .writeAsBytes(bytes);
          } else {
            final stride = bytes.length ~/ height;
            capture['stride'] = stride;
            if (bytes.length % height != 0 || stride < width * 4) {
              errors.add('Invalid raw frame layout');
            } else {
              var colorful = false;
              for (var y = 0; y < height && !colorful; y++) {
                for (var x = 0; x < width; x++) {
                  final i = y * stride + x * 4;
                  if (bytes[i] != bytes[i + 1] ||
                      bytes[i + 1] != bytes[i + 2]) {
                    colorful = true;
                    break;
                  }
                }
              }
              capture['colorful'] = colorful;
              if (!colorful) errors.add('No color in raw screenshot');
            }
          }
          capture['hwdecAfter'] = await native.getProperty('hwdec-current');
          if (capture['hwdecAfter'] != 'd3d11va') errors.add('Decoder changed');
        }
      }
      await player.setSubtitleTrack(SubtitleTrack.data(
          '1\n00:00:00,000 --> 00:00:30,000\nSCREENSHOT D3D11 TEST\n'));
      await Future<void>.delayed(const Duration(milliseconds: 500));
      final withoutSubtitles = await player.screenshot(format: 'image/png');
      final withSubtitles = await player.screenshot(
          format: 'image/png', includeLibassSubtitles: true);
      result['subtitleDifference'] = withoutSubtitles != null &&
          withSubtitles != null &&
          !listEquals(withoutSubtitles, withSubtitles);
      if (result['subtitleDifference'] != true) {
        errors.add('Missing subtitle difference');
      }
      if (withSubtitles != null) {
        await File('screenshot-$index-subtitles.png')
            .writeAsBytes(withSubtitles);
      }
      await player.play();
      await player.seek(const Duration(seconds: 1));
      final beforeResume = player.state.position;
      await Future<void>.delayed(const Duration(seconds: 1));
      result['playingAfter'] = player.state.playing;
      result['positionAdvanced'] = player.state.position != beforeResume;
      result['hwdecAfter'] = await native.getProperty('hwdec-current');
      if (!player.state.playing ||
          result['positionAdvanced'] != true ||
          result['hwdecAfter'] != 'd3d11va') {
        errors.add('Playback/decoder changed after screenshots');
      }
    } catch (error, stack) {
      errors.add('$error\n$stack');
    } finally {
      current.value = null;
      await WidgetsBinding.instance.endOfFrame;
      await player.dispose().timeout(const Duration(seconds: 20));
      for (final subscription in subscriptions) {
        await subscription.cancel();
      }
    }
    debugPrint('SCREENSHOT_TEST $result');
  }
  await logs.close();
  await File('screenshot-results.json')
      .writeAsString(const JsonEncoder.withIndent('  ').convert(results));
  debugPrint('SCREENSHOT_TEST COMPLETE');
}
