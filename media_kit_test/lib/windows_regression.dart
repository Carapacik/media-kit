// Standalone, repeatable Windows renderer exercise. See docs/WINDOWS_TESTING.md.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:media_kit/media_kit.dart';
import 'package:media_kit_video/media_kit_video.dart';

void main(List<String> arguments) {
  WidgetsFlutterBinding.ensureInitialized();
  MediaKit.ensureInitialized();
  final files = arguments.where((e) => !e.startsWith('--')).toList();
  runApp(MaterialApp(
    theme: ThemeData.dark(useMaterial3: true),
    home: WindowsRegression(
      files: files,
      software: arguments.contains('--software'),
      automatic: arguments.contains('--auto'),
    ),
  ));
}

class WindowsRegression extends StatefulWidget {
  final List<String> files;
  final bool software;
  final bool automatic;
  const WindowsRegression(
      {super.key,
      required this.files,
      required this.software,
      required this.automatic});

  @override
  State<WindowsRegression> createState() => _WindowsRegressionState();
}

class _Slot {
  final Player player;
  final VideoController controller;
  final List<StreamSubscription<dynamic>> subscriptions = [];
  int positionEvents = 0;
  String details = '';
  _Slot(this.player, this.controller);
}

class _WindowsRegressionState extends State<WindowsRegression> {
  static const window = MethodChannel('media_kit_test/window');
  final slots = List<_Slot?>.filled(6, null);
  final events = <Map<String, Object?>>[];
  final errors = <String>[];
  bool busy = false;
  String status = 'Ready';
  Timer? refresh;
  void Function(FlutterErrorDetails)? previousErrorHandler;

  @override
  void initState() {
    super.initState();
    previousErrorHandler = FlutterError.onError;
    FlutterError.onError = (details) {
      errors.add(details.toString());
      previousErrorHandler?.call(details);
    };
    refresh = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (widget.automatic) {
        unawaited(runScenario());
      } else if (widget.files.isNotEmpty) {
        unawaited(perform('Open six', openSix));
      }
    });
  }

  void record(String action, [Object? detail]) {
    events.add({
      'time': DateTime.now().toIso8601String(),
      'action': action,
      'detail': detail
    });
    debugPrint('WINDOWS_TEST $action $detail');
    if (mounted) setState(() => status = action);
  }

  Future<void> perform(String name, Future<void> Function() action) async {
    if (busy) return;
    setState(() => busy = true);
    try {
      record(name);
      await action().timeout(const Duration(minutes: 3));
    } catch (error, stack) {
      errors.add('$name: $error\n$stack');
      record('FAILED', error.toString());
    } finally {
      if (mounted) setState(() => busy = false);
      await saveReport();
    }
  }

  Future<void> openSix() async {
    if (widget.files.isEmpty) {
      throw StateError('Pass local video paths as arguments');
    }
    for (var i = 0; i < slots.length; ++i) {
      if (slots[i] != null) continue;
      final player = Player();
      final controller = VideoController(player,
          configuration: VideoControllerConfiguration(
            enableHardwareAcceleration: !widget.software,
          ));
      final slot = _Slot(player, controller);
      slot.subscriptions.add(player.stream.error.listen((error) {
        errors.add('player ${i + 1}: $error');
      }));
      slot.subscriptions
          .add(player.stream.position.listen((_) => slot.positionEvents++));
      setState(() => slots[i] = slot);
      await player.setVolume(0);
      await player.setPlaylistMode(PlaylistMode.loop);
      await player.open(Media(widget.files[i % widget.files.length]));
    }
    await waitFor(
        () => slots.every((s) =>
            s != null &&
            s.controller.rect.value != null &&
            s.controller.rect.value!.width > 1),
        'six sized textures');
    for (var i = 0; i < slots.length; ++i) {
      final slot = slots[i]!;
      if (slot.details.isNotEmpty) continue;
      final native = slot.player.platform as NativePlayer;
      final codec = await native.getProperty('current-tracks/video/codec');
      final hwdec = await native.getProperty('hwdec-current');
      final source = widget.files[i % widget.files.length];
      slot.details =
          '${source.startsWith('http') ? 'HTTP' : 'local'} · $codec · $hwdec';
      record('Player ${i + 1} source', {
        'source': source,
        'codec': codec,
        'hwdec': hwdec,
        'width': slot.controller.rect.value?.width,
        'height': slot.controller.rect.value?.height,
        'mpv': await native.getProperty('mpv-version')
      });
    }
    record('Six players opened');
  }

  Future<void> waitFor(bool Function() predicate, String description) async {
    final deadline = DateTime.now().add(const Duration(seconds: 20));
    while (!predicate()) {
      if (DateTime.now().isAfter(deadline)) throw TimeoutException(description);
      await Future<void>.delayed(const Duration(milliseconds: 100));
      if (!mounted) throw StateError('Screen closed');
    }
  }

  Future<void> checkProgress(
      {Duration duration = const Duration(seconds: 2)}) async {
    final active = slots.whereType<_Slot>().toList();
    final before = {for (final slot in active) slot: slot.positionEvents};
    await Future<void>.delayed(duration);
    for (final slot in active) {
      if (slot.positionEvents <= before[slot]!) {
        throw StateError('Playback stopped');
      }
    }
    record('Playback progress', active.length);
  }

  Future<void> disposeSlot(int i) async {
    final slot = slots[i];
    if (slot == null) return;
    // Remove the Video widget before tearing down its player.
    setState(() => slots[i] = null);
    await WidgetsBinding.instance.endOfFrame;
    final stopwatch = Stopwatch()..start();
    await slot.player.dispose().timeout(const Duration(seconds: 20));
    for (final subscription in slot.subscriptions) {
      await subscription.cancel();
    }
    record('Disposed ${i + 1}', {'elapsedMs': stopwatch.elapsedMilliseconds});
  }

  Future<void> disposeSequentially() async {
    for (var i = 0; i < slots.length; ++i) {
      await disposeSlot(i);
      // The other five continue playing for five seconds before this slot is
      // recreated. Only then move to the next slot.
      await checkProgress(duration: const Duration(seconds: 5));
      await openSix();
      final restarted = slots[i]!;
      await waitFor(
          () => restarted.positionEvents > 1 && restarted.player.state.playing,
          'player ${i + 1} restarted');
      record('Restarted ${i + 1} after 5 seconds');
    }
  }

  Future<void> resizeSequence() async {
    for (var repeat = 0; repeat < 3; ++repeat) {
      for (final size in [
        [960, 640],
        [1440, 900],
        [800, 600],
        [1280, 800]
      ]) {
        await window.invokeMethod<void>('resize', size);
        await Future<void>.delayed(const Duration(milliseconds: 250));
      }
    }
    record('12 window resizes');
    for (final size in [
      [641, 359],
      [3840, 1600],
      [1080, 1920],
      [1920, 1080]
    ]) {
      for (final slot in slots.whereType<_Slot>()) {
        await slot.controller.setSize(width: size[0], height: size[1]);
      }
      final expected = widget.software
          ? switch (size[0]) {
              3840 => const Size(1920, 800),
              1080 => const Size(607, 1080),
              _ => Size(size[0].toDouble(), size[1].toDouble())
            }
          : Size(size[0].toDouble(), size[1].toDouble());
      await waitFor(
          () => slots
              .whereType<_Slot>()
              .every((s) => s.controller.rect.value?.size == expected),
          'texture size $expected');
      record('Texture resize', '${size[0]}x${size[1]} -> $expected');
    }
    for (final slot in slots.whereType<_Slot>()) {
      await slot.controller.setSize();
    }
    await checkProgress();
  }

  Future<void> runScenario() => perform('Automated scenario', () async {
        await openSix();
        await checkProgress();
        await resizeSequence();
        final first = slots.first!;
        await first.player.pause();
        await first.controller.setSize(width: 640, height: 360);
        await waitFor(
            () => first.controller.rect.value?.size == const Size(640, 360),
            'resize while paused');
        await first.player.seek(const Duration(seconds: 1));
        await first.player.setRate(1.25);
        await first.player.play();
        await first.controller.setSize();
        record('Pause, resize, seek, rate, resume');
        final bytes = await first.player.screenshot();
        if (bytes == null || bytes.isEmpty) {
          // Record this independent API failure and still exercise disposal.
          errors.add('Screenshot is empty');
          record('Screenshot FAILED');
        } else {
          record('Decoded screenshot', bytes.length);
        }
        await disposeSlot(2);
        await checkProgress();
        await openSix();
        await checkProgress();
        await disposeSequentially();
        record('Cycle complete: all six playing');
        await checkProgress();
        for (var i = 0; i < slots.length; ++i) {
          await disposeSlot(i);
        }
        record('Final cleanup complete');
        record(errors.isEmpty ? 'PASS' : 'FAIL', errors.length);
      });

  Future<void> saveReport() async {
    final report =
        File('windows-regression-${widget.software ? 'sw' : 'hw'}.json');
    await report.writeAsString(const JsonEncoder.withIndent('  ').convert({
      'requestedRenderer': widget.software ? 'software' : 'ANGLE',
      'files': widget.files,
      'status': status,
      'errors': errors,
      'events': events,
      'note':
          'Position events and decoded screenshots do not prove display presentation. Inspect the six visible videos separately.',
    }));
  }

  @override
  void dispose() {
    refresh?.cancel();
    FlutterError.onError = previousErrorHandler;
    for (final slot in slots.whereType<_Slot>()) {
      unawaited(slot.player.dispose());
      for (final subscription in slot.subscriptions) {
        unawaited(subscription.cancel());
      }
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
        appBar: AppBar(
            title: Text(
                'Windows · six videos · ${widget.software ? 'S/W' : 'ANGLE'}')),
        body: Column(children: [
          Wrap(spacing: 8, children: [
            FilledButton(
                onPressed: busy ? null : runScenario,
                child: const Text('Run scenario')),
            TextButton(
                onPressed: busy ? null : () => perform('Open six', openSix),
                child: const Text('Open six')),
            TextButton(
                onPressed:
                    busy ? null : () => perform('Resize', resizeSequence),
                child: const Text('Resize')),
            TextButton(
                onPressed: busy
                    ? null
                    : () =>
                        perform('Dispose sequentially', disposeSequentially),
                child: const Text('Dispose in order')),
          ]),
          Padding(
              padding: const EdgeInsets.all(8),
              child: Text('$status · errors: ${errors.length}')),
          if (widget.files.isEmpty)
            const Text('Launch with local video file paths as arguments.'),
          Expanded(child: LayoutBuilder(builder: (context, constraints) {
            final columns = constraints.maxWidth >= 900 ? 3 : 2;
            final rows = (6 / columns).ceil();
            return GridView.count(
              crossAxisCount: columns,
              childAspectRatio: (constraints.maxWidth / columns) /
                  (constraints.maxHeight / rows),
              children: List.generate(6, (i) {
                final slot = slots[i];
                return Card(
                    child: Column(children: [
                  Row(children: [
                    Expanded(
                        child: Text(
                            '  ${i + 1} · ${slot?.player.state.position.inSeconds ?? 0}s · ${slot?.details ?? ''}',
                            maxLines: 2,
                            overflow: TextOverflow.ellipsis)),
                    TextButton(
                        onPressed: busy || slot == null
                            ? null
                            : () => perform(
                                'Dispose ${i + 1}', () => disposeSlot(i)),
                        child: const Text('Dispose')),
                  ]),
                  Expanded(
                      child: slot == null
                          ? const Center(child: Text('Disposed'))
                          : Video(
                              key: ObjectKey(slot),
                              controller: slot.controller)),
                ]));
              }),
            );
          })),
        ]),
      );
}
