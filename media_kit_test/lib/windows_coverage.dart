// Additional real-engine checks not covered by windows_regression.dart.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:media_kit/media_kit.dart';
import 'package:media_kit_video/media_kit_video.dart';

void main(List<String> arguments) {
  WidgetsFlutterBinding.ensureInitialized();
  MediaKit.ensureInitialized();
  runApp(MaterialApp(
    theme: ThemeData.dark(),
    home: CoverageScreen(
      sources: arguments.where((a) => !a.startsWith('--')).toList(),
      software: arguments.contains('--software'),
      rotationOnly: arguments.contains('--rotation-only'),
    ),
  ));
}

class CoverageScreen extends StatefulWidget {
  final List<String> sources;
  final bool software;
  final bool rotationOnly;
  const CoverageScreen(
      {super.key,
      required this.sources,
      required this.software,
      this.rotationOnly = false});

  @override
  State<CoverageScreen> createState() => _CoverageScreenState();
}

class _Slot {
  final Player player;
  final VideoController controller;
  final subscriptions = <StreamSubscription<dynamic>>[];
  int progress = 0;
  _Slot(this.player, this.controller);
}

class _CoverageScreenState extends State<CoverageScreen> {
  static const window = MethodChannel('media_kit_test/window');
  final slots = List<_Slot?>.filled(6, null);
  final events = <Map<String, Object?>>[];
  final errors = <String>[];
  final memory = <Map<String, Object?>>[];
  final viewportKey = GlobalKey();
  BoxFit? probeFit;
  String status = 'Starting';
  String get hwdec => widget.software ? 'no' : 'd3d11va';
  String get mode => widget.software ? 'sw-cpu' : 'hw';
  late final void Function(FlutterErrorDetails)? previousError;

  @override
  void initState() {
    super.initState();
    previousError = FlutterError.onError;
    FlutterError.onError = (details) {
      errors.add(details.toString());
      previousError?.call(details);
    };
    WidgetsBinding.instance.addPostFrameCallback((_) => unawaited(run()));
  }

  void record(String name, [Object? detail]) {
    events.add({
      'time': DateTime.now().toIso8601String(),
      'name': name,
      'detail': detail
    });
    debugPrint('COVERAGE $name ${jsonEncode(detail)}');
    if (mounted) setState(() => status = name);
  }

  void check(bool value, String message) {
    if (!value) throw StateError(message);
  }

  Future<void> waitFor(bool Function() condition, String message) async {
    final deadline = DateTime.now().add(const Duration(seconds: 20));
    while (!condition()) {
      if (DateTime.now().isAfter(deadline)) throw TimeoutException(message);
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
  }

  _Slot createSlot() {
    final player = Player();
    final controller = VideoController(player,
        configuration: VideoControllerConfiguration(
          hwdec: hwdec,
          enableHardwareAcceleration: !widget.software,
        ));
    final slot = _Slot(player, controller);
    slot.subscriptions
        .add(player.stream.position.listen((_) => slot.progress++));
    slot.subscriptions.add(player.stream.error.listen(errors.add));
    return slot;
  }

  Future<void> open(_Slot slot, String source) async {
    await slot.player.setVolume(0);
    await slot.player.setPlaylistMode(PlaylistMode.loop);
    await slot.player.open(Media(source)).timeout(const Duration(seconds: 20));
    await waitFor(
        () =>
            slot.player.state.position.inMilliseconds > 100 &&
            (slot.controller.rect.value?.width ?? 0) > 1,
        'video started');
  }

  Future<void> openSix() async {
    for (var i = 0; i < slots.length; i++) {
      if (slots[i] != null) continue;
      final slot = createSlot();
      setState(() => slots[i] = slot);
      await open(slot, widget.sources[i % widget.sources.length]);
    }
  }

  Future<void> disposeSlot(_Slot slot) async {
    await slot.player.dispose().timeout(const Duration(seconds: 10));
    for (final subscription in slot.subscriptions) {
      await subscription.cancel();
    }
  }

  Future<void> disposeSix() async {
    final previous = slots.whereType<_Slot>().toList();
    setState(() {
      probeFit = null;
      slots.fillRange(0, slots.length, null);
    });
    await WidgetsBinding.instance.endOfFrame;
    final timer = Stopwatch()..start();
    await Future.wait(previous.map(disposeSlot));
    record('Disposed all concurrently',
        {'count': previous.length, 'ms': timer.elapsedMilliseconds});
  }

  Future<void> progress([int milliseconds = 1200]) async {
    final before = {
      for (final slot in slots.whereType<_Slot>()) slot: slot.progress
    };
    await Future<void>.delayed(Duration(milliseconds: milliseconds));
    for (final entry in before.entries) {
      check(entry.key.progress > entry.value, 'A player stopped advancing');
    }
  }

  Future<Map<String, dynamic>> metrics() async => {
        ...Map<String, dynamic>.from(
            (await window.invokeMapMethod<String, dynamic>('metrics'))!),
        'rss': ProcessInfo.currentRss,
      };

  Future<void> windowModes() async {
    for (var i = 0; i < 3; i++) {
      await window.invokeMethod<void>('fullscreen', true);
      await progress();
      check((await metrics())['fullscreen'] == true, 'Fullscreen not entered');
      await window.invokeMethod<void>('fullscreen', false);
      await progress();
      check(
          (await metrics())['fullscreen'] == false, 'Fullscreen not restored');
    }
    await window.invokeMethod<void>('minimize');
    check((await metrics())['minimized'] == true, 'Window not minimized');
    await Future<void>.delayed(const Duration(seconds: 2));
    await window.invokeMethod<void>('restore');
    await progress();
    check((await metrics())['minimized'] == false, 'Window not restored');
    record('Fullscreen x3 and minimize/restore PASS');
  }

  Size expectedSize(_Slot slot) {
    final v = slot.player.state.videoParams;
    var w = (v.dw ?? 0).toDouble(), h = (v.dh ?? 0).toDouble();
    if (v.rotate == 90 || v.rotate == 270) {
      final old = w;
      w = h;
      h = old;
    }
    if (widget.software && (w > 1920 || h > 1080)) {
      final scale = (1920 / w) < (1080 / h) ? 1920 / w : 1080 / h;
      w = (w * scale).floorToDouble();
      h = (h * scale).floorToDouble();
    }
    return Size(w, h);
  }

  Future<void> sourceChanges({bool rotatedOnly = false}) async {
    final slot = slots.first!;
    var screenshots = 0;
    final sources = [
      if (!rotatedOnly) ...widget.sources.take(4),
      '${File(widget.sources.first).parent.path}/landscape-rotated.mp4',
    ];
    for (final fixed in [true, false]) {
      if (fixed) {
        await slot.controller.setSize(width: 300, height: 400);
      } else {
        await slot.controller.setSize();
      }
      for (final source in sources) {
        await slot.player.open(Media(source));
        await waitFor(() => slot.player.state.position.inMilliseconds > 100,
            'new source playing');
        await waitFor(
            () =>
                slot.controller.rect.value?.size ==
                (fixed ? const Size(300, 400) : expectedSize(slot)),
            'new source texture');
        if (source.endsWith('landscape-rotated.mp4')) {
          check((slot.player.state.videoParams.rotate ?? 0) % 180 == 90,
              'Fixture has no rotation metadata');
          final expected = fixed
              ? const Size(300, 400)
              : widget.software
                  ? const Size(607, 1080)
                  : const Size(720, 1280);
          check(slot.controller.rect.value?.size == expected,
              'Incorrect rotated texture size');
        }
        final native = slot.player.platform as NativePlayer;
        final decoder = await native.getProperty('hwdec-current');
        check(decoder == hwdec, 'Requested $hwdec, actual $decoder');
        record('Source switch', {
          'fixed': fixed,
          'codec': await native.getProperty('current-tracks/video/codec'),
          'hwdec': decoder,
          'texture': [
            slot.controller.rect.value?.width,
            slot.controller.rect.value?.height
          ]
        });
        for (final format in ['image/jpeg', 'image/png', null]) {
          final bytes = await slot.player
              .screenshot(format: format)
              .timeout(const Duration(seconds: 10));
          check(bytes != null && bytes.isNotEmpty,
              'Empty screenshot after source switch');
          screenshots++;
        }
        await progress(500);
      }
    }
    record('Fixed/auto source switching and rotation PASS',
        {'screenshots': screenshots});
  }

  Future<void> playbackControls() async {
    final slot = slots.first!;
    final native = slot.player.platform as NativePlayer;
    await slot.player.pause();
    await Future<void>.delayed(const Duration(milliseconds: 250));
    final paused = slot.player.state.position;
    final others = {
      for (final other in slots.skip(1).whereType<_Slot>())
        other: other.progress
    };
    await Future<void>.delayed(const Duration(milliseconds: 700));
    check(!slot.player.state.playing && slot.player.state.position == paused,
        'Paused video kept advancing');
    for (final entry in others.entries) {
      check(entry.key.progress > entry.value,
          'Pausing one player stopped another');
    }
    await slot.controller.setSize(width: 641, height: 359);
    await waitFor(
        () => slot.controller.rect.value?.size == const Size(641, 359),
        'paused odd-size resize');
    await slot.player.seek(const Duration(seconds: 2));
    await waitFor(
        () => (slot.player.state.position.inMilliseconds - 2000).abs() <= 150,
        'seek while paused');
    for (final rate in [0.5, 2.0, 1.0]) {
      await slot.player.setRate(rate);
      final actual = double.parse(await native.getProperty('speed'));
      check((actual - rate).abs() < 0.001, 'Rate not applied');
    }
    final screenshot = await slot.player.screenshot();
    check(screenshot != null && screenshot.isNotEmpty,
        'Paused screenshot is empty');
    await slot.controller.setSize();
    await slot.player.play();
    await progress();
    record(
        'Pause isolation, odd-size resize, exact seek, speed and resume PASS');
  }

  Future<void> rotationPixels() async {
    final slot = slots.first!;
    final native = slot.player.platform as NativePlayer;
    final directory = File(widget.sources.first).parent.path;
    const expectedColors = [
      [0, 1, 2, 3],
      [1, 3, 0, 2],
      [3, 2, 1, 0],
      [2, 0, 3, 1],
    ];
    const rgb = [
      [255, 0, 0],
      [0, 255, 0],
      [0, 0, 255],
      [255, 255, 0],
    ];
    await native.setProperty('keepaspect', 'yes');
    for (final codec in ['h264', 'hevc']) {
      for (final degrees in [0, 90, 180, 270]) {
        final portrait = degrees % 180 == 90;
        final display = portrait ? const Size(180, 320) : const Size(320, 180);
        // Mix local files and HTTP without changing the underlying bitstream.
        final name = 'rotation-$codec-$degrees.mp4';
        final source = degrees == 180 || degrees == 270
            ? Uri.parse(widget.sources[2]).resolve(name).toString()
            : '$directory/$name';
        await slot.player.open(Media(source));
        await waitFor(() => slot.player.state.position.inMilliseconds > 100,
            'rotated source playing');
        check(await native.getProperty('hwdec-current') == hwdec,
            'Rotation changed the decoder');
        check(await native.getProperty('current-tracks/video/codec') == codec,
            'Unexpected rotation fixture codec');
        for (final fixed in [true, false]) {
          await slot.controller
              .setSize(width: fixed ? 300 : null, height: fixed ? 400 : null);
          final texture = fixed ? const Size(300, 400) : display;
          await waitFor(() => slot.controller.rect.value?.size == texture,
              'rotated texture dimensions');
          setState(() => probeFit = BoxFit.contain);
          await Future<void>.delayed(const Duration(milliseconds: 500));
          if (!mounted) throw StateError('Rotation view was disposed');
          final box =
              viewportKey.currentContext!.findRenderObject()! as RenderBox;
          final origin = box.localToGlobal(Offset.zero);
          final dpi = View.of(context).devicePixelRatio;
          final capture =
              (await window.invokeMapMethod<String, dynamic>('capture'))!;
          final width = capture['width'] as int;
          final height = capture['height'] as int;
          final bytes = capture['bgra'] as Uint8List;
          final imageBytes = Uint8List.fromList(bytes);
          for (var i = 3; i < imageBytes.length; i += 4) {
            imageBytes[i] = 255;
          }
          final image = Completer<ui.Image>();
          ui.decodeImageFromPixels(imageBytes, width, height,
              ui.PixelFormat.bgra8888, image.complete);
          final bitmap = await image.future;
          final png = await bitmap.toByteData(format: ui.ImageByteFormat.png);
          await File('$mode-rotation-$codec-$degrees-$fixed.png')
              .writeAsBytes(png!.buffer.asUint8List());
          bitmap.dispose();
          final fit =
              applyBoxFit(BoxFit.contain, display, box.size).destination;
          final left = origin.dx + (box.size.width - fit.width) / 2;
          final top = origin.dy + (box.size.height - fit.height) / 2;
          for (var quadrant = 0; quadrant < 4; quadrant++) {
            final x =
                ((left + fit.width * (quadrant % 2 == 0 ? 0.25 : 0.75)) * dpi)
                    .round();
            final y = ((top + fit.height * (quadrant < 2 ? 0.25 : 0.75)) * dpi)
                .round();
            check(x >= 0 && y >= 0 && x < width && y < height,
                'Rotation sample outside window');
            final i = (y * width + x) * 4;
            final actual = [bytes[i + 2], bytes[i + 1], bytes[i]];
            final expected = rgb[expectedColors[degrees ~/ 90][quadrant]];
            check(
                List.generate(3, (c) => (actual[c] - expected[c]).abs() <= 45)
                    .every((matches) => matches),
                'Wrong rotation: $codec $degrees fixed=$fixed '
                'quadrant=$quadrant actual=$actual expected=$expected');
          }
          record('Rotated window pixels PASS', {
            'codec': codec,
            'degrees': degrees,
            'fixed': fixed,
            'network': source.startsWith('http'),
            'hwdec': hwdec,
          });
          final screenshot = await slot.player.screenshot(format: 'image/png');
          check(screenshot != null && screenshot.isNotEmpty,
              'Empty rotated screenshot');
          await progress(500);
        }
      }
    }
    setState(() => probeFit = null);
    record('H264/HEVC rotation 0/90/180/270 local/HTTP pixels PASS');
  }

  Future<void> sequentialSoftware() async {
    for (var i = 0; i < slots.length; i++) {
      final old = slots[i]!;
      setState(() => slots[i] = null);
      await WidgetsBinding.instance.endOfFrame;
      await disposeSlot(old);
      await progress(5000);
      final next = createSlot();
      setState(() => slots[i] = next);
      await open(next, widget.sources[i % widget.sources.length]);
      record('CPU: restored player after 5 seconds', i + 1);
    }
  }

  Future<void> presentation() async {
    final slot = slots.first!;
    final native = slot.player.platform as NativePlayer;
    final directory = File(widget.sources.first).parent.path;
    for (final codec in ['h264', 'hevc']) {
      await slot.controller.setSize();
      await slot.player.open(Media('$directory/aspect-$codec.mp4'));
      await waitFor(
          () => slot.controller.rect.value?.size == const Size(1600, 900),
          'aspect fixture');
      for (final variant in [
        'contain',
        'cover',
        'fill',
        'texture-bars',
        'texture-stretch'
      ]) {
        final fixed = variant.startsWith('texture-');
        await native.setProperty(
            'keepaspect', variant == 'texture-stretch' ? 'no' : 'yes');
        if (fixed) {
          await slot.controller.setSize(width: 300, height: 400);
        } else {
          await slot.controller.setSize();
        }
        await waitFor(
            () =>
                slot.controller.rect.value?.size ==
                (fixed ? const Size(300, 400) : const Size(1600, 900)),
            'aspect texture size');
        setState(() => probeFit = switch (variant) {
              'contain' => BoxFit.contain,
              'cover' => BoxFit.cover,
              _ => BoxFit.fill,
            });
        await Future<void>.delayed(const Duration(milliseconds: 600));
        await captureAndCheck('$codec-$variant', variant);
      }
    }
    await native.setProperty('keepaspect', 'yes');
    await slot.controller.setSize();
    setState(() => probeFit = null);
    record(
        'Composed window: contain/cover/fill and fixed-texture bars/stretch PASS');
  }

  Future<void> captureAndCheck(String name, String variant) async {
    final box = viewportKey.currentContext!.findRenderObject()! as RenderBox;
    final origin = box.localToGlobal(Offset.zero);
    final scale = View.of(context).devicePixelRatio;
    final capture = (await window.invokeMapMethod<String, dynamic>('capture'))!;
    final width = capture['width'] as int, height = capture['height'] as int;
    final bytes = Uint8List.fromList(capture['bgra'] as Uint8List);
    // GDI captures have no meaningful alpha; keep their RGB values unchanged.
    for (var i = 3; i < bytes.length; i += 4) {
      bytes[i] = 255;
    }
    final image = Completer<ui.Image>();
    ui.decodeImageFromPixels(
        bytes, width, height, ui.PixelFormat.bgra8888, image.complete);
    final bitmap = await image.future;
    final png = await bitmap.toByteData(format: ui.ImageByteFormat.png);
    await File('$mode-$name.png').writeAsBytes(png!.buffer.asUint8List());
    bitmap.dispose();
    var minX = 100000, maxX = -1, minY = 100000, maxY = -1;
    var whiteMinX = 100000, whiteMaxX = -1, whiteMinY = 100000, whiteMaxY = -1;
    for (var y = (origin.dy * scale).round();
        y < ((origin.dy + box.size.height) * scale).round();
        y++) {
      for (var x = (origin.dx * scale).round();
          x < ((origin.dx + box.size.width) * scale).round();
          x++) {
        check(x >= 0 && y >= 0 && x < width && y < height,
            'Viewport outside capture');
        final i = (y * width + x) * 4;
        final b = bytes[i], g = bytes[i + 1], r = bytes[i + 2];
        final red = r > 180 && g < 70 && b < 70;
        final white = r > 210 && g > 210 && b > 210;
        if (red || white) {
          if (x < minX) minX = x;
          if (x > maxX) maxX = x;
          if (y < minY) minY = y;
          if (y > maxY) maxY = y;
        }
        if (white) {
          if (x < whiteMinX) whiteMinX = x;
          if (x > whiteMaxX) whiteMaxX = x;
          if (y < whiteMinY) whiteMinY = y;
          if (y > whiteMaxY) whiteMaxY = y;
        }
      }
    }
    final content = Size((maxX - minX + 1) / scale, (maxY - minY + 1) / scale);
    final square = Size((whiteMaxX - whiteMinX + 1) / scale,
        (whiteMaxY - whiteMinY + 1) / scale);
    final bars = variant == 'contain' || variant == 'texture-bars';
    final cover = variant == 'cover';
    final expectedContent = Size(300, bars ? 168.75 : 400);
    final expectedSquare = Size(cover ? 88.89 : 37.5, bars ? 37.5 : 88.89);
    record('Window pixel bounds', {
      'case': name,
      'content': [content.width, content.height],
      'square': [square.width, square.height]
    });
    check(
        (content.width - expectedContent.width).abs() <= 4 &&
            (content.height - expectedContent.height).abs() <= 4 &&
            (square.width - expectedSquare.width).abs() <= 4 &&
            (square.height - expectedSquare.height).abs() <= 4,
        'Unexpected presentation: $name');
  }

  Future<void> routeCycles() async {
    final navigator = Navigator.of(context);
    await disposeSix();
    for (var i = 0; i < 8; i++) {
      final slot = createSlot();
      final route = navigator.push<void>(PageRouteBuilder(
        transitionDuration: Duration.zero,
        reverseTransitionDuration: Duration.zero,
        pageBuilder: (_, __, ___) => Scaffold(
            body:
                Video(controller: slot.controller, controls: NoVideoControls)),
      ));
      await open(slot, widget.sources[i % widget.sources.length]);
      navigator.pop();
      await route;
      await WidgetsBinding.instance.endOfFrame;
      await disposeSlot(slot);
    }
    record('Open/play/pop/dispose routes x8 PASS');
    for (var i = 0; i < 12; i++) {
      final player = Player();
      final controller = VideoController(player,
          configuration: VideoControllerConfiguration(
              hwdec: hwdec, enableHardwareAcceleration: !widget.software));
      final cancelled =
          controller.platform.future.then((_) => false, onError: (Object e) {
        check(e is StateError, 'Unexpected cancellation: $e');
        return true;
      });
      await player.dispose().timeout(const Duration(seconds: 10));
      check(await cancelled,
          'Controller unexpectedly initialized without a frame');
    }
    record('Dispose before first Flutter frame x12 PASS');
  }

  Future<void> resourceCycles() async {
    for (var cycle = 0; cycle < 6; cycle++) {
      await openSix();
      await progress(500);
      await disposeSix();
      // NativePlayer currently schedules mpv destruction five seconds later.
      await Future<void>.delayed(const Duration(seconds: 7));
      final sample = <String, Object?>{'cycle': cycle, ...await metrics()};
      memory.add(sample);
      record('Settled resource sample', sample);
    }
    record('Six-player create/dispose x6 PASS',
        'Resource samples are observations, not a proof of absence of leaks.');
  }

  Future<void> run() async {
    var passed = false;
    try {
      check(widget.sources.length >= 4,
          'Pass at least four mixed local/HTTP sources');
      await window.invokeMethod<void>('restore');
      await openSix();
      await progress();
      if (widget.rotationOnly) {
        await sourceChanges(rotatedOnly: true);
        await rotationPixels();
        await playbackControls();
      } else {
        await windowModes();
        await sourceChanges();
        await rotationPixels();
        await playbackControls();
        if (widget.software) await sequentialSoftware();
        await presentation();
        await routeCycles();
        await resourceCycles();
        await openSix();
      }
      await progress();
      check(errors.isEmpty, 'Asynchronous errors: $errors');
      record('Ready to close with six playing');
      passed = true;
    } catch (error, stack) {
      errors.add('$error\n$stack');
      record('FAIL', error.toString());
      await disposeSix();
    } finally {
      await File('coverage-$mode.json')
          .writeAsString(const JsonEncoder.withIndent('  ').convert({
        'status': passed ? 'PASS' : 'FAIL',
        'mode': mode,
        'errors': errors,
        'events': events,
        'resources': memory,
        'closeWhilePlayingRequested': passed,
      }));
      // The external runner separately checks that the process exits normally.
      await window.invokeMethod<void>('close');
    }
  }

  @override
  void dispose() {
    FlutterError.onError = previousError;
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
        appBar: AppBar(title: Text('$mode · $status')),
        body: probeFit != null
            ? Center(
                child: SizedBox(
                    key: viewportKey,
                    width: 300,
                    height: 400,
                    child: Video(
                        controller: slots.first!.controller,
                        fit: probeFit!,
                        controls: NoVideoControls)))
            : GridView.count(
                crossAxisCount: 3,
                childAspectRatio: 1.5,
                children: [
                    for (final slot in slots)
                      slot == null
                          ? const SizedBox()
                          : Video(
                              key: ObjectKey(slot),
                              controller: slot.controller,
                              controls: NoVideoControls)
                  ]),
      );
}
