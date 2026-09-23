import 'package:flutter/material.dart';

import '../services/mqtt_service.dart';
import '../widgets/dashboard_widgets.dart';

class HomeScreen extends StatefulWidget {
  final VoidCallback onToggleTheme;
  final ThemeMode themeMode;

  const HomeScreen({
    super.key,
    required this.onToggleTheme,
    required this.themeMode,
  });

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final MqttService _mqtt = MqttService();
  final TextEditingController _hostController = TextEditingController();
  final TextEditingController _portController =
      TextEditingController(text: '1883');

  @override
  void initState() {
    super.initState();
    _mqtt.addListener(_refresh);
  }

  void _refresh() {
    if (mounted) setState(() {});
  }

  @override
  void dispose() {
    _mqtt.removeListener(_refresh);
    _mqtt.dispose();
    _hostController.dispose();
    _portController.dispose();
    super.dispose();
  }

  Future<void> _connect() async {
    await _mqtt.connect(
      host: _hostController.text,
      port: int.tryParse(_portController.text.trim()) ?? 1883,
    );
  }

  @override
  Widget build(BuildContext context) {
    final s = _mqtt.state;
    final theme = Theme.of(context);
    final manualEnabled = s.mqttConnected && !s.autoMode;

    return Scaffold(
      appBar: AppBar(
        titleSpacing: 20,
        title: const Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Smart Room',
              style: TextStyle(fontWeight: FontWeight.w800),
            ),
            Text(
              'Energy Optimization Dashboard',
              style: TextStyle(fontSize: 12, fontWeight: FontWeight.w400),
            ),
          ],
        ),
        actions: [
          Tooltip(
            message:
                s.mqttConnected ? 'MQTT connected' : 'MQTT disconnected',
            child: Icon(
              Icons.circle,
              size: 12,
              color: s.mqttConnected
                  ? theme.colorScheme.primary
                  : theme.colorScheme.error,
            ),
          ),
          IconButton(
            onPressed: widget.onToggleTheme,
            tooltip: 'Theme',
            icon: Icon(
              widget.themeMode == ThemeMode.dark
                  ? Icons.light_mode_outlined
                  : Icons.dark_mode_outlined,
            ),
          ),
          const SizedBox(width: 8),
        ],
      ),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.fromLTRB(16, 12, 16, 32),
          children: [
            _ConnectionCard(
              hostController: _hostController,
              portController: _portController,
              connected: s.mqttConnected,
              connecting: s.connecting,
              brokerHost: s.brokerHost,
              brokerPort: s.brokerPort,
              onConnect: _connect,
              onDisconnect: () => _mqtt.disconnect(),
            ),
            if (s.lastError.isNotEmpty) ...[
              const SizedBox(height: 12),
              _ErrorBanner(message: s.lastError),
            ],
            const SizedBox(height: 16),
            _ModeCard(
              autoMode: s.autoMode,
              connected: s.mqttConnected,
              onChanged: _mqtt.setAutoMode,
            ),
            const SizedBox(height: 16),
            LayoutBuilder(
              builder: (context, constraints) {
                final twoColumns = constraints.maxWidth >= 760;
                final width = twoColumns
                    ? (constraints.maxWidth - 16) / 2
                    : constraints.maxWidth;

                return Wrap(
                  spacing: 16,
                  runSpacing: 16,
                  children: [
                    SizedBox(
                      width: width,
                      child: AiStateCard(
                        state: s.aiState,
                        confidence: s.confidence,
                        valid: s.aiValid,
                      ),
                    ),
                    SizedBox(
                      width: width,
                      child: TemperatureCard(
                        roomTemp: s.roomTemp,
                        targetTemp: s.targetTemp,
                        manualEnabled: manualEnabled,
                        onDecrease: () => _mqtt.changeTargetTemp(-1),
                        onIncrease: () => _mqtt.changeTargetTemp(1),
                      ),
                    ),
                    SizedBox(
                      width: width,
                      child: DeviceControlCard(
                        enabled: manualEnabled,
                        relay1: s.relay1,
                        relay2: s.relay2,
                        acPower: s.acPower,
                        onRelay1: _mqtt.setRelay1,
                        onRelay2: _mqtt.setRelay2,
                        onAcPower: _mqtt.setAcPower,
                      ),
                    ),
                    SizedBox(
                      width: width,
                      child: SystemStatusCard(
                        central: s.systemOnline,
                        wifi: s.wifiOnline,
                        mqtt: s.centralMqttOnline,
                        ai: s.aiValid,
                        actuator: s.actuatorValid,
                      ),
                    ),
                    SizedBox(
                      width: width,
                      child: LoadFeedbackCard(
                        load1: s.load1,
                        load2: s.load2,
                        relayMask: s.relayMask,
                        loadMask: s.loadMask,
                      ),
                    ),
                    SizedBox(
                      width: width,
                      child: RtcCard(
                        rtc: s.rtc,
                        enabled: s.mqttConnected,
                        onSync: _mqtt.syncPhoneTime,
                      ),
                    ),
                  ],
                );
              },
            ),
          ],
        ),
      ),
    );
  }
}

class _ConnectionCard extends StatelessWidget {
  final TextEditingController hostController;
  final TextEditingController portController;
  final bool connected;
  final bool connecting;
  final String brokerHost;
  final int brokerPort;
  final VoidCallback onConnect;
  final VoidCallback onDisconnect;

  const _ConnectionCard({
    required this.hostController,
    required this.portController,
    required this.connected,
    required this.connecting,
    required this.brokerHost,
    required this.brokerPort,
    required this.onConnect,
    required this.onDisconnect,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: connected
            ? Row(
                children: [
                  Icon(
                    Icons.cloud_done_outlined,
                    color: theme.colorScheme.primary,
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Text(
                          'MQTT Broker Connected',
                          style: TextStyle(fontWeight: FontWeight.w700),
                        ),
                        Text(
                          '$brokerHost:$brokerPort',
                          style: theme.textTheme.bodySmall,
                        ),
                      ],
                    ),
                  ),
                  TextButton(
                    onPressed: onDisconnect,
                    child: const Text('Disconnect'),
                  ),
                ],
              )
            : Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    'MQTT Broker',
                    style: theme.textTheme.titleMedium?.copyWith(
                      fontWeight: FontWeight.w700,
                    ),
                  ),
                  const SizedBox(height: 6),
                  Text(
                    'Nhập IPv4 của laptop chạy Mosquitto. '
                    'Không dùng 127.0.0.1 trên điện thoại.',
                    style: theme.textTheme.bodySmall,
                  ),
                  const SizedBox(height: 14),
                  Row(
                    children: [
                      Expanded(
                        flex: 3,
                        child: TextField(
                          controller: hostController,
                          keyboardType: TextInputType.url,
                          decoration: const InputDecoration(
                            labelText: 'Broker IP',
                            hintText: '192.168.107.x',
                            border: OutlineInputBorder(),
                          ),
                        ),
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: TextField(
                          controller: portController,
                          keyboardType: TextInputType.number,
                          decoration: const InputDecoration(
                            labelText: 'Port',
                            border: OutlineInputBorder(),
                          ),
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 12),
                  SizedBox(
                    width: double.infinity,
                    child: FilledButton.icon(
                      onPressed: connecting ? null : onConnect,
                      icon: connecting
                          ? const SizedBox(
                              width: 18,
                              height: 18,
                              child: CircularProgressIndicator(strokeWidth: 2),
                            )
                          : const Icon(Icons.link),
                      label: Text(
                        connecting ? 'Connecting...' : 'Connect',
                      ),
                    ),
                  ),
                ],
              ),
      ),
    );
  }
}

class _ModeCard extends StatelessWidget {
  final bool autoMode;
  final bool connected;
  final ValueChanged<bool> onChanged;

  const _ModeCard({
    required this.autoMode,
    required this.connected,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Card(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 14),
        child: Row(
          children: [
            Container(
              width: 46,
              height: 46,
              decoration: BoxDecoration(
                color: theme.colorScheme.primaryContainer,
                borderRadius: BorderRadius.circular(15),
              ),
              child: Icon(
                autoMode ? Icons.auto_mode : Icons.touch_app_outlined,
                color: theme.colorScheme.onPrimaryContainer,
              ),
            ),
            const SizedBox(width: 14),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    autoMode ? 'AUTO Mode' : 'MANUAL Mode',
                    style: theme.textTheme.titleMedium?.copyWith(
                      fontWeight: FontWeight.w800,
                    ),
                  ),
                  Text(
                    autoMode
                        ? 'Central điều khiển theo trạng thái AI.'
                        : 'Cho phép điều khiển Relay và AC.',
                    style: theme.textTheme.bodySmall,
                  ),
                ],
              ),
            ),
            Switch(
              value: autoMode,
              onChanged: connected ? onChanged : null,
            ),
          ],
        ),
      ),
    );
  }
}

class _ErrorBanner extends StatelessWidget {
  final String message;

  const _ErrorBanner({required this.message});

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Container(
      padding: const EdgeInsets.all(13),
      decoration: BoxDecoration(
        color: theme.colorScheme.errorContainer,
        borderRadius: BorderRadius.circular(14),
      ),
      child: Row(
        children: [
          Icon(
            Icons.error_outline,
            color: theme.colorScheme.onErrorContainer,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              message,
              style: TextStyle(color: theme.colorScheme.onErrorContainer),
            ),
          ),
        ],
      ),
    );
  }
}
