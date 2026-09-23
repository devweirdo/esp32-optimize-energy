import 'package:flutter/material.dart';

class AiStateCard extends StatelessWidget {
  final String state;
  final double confidence;
  final bool valid;

  const AiStateCard({
    super.key,
    required this.state,
    required this.confidence,
    required this.valid,
  });

  IconData _iconForState(String value) {
    switch (value.toLowerCase()) {
      case 'empty':
        return Icons.meeting_room_outlined;
      case 'reading':
        return Icons.menu_book_rounded;
      case 'sleep':
        return Icons.bedtime_rounded;
      case 'active':
        return Icons.directions_run_rounded;
      default:
        return Icons.psychology_alt_outlined;
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final percentage = (confidence * 100).clamp(0, 100).toDouble();

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'AI Room State',
              style: theme.textTheme.titleMedium?.copyWith(
                fontWeight: FontWeight.w700,
              ),
            ),
            const SizedBox(height: 22),
            Row(
              children: [
                Container(
                  width: 62,
                  height: 62,
                  decoration: BoxDecoration(
                    color: theme.colorScheme.primaryContainer,
                    borderRadius: BorderRadius.circular(20),
                  ),
                  child: Icon(
                    _iconForState(state),
                    size: 32,
                    color: theme.colorScheme.onPrimaryContainer,
                  ),
                ),
                const SizedBox(width: 16),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        valid ? state : 'No data',
                        style: theme.textTheme.headlineSmall?.copyWith(
                          fontWeight: FontWeight.w800,
                        ),
                      ),
                      const SizedBox(height: 5),
                      Text(
                        valid
                            ? 'Confidence ${percentage.toStringAsFixed(1)}%'
                            : 'Waiting for Camera Node',
                      ),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 20),
            LinearProgressIndicator(
              value: valid ? percentage / 100 : 0,
              minHeight: 8,
              borderRadius: BorderRadius.circular(99),
            ),
          ],
        ),
      ),
    );
  }
}

class TemperatureCard extends StatelessWidget {
  final double roomTemp;
  final int targetTemp;
  final bool manualEnabled;
  final VoidCallback onDecrease;
  final VoidCallback onIncrease;

  const TemperatureCard({
    super.key,
    required this.roomTemp,
    required this.targetTemp,
    required this.manualEnabled,
    required this.onDecrease,
    required this.onIncrease,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(18),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Temperature',
              style: theme.textTheme.titleMedium?.copyWith(
                fontWeight: FontWeight.w700,
              ),
            ),
            const SizedBox(height: 18),
            Row(
              children: [
                Expanded(
                  child: _ValueTile(
                    icon: Icons.thermostat_rounded,
                    label: 'Room',
                    value: '${roomTemp.toStringAsFixed(1)} °C',
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: _ValueTile(
                    icon: Icons.tune_rounded,
                    label: 'Target',
                    value: '$targetTemp °C',
                  ),
                ),
              ],
            ),
            const SizedBox(height: 18),
            Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                IconButton.filledTonal(
                  onPressed: manualEnabled ? onDecrease : null,
                  icon: const Icon(Icons.remove),
                ),
                Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 22),
                  child: Text(
                    '$targetTemp °C',
                    style: theme.textTheme.headlineSmall?.copyWith(
                      fontWeight: FontWeight.w800,
                    ),
                  ),
                ),
                IconButton.filledTonal(
                  onPressed: manualEnabled ? onIncrease : null,
                  icon: const Icon(Icons.add),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class DeviceControlCard extends StatelessWidget {
  final bool enabled;
  final bool relay1;
  final bool relay2;
  final bool acPower;
  final ValueChanged<bool> onRelay1;
  final ValueChanged<bool> onRelay2;
  final ValueChanged<bool> onAcPower;

  const DeviceControlCard({
    super.key,
    required this.enabled,
    required this.relay1,
    required this.relay2,
    required this.acPower,
    required this.onRelay1,
    required this.onRelay2,
    required this.onAcPower,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    Widget row(
      String title,
      String subtitle,
      IconData icon,
      bool value,
      ValueChanged<bool> onChanged,
    ) {
      return Container(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
        decoration: BoxDecoration(
          color: theme.colorScheme.surfaceContainerHighest.withValues(alpha: .45),
          borderRadius: BorderRadius.circular(16),
        ),
        child: Row(
          children: [
            Icon(icon),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(title,
                      style: const TextStyle(fontWeight: FontWeight.w700)),
                  Text(subtitle, style: theme.textTheme.bodySmall),
                ],
              ),
            ),
            Switch(
              value: value,
              onChanged: enabled ? onChanged : null,
            ),
          ],
        ),
      );
    }

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(18),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Device Control',
              style: theme.textTheme.titleMedium?.copyWith(
                fontWeight: FontWeight.w700,
              ),
            ),
            const SizedBox(height: 5),
            Text(
              enabled
                  ? 'Manual control is enabled.'
                  : 'Switch to MANUAL to control devices.',
              style: theme.textTheme.bodySmall,
            ),
            const SizedBox(height: 16),
            row(
              'Relay 1',
              relay1 ? 'ON' : 'OFF',
              Icons.lightbulb_outline_rounded,
              relay1,
              onRelay1,
            ),
            const SizedBox(height: 10),
            row(
              'Relay 2',
              relay2 ? 'ON' : 'OFF',
              Icons.power_outlined,
              relay2,
              onRelay2,
            ),
            const SizedBox(height: 10),
            row(
              'Air Conditioner',
              acPower ? 'ON' : 'OFF',
              Icons.ac_unit_rounded,
              acPower,
              onAcPower,
            ),
          ],
        ),
      ),
    );
  }
}

class SystemStatusCard extends StatelessWidget {
  final bool central;
  final bool wifi;
  final bool mqtt;
  final bool ai;
  final bool actuator;

  const SystemStatusCard({
    super.key,
    required this.central,
    required this.wifi,
    required this.mqtt,
    required this.ai,
    required this.actuator,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    Widget line(String label, bool ok) {
      return Padding(
        padding: const EdgeInsets.only(bottom: 12),
        child: Row(
          children: [
            Icon(
              ok ? Icons.check_circle : Icons.cancel,
              size: 19,
              color: ok ? theme.colorScheme.primary : theme.colorScheme.error,
            ),
            const SizedBox(width: 10),
            Expanded(child: Text(label)),
            Text(
              ok ? 'OK' : 'Offline',
              style: const TextStyle(fontWeight: FontWeight.w600),
            ),
          ],
        ),
      );
    }

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(18),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'System Status',
              style: theme.textTheme.titleMedium?.copyWith(
                fontWeight: FontWeight.w700,
              ),
            ),
            const SizedBox(height: 18),
            line('Central', central),
            line('Wi-Fi', wifi),
            line('MQTT', mqtt),
            line('AI Camera', ai),
            line('Actuator Node', actuator),
          ],
        ),
      ),
    );
  }
}

class LoadFeedbackCard extends StatelessWidget {
  final bool load1;
  final bool load2;
  final int relayMask;
  final int loadMask;

  const LoadFeedbackCard({
    super.key,
    required this.load1,
    required this.load2,
    required this.relayMask,
    required this.loadMask,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    Widget load(String label, bool active) {
      return Expanded(
        child: Container(
          padding: const EdgeInsets.all(15),
          decoration: BoxDecoration(
            color:
                theme.colorScheme.surfaceContainerHighest.withValues(alpha: .45),
            borderRadius: BorderRadius.circular(16),
          ),
          child: Column(
            children: [
              Icon(
                active ? Icons.electric_bolt_rounded : Icons.power_off_outlined,
                size: 28,
              ),
              const SizedBox(height: 7),
              Text(label),
              const SizedBox(height: 3),
              Text(
                active ? 'LOAD ON' : 'LOAD OFF',
                style: const TextStyle(fontWeight: FontWeight.w800),
              ),
            ],
          ),
        ),
      );
    }

    final r = relayMask.toRadixString(16).padLeft(2, '0').toUpperCase();
    final l = loadMask.toRadixString(16).padLeft(2, '0').toUpperCase();

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(18),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Load Feedback',
              style: theme.textTheme.titleMedium?.copyWith(
                fontWeight: FontWeight.w700,
              ),
            ),
            const SizedBox(height: 16),
            Row(
              children: [
                load('Channel 1', load1),
                const SizedBox(width: 12),
                load('Channel 2', load2),
              ],
            ),
            const SizedBox(height: 12),
            Text(
              'relayMask: 0x$r   loadMask: 0x$l',
              style: theme.textTheme.bodySmall,
            ),
          ],
        ),
      ),
    );
  }
}

class RtcCard extends StatelessWidget {
  final String rtc;
  final bool enabled;
  final VoidCallback onSync;

  const RtcCard({
    super.key,
    required this.rtc,
    required this.enabled,
    required this.onSync,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final value = rtc == '--' ? '--' : rtc.replaceFirst('T', ' ');

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(18),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Node 3 RTC',
              style: theme.textTheme.titleMedium?.copyWith(
                fontWeight: FontWeight.w700,
              ),
            ),
            const SizedBox(height: 18),
            Row(
              children: [
                const Icon(Icons.schedule_rounded, size: 30),
                const SizedBox(width: 12),
                Expanded(
                  child: Text(
                    value,
                    style: theme.textTheme.titleMedium?.copyWith(
                      fontWeight: FontWeight.w800,
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 16),
            SizedBox(
              width: double.infinity,
              child: OutlinedButton.icon(
                onPressed: enabled ? onSync : null,
                icon: const Icon(Icons.sync_rounded),
                label: const Text('Sync phone time'),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _ValueTile extends StatelessWidget {
  final IconData icon;
  final String label;
  final String value;

  const _ValueTile({
    required this.icon,
    required this.label,
    required this.value,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: theme.colorScheme.surfaceContainerHighest.withValues(alpha: .45),
        borderRadius: BorderRadius.circular(16),
      ),
      child: Column(
        children: [
          Icon(icon),
          const SizedBox(height: 8),
          Text(label, style: theme.textTheme.bodySmall),
          const SizedBox(height: 4),
          Text(
            value,
            style: theme.textTheme.titleLarge?.copyWith(
              fontWeight: FontWeight.w800,
            ),
          ),
        ],
      ),
    );
  }
}
