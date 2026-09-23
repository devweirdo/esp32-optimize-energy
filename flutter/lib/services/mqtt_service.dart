import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

import '../models/smart_room_state.dart';

class MqttService extends ChangeNotifier {
  static const topicAi = 'smartroom/status/ai';
  static const topicActuator = 'smartroom/status/actuator';
  static const topicSystem = 'smartroom/status/system';
  static const topicControl = 'smartroom/cmd/control';
  static const topicTime = 'smartroom/cmd/time';

  MqttServerClient? _client;
  StreamSubscription<List<MqttReceivedMessage<MqttMessage>>>? _updatesSub;

  SmartRoomState _state = SmartRoomState.initial();
  SmartRoomState get state => _state;

  bool get isConnected =>
      _client?.connectionStatus?.state == MqttConnectionState.connected;

  Future<void> connect({
    required String host,
    int port = 1883,
  }) async {
    final cleanHost = host.trim();

    if (cleanHost.isEmpty) {
      _setState(_state.copyWith(lastError: 'Vui lòng nhập IP broker.'));
      return;
    }

    await disconnect(silent: true);

    _setState(_state.copyWith(
      connecting: true,
      mqttConnected: false,
      brokerHost: cleanHost,
      brokerPort: port,
      lastError: '',
    ));

    final clientId =
        'FlutterSmartRoom_${DateTime.now().millisecondsSinceEpoch}';

    final client = MqttServerClient.withPort(
      cleanHost,
      clientId,
      port,
      maxConnectionAttempts: 3,
    );

    client.keepAlivePeriod = 20;
    client.autoReconnect = true;
    client.resubscribeOnAutoReconnect = true;
    client.logging(on: false);

    client.connectionMessage = MqttConnectMessage()
        .withClientIdentifier(clientId)
        .startClean();

    client.onConnected = () {
      _subscribeAll();
      _setState(_state.copyWith(
        mqttConnected: true,
        connecting: false,
        lastError: '',
      ));
    };

    client.onAutoReconnect = () {
      _setState(_state.copyWith(
        mqttConnected: false,
        connecting: true,
        lastError: 'Đang kết nối lại MQTT...',
      ));
    };

    client.onAutoReconnected = () {
      _setState(_state.copyWith(
        mqttConnected: true,
        connecting: false,
        lastError: '',
      ));
    };

    _client = client;

    try {
      final status = await client.connect();

      if (status?.state != MqttConnectionState.connected) {
        throw Exception(
          'MQTT connect failed: ${status?.returnCode ?? 'unknown'}',
        );
      }

      await _updatesSub?.cancel();
      _updatesSub = client.updates?.listen(_handleMessages);
    } catch (e) {
      client.autoReconnect = false;
      client.disconnect();
      _setState(_state.copyWith(
        mqttConnected: false,
        connecting: false,
        lastError: 'Không kết nối được broker: $e',
      ));
    }
  }

  void _subscribeAll() {
    final client = _client;
    if (client == null) return;

    client.subscribe(topicAi, MqttQos.atLeastOnce);
    client.subscribe(topicActuator, MqttQos.atLeastOnce);
    client.subscribe(topicSystem, MqttQos.atLeastOnce);
  }

  void _handleMessages(List<MqttReceivedMessage<MqttMessage>> messages) {
    for (final item in messages) {
      final message = item.payload;
      if (message is! MqttPublishMessage) continue;

      final payload =
          MqttPublishPayload.bytesToStringAsString(message.payload.message);

      try {
        final decoded = jsonDecode(payload);
        if (decoded is! Map<String, dynamic>) continue;

        switch (item.topic) {
          case topicAi:
            _handleAi(decoded);
            break;
          case topicActuator:
            _handleActuator(decoded);
            break;
          case topicSystem:
            _handleSystem(decoded);
            break;
        }
      } catch (_) {
        // Ignore malformed MQTT data instead of stopping the dashboard.
      }
    }
  }

  void _handleAi(Map<String, dynamic> json) {
    _setState(_state.copyWith(
      aiState: json['state']?.toString() ?? _state.aiState,
      aiStateId: _toInt(json['stateId'], _state.aiStateId),
      confidence: _toDouble(json['confidence'], _state.confidence),
      aiValid: true,
    ));
  }

  void _handleActuator(Map<String, dynamic> json) {
    final mode = json['mode']?.toString() ?? _state.actuatorMode;
    final upperMode = mode.toUpperCase();

    _setState(_state.copyWith(
      relayMask: _toInt(json['relayMask'], _state.relayMask),
      loadMask: _toInt(json['loadMask'], _state.loadMask),
      relay1: _toBool(json['relay1'], _state.relay1),
      relay2: _toBool(json['relay2'], _state.relay2),
      load1: _toBool(json['load1'], _state.load1),
      load2: _toBool(json['load2'], _state.load2),
      acPower: _toBool(json['acPower'], _state.acPower),
      targetTemp: _toInt(json['targetTemp'], _state.targetTemp),
      roomTemp: _toDouble(json['roomTemp'], _state.roomTemp),
      actuatorMode: mode,
      autoMode: upperMode == 'AUTO'
          ? true
          : upperMode == 'MANUAL'
              ? false
              : _state.autoMode,
      rtc: json['rtc']?.toString() ?? _state.rtc,
      actuatorValid: true,
    ));
  }

  void _handleSystem(Map<String, dynamic> json) {
    _setState(_state.copyWith(
      systemOnline: _toBool(json['online'], _state.systemOnline),
      wifiOnline: _toBool(json['wifi'], _state.wifiOnline),
      centralMqttOnline:
          _toBool(json['mqtt'], _state.centralMqttOnline),
      autoMode: _toBool(json['auto'], _state.autoMode),
      aiValid: _toBool(json['aiValid'], _state.aiValid),
      actuatorValid: _toBool(json['actuatorValid'], _state.actuatorValid),
    ));
  }

  void setAutoMode(bool enabled) {
    if (!_requireConnection()) return;

    if (enabled) {
      _publishJson(topicControl, {'mode': 'AUTO'});
      _setState(_state.copyWith(
        autoMode: true,
        actuatorMode: 'AUTO',
      ));
      return;
    }

    _publishManual(
      relayMask: _state.relayMask,
      acPower: _state.acPower,
      targetTemp: _state.targetTemp,
    );
  }

  void setRelay1(bool on) {
    if (_state.autoMode || !_requireConnection()) return;

    var mask = _state.relayMask;
    mask = on ? (mask | 0x01) : (mask & ~0x01);

    _publishManual(
      relayMask: mask,
      acPower: _state.acPower,
      targetTemp: _state.targetTemp,
    );
  }

  void setRelay2(bool on) {
    if (_state.autoMode || !_requireConnection()) return;

    var mask = _state.relayMask;
    mask = on ? (mask | 0x02) : (mask & ~0x02);

    _publishManual(
      relayMask: mask,
      acPower: _state.acPower,
      targetTemp: _state.targetTemp,
    );
  }

  void setAcPower(bool on) {
    if (_state.autoMode || !_requireConnection()) return;

    _publishManual(
      relayMask: _state.relayMask,
      acPower: on,
      targetTemp: _state.targetTemp,
    );
  }

  void changeTargetTemp(int delta) {
    if (_state.autoMode || !_requireConnection()) return;

    final next = (_state.targetTemp + delta).clamp(16, 30);

    _publishManual(
      relayMask: _state.relayMask,
      acPower: _state.acPower,
      targetTemp: next,
    );
  }

  void _publishManual({
    required int relayMask,
    required bool acPower,
    required int targetTemp,
  }) {
    _publishJson(topicControl, {
      'mode': 'MANUAL',
      'relayMask': relayMask,
      'acPower': acPower,
      'targetTemp': targetTemp,
    });

    _setState(_state.copyWith(
      autoMode: false,
      actuatorMode: 'MANUAL',
      relayMask: relayMask,
      relay1: (relayMask & 0x01) != 0,
      relay2: (relayMask & 0x02) != 0,
      acPower: acPower,
      targetTemp: targetTemp,
    ));
  }

  void syncPhoneTime() {
    if (!_requireConnection()) return;

    final now = DateTime.now();

    _publishJson(topicTime, {
      'year': now.year,
      'month': now.month,
      'day': now.day,
      'hour': now.hour,
      'minute': now.minute,
      'second': now.second,
    });
  }

  void _publishJson(String topic, Map<String, dynamic> data) {
    final client = _client;
    if (client == null || !isConnected) return;

    final builder = MqttClientPayloadBuilder();
    builder.addString(jsonEncode(data));

    client.publishMessage(
      topic,
      MqttQos.atLeastOnce,
      builder.payload!,
    );
  }

  bool _requireConnection() {
    if (isConnected) return true;

    _setState(_state.copyWith(lastError: 'MQTT chưa kết nối.'));
    return false;
  }

  Future<void> disconnect({bool silent = false}) async {
    await _updatesSub?.cancel();
    _updatesSub = null;

    final client = _client;
    _client = null;

    if (client != null) {
      client.autoReconnect = false;
      client.disconnect();
    }

    if (!silent) {
      _setState(_state.copyWith(
        mqttConnected: false,
        connecting: false,
      ));
    }
  }

  int _toInt(dynamic value, int fallback) {
    if (value is int) return value;
    if (value is num) return value.toInt();
    return int.tryParse(value?.toString() ?? '') ?? fallback;
  }

  double _toDouble(dynamic value, double fallback) {
    if (value is num) return value.toDouble();
    return double.tryParse(value?.toString() ?? '') ?? fallback;
  }

  bool _toBool(dynamic value, bool fallback) {
    if (value is bool) return value;
    if (value is num) return value != 0;

    final text = value?.toString().toLowerCase();
    if (text == 'true' || text == '1') return true;
    if (text == 'false' || text == '0') return false;

    return fallback;
  }

  void _setState(SmartRoomState value) {
    _state = value;
    notifyListeners();
  }

  @override
  void dispose() {
    _updatesSub?.cancel();
    _client?.autoReconnect = false;
    _client?.disconnect();
    super.dispose();
  }
}
