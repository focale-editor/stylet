import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:stylet/stylet.dart';

/// Starts the interactive Stylet example.
void main() => runApp(const StyletExampleApp());

/// Demonstrates every portable and native stylus value exposed by Stylet.
class StyletExampleApp extends StatelessWidget {
  /// Creates the example application.
  const StyletExampleApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
    debugShowCheckedModeBanner: false,
    title: 'Stylet example',
    theme: ThemeData(
      colorScheme: ColorScheme.fromSeed(
        seedColor: const Color(0xff6750a4),
        brightness: Brightness.dark,
      ),
      useMaterial3: true,
    ),
    home: const _StylusLaboratory(),
  );
}

/// Interactive canvas that visualizes live stylus samples.
class _StylusLaboratory extends StatefulWidget {
  /// Creates the stylus laboratory page.
  const _StylusLaboratory();

  @override
  State<_StylusLaboratory> createState() => _StylusLaboratoryState();
}

/// Holds the latest sample, action, capabilities, and short pointer trail.
class _StylusLaboratoryState extends State<_StylusLaboratory> {
  /// Maximum number of points retained by the trail painter.
  static const int _maximumTrailLength = 160;

  /// Shared plugin controller used by the demo.
  final Stylet _stylet = Stylet.instance;

  /// Recent local pointer positions rendered on the canvas.
  final List<Offset> _trail = [];

  /// Latest normalized pointer sample.
  StylusMotionEvent? _motion;

  /// Latest double-tap or squeeze interaction.
  StylusActionEvent? _action;

  /// Platform features resolved asynchronously after startup.
  StylusCapabilities? _capabilities;

  @override
  void initState() {
    super.initState();
    _loadCapabilities();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Stylet input laboratory')),
    body: SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              'Draw, hover, rotate the barrel, or use a stylus body gesture.',
              style: Theme.of(context).textTheme.bodyLarge,
            ),
            const SizedBox(height: 12),
            _CapabilityStrip(capabilities: _capabilities),
            const SizedBox(height: 12),
            Expanded(
              child: StyletListener(
                stylet: _stylet,
                behavior: HitTestBehavior.opaque,
                onEvent: _handleMotion,
                onAction: _handleAction,
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: Theme.of(context).colorScheme.surfaceContainer,
                    borderRadius: BorderRadius.circular(20),
                    border: Border.all(
                      color: Theme.of(context).colorScheme.outlineVariant,
                    ),
                  ),
                  child: ClipRRect(
                    borderRadius: BorderRadius.circular(20),
                    child: CustomPaint(
                      painter: _StylusTrailPainter(
                        points: List.unmodifiable(_trail),
                        motion: _motion,
                      ),
                      child: const SizedBox.expand(),
                    ),
                  ),
                ),
              ),
            ),
            const SizedBox(height: 12),
            _TelemetryPanel(motion: _motion, action: _action),
          ],
        ),
      ),
    ),
  );

  /// Resolves backend capabilities while tolerating a disappearing page.
  Future<void> _loadCapabilities() async {
    final StylusCapabilities capabilities = await _stylet.capabilities;
    if (mounted) {
      setState(() => _capabilities = capabilities);
    }
  }

  /// Stores one motion sample and maintains a bounded visual trail.
  void _handleMotion(StylusMotionEvent event) {
    setState(() {
      _motion = event;
      if (event.phase == StylusPhase.down) {
        _trail.clear();
      }
      if (event.phase == StylusPhase.down || event.phase == StylusPhase.move) {
        _trail.add(event.localPosition);
        if (_trail.length > _maximumTrailLength) {
          _trail.removeRange(0, _trail.length - _maximumTrailLength);
        }
      }
    });
  }

  /// Stores the latest stylus body interaction for inspection.
  void _handleAction(StylusActionEvent event) =>
      setState(() => _action = event);
}

/// Displays the advanced features offered by the current backend.
class _CapabilityStrip extends StatelessWidget {
  /// Capability description, or `null` while it is loading.
  final StylusCapabilities? capabilities;

  /// Creates a horizontally wrapping capability list.
  const _CapabilityStrip({required this.capabilities});

  @override
  Widget build(BuildContext context) {
    final StylusCapabilities? value = capabilities;
    if (value == null) {
      return const LinearProgressIndicator();
    }
    return Wrap(
      spacing: 6,
      runSpacing: 6,
      children: [
        for (final StylusFeature feature in value.features)
          Chip(label: Text(feature.name)),
      ],
    );
  }
}

/// Displays numerical data from the most recent stylus events.
class _TelemetryPanel extends StatelessWidget {
  /// Latest normalized motion sample.
  final StylusMotionEvent? motion;

  /// Latest stylus body interaction.
  final StylusActionEvent? action;

  /// Creates a telemetry panel from optional live data.
  const _TelemetryPanel({required this.motion, required this.action});

  @override
  Widget build(BuildContext context) {
    final StylusMotionEvent? sample = motion;
    final String actionLabel = action == null
        ? '—'
        : '${action!.action.name} · ${action!.phase.name}';
    return Card(
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Wrap(
          spacing: 24,
          runSpacing: 10,
          children: [
            _Metric(label: 'Phase', value: sample?.phase.name ?? '—'),
            _Metric(label: 'Tool', value: sample?.tool.name ?? '—'),
            _Metric(
              label: 'Pressure',
              value: _formatNumber(sample?.normalizedPressure),
            ),
            _Metric(label: 'Tilt', value: _formatDegrees(sample?.tilt)),
            _Metric(
              label: 'Azimuth',
              value: _formatDegrees(sample?.orientation),
            ),
            _Metric(
              label: 'Barrel',
              value: _formatDegrees(sample?.barrelRotation),
            ),
            _Metric(
              label: 'Button 1',
              value: sample?.isSideButtonPressed(number: 1) ?? false
                  ? 'down'
                  : 'up',
            ),
            _Metric(label: 'Action', value: actionLabel),
          ],
        ),
      ),
    );
  }
}

/// One compact label-value pair in the telemetry panel.
class _Metric extends StatelessWidget {
  /// Short metric label.
  final String label;

  /// Human-readable current value.
  final String value;

  /// Creates a telemetry metric.
  const _Metric({required this.label, required this.value});

  @override
  Widget build(BuildContext context) => SizedBox(
    width: 132,
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          label,
          style: Theme.of(context).textTheme.labelMedium
              ?.copyWith(color: Theme.of(context).colorScheme.onSurfaceVariant),
        ),
        Text(
          value,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: Theme.of(context).textTheme.bodyLarge,
        ),
      ],
    ),
  );
}

/// Paints a pressure-sensitive trail and a rotated stylus nib preview.
class _StylusTrailPainter extends CustomPainter {
  /// Recent local pointer positions.
  final List<Offset> points;

  /// Latest sample controlling width and nib angle.
  final StylusMotionEvent? motion;

  /// Creates a painter from immutable input snapshots.
  const _StylusTrailPainter({required this.points, required this.motion});

  @override
  void paint(Canvas canvas, Size size) {
    final StylusMotionEvent? sample = motion;
    final double pressure = sample?.normalizedPressure ?? 0.5;
    final Paint trailPaint = Paint()
      ..color = const Color(0xffd0bcff)
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round
      ..strokeWidth = 2 + pressure * 14
      ..style = PaintingStyle.stroke;
    if (points.length > 1) {
      final Path path = Path()..moveTo(points.first.dx, points.first.dy);
      for (final Offset point in points.skip(1)) {
        path.lineTo(point.dx, point.dy);
      }
      canvas.drawPath(path, trailPaint);
    }
    if (sample == null) {
      return;
    }
    final double rotation = sample.barrelRotation ?? sample.orientation ?? 0;
    canvas
      ..save()
      ..translate(sample.localPosition.dx, sample.localPosition.dy)
      ..rotate(rotation)
      ..drawOval(
        Rect.fromCenter(center: Offset.zero, width: 28, height: 8),
        Paint()..color = const Color(0xffffd8e4),
      )
      ..restore();
  }

  @override
  bool shouldRepaint(covariant _StylusTrailPainter oldDelegate) =>
      oldDelegate.motion != motion || oldDelegate.points != points;
}

/// Formats an optional normalized value for telemetry.
String _formatNumber(double? value) =>
    value == null ? '—' : value.toStringAsFixed(3);

/// Formats an optional radian angle as degrees for telemetry.
String _formatDegrees(double? radians) =>
    radians == null ? '—' : '${(radians * 180 / math.pi).toStringAsFixed(1)}°';
