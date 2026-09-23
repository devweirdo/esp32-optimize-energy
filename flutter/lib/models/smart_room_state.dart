class SmartRoomState {
  final bool mqttConnected;
  final bool connecting;
  final String brokerHost;
  final int brokerPort;
  final String lastError;

  final bool systemOnline;
  final bool wifiOnline;
  final bool centralMqttOnline;
  final bool autoMode;
  final bool aiValid;
  final bool actuatorValid;

  final String aiState;
  final int aiStateId;
  final double confidence;

  final int relayMask;
  final int loadMask;
  final bool relay1;
  final bool relay2;
  final bool load1;
  final bool load2;
  final bool acPower;
  final int targetTemp;
  final double roomTemp;
  final String actuatorMode;
  final String rtc;

  const SmartRoomState({
    required this.mqttConnected,
    required this.connecting,
    required this.brokerHost,
    required this.brokerPort,
    required this.lastError,
    required this.systemOnline,
    required this.wifiOnline,
    required this.centralMqttOnline,
    required this.autoMode,
    required this.aiValid,
    required this.actuatorValid,
    required this.aiState,
    required this.aiStateId,
    required this.confidence,
    required this.relayMask,
    required this.loadMask,
    required this.relay1,
    required this.relay2,
    required this.load1,
    required this.load2,
    required this.acPower,
    required this.targetTemp,
    required this.roomTemp,
    required this.actuatorMode,
    required this.rtc,
  });

  factory SmartRoomState.initial() => const SmartRoomState(
        mqttConnected: false,
        connecting: false,
        brokerHost: '',
        brokerPort: 1883,
        lastError: '',
        systemOnline: false,
        wifiOnline: false,
        centralMqttOnline: false,
        autoMode: true,
        aiValid: false,
        actuatorValid: false,
        aiState: '--',
        aiStateId: -1,
        confidence: 0,
        relayMask: 0,
        loadMask: 0,
        relay1: false,
        relay2: false,
        load1: false,
        load2: false,
        acPower: false,
        targetTemp: 25,
        roomTemp: 0,
        actuatorMode: '--',
        rtc: '--',
      );

  SmartRoomState copyWith({
    bool? mqttConnected,
    bool? connecting,
    String? brokerHost,
    int? brokerPort,
    String? lastError,
    bool? systemOnline,
    bool? wifiOnline,
    bool? centralMqttOnline,
    bool? autoMode,
    bool? aiValid,
    bool? actuatorValid,
    String? aiState,
    int? aiStateId,
    double? confidence,
    int? relayMask,
    int? loadMask,
    bool? relay1,
    bool? relay2,
    bool? load1,
    bool? load2,
    bool? acPower,
    int? targetTemp,
    double? roomTemp,
    String? actuatorMode,
    String? rtc,
  }) {
    return SmartRoomState(
      mqttConnected: mqttConnected ?? this.mqttConnected,
      connecting: connecting ?? this.connecting,
      brokerHost: brokerHost ?? this.brokerHost,
      brokerPort: brokerPort ?? this.brokerPort,
      lastError: lastError ?? this.lastError,
      systemOnline: systemOnline ?? this.systemOnline,
      wifiOnline: wifiOnline ?? this.wifiOnline,
      centralMqttOnline: centralMqttOnline ?? this.centralMqttOnline,
      autoMode: autoMode ?? this.autoMode,
      aiValid: aiValid ?? this.aiValid,
      actuatorValid: actuatorValid ?? this.actuatorValid,
      aiState: aiState ?? this.aiState,
      aiStateId: aiStateId ?? this.aiStateId,
      confidence: confidence ?? this.confidence,
      relayMask: relayMask ?? this.relayMask,
      loadMask: loadMask ?? this.loadMask,
      relay1: relay1 ?? this.relay1,
      relay2: relay2 ?? this.relay2,
      load1: load1 ?? this.load1,
      load2: load2 ?? this.load2,
      acPower: acPower ?? this.acPower,
      targetTemp: targetTemp ?? this.targetTemp,
      roomTemp: roomTemp ?? this.roomTemp,
      actuatorMode: actuatorMode ?? this.actuatorMode,
      rtc: rtc ?? this.rtc,
    );
  }
}
