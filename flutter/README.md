# Smart Room Flutter App

Ứng dụng Flutter cho hệ thống:

Camera ESP32-S3 -> ESP-NOW -> Central ESP32-C6 <-> ESP-NOW <-> Node 3
                                      |
                                     MQTT
                                      |
                                  Mosquitto
                                      |
                                   Flutter

## Topic

Subscribe:
- `smartroom/status/ai`
- `smartroom/status/actuator`
- `smartroom/status/system`

Publish:
- `smartroom/cmd/control`
- `smartroom/cmd/time`

## Cài nhanh

```bash
flutter create smart_room_app
```

Sau đó thay `pubspec.yaml` và thư mục `lib/` bằng các file trong gói này.

Android: thêm vào `android/app/src/main/AndroidManifest.xml`, ngay dưới thẻ `<manifest ...>`:

```xml
<uses-permission android:name="android.permission.INTERNET" />
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
```

Chạy:

```bash
flutter pub get
flutter run
```

Trong app:
- Broker IP = IPv4 của laptop chạy Mosquitto (`ipconfig`).
- Không dùng `127.0.0.1` trên điện thoại.
- Port = `1883`.
- Laptop và điện thoại phải cùng LAN/hotspot.

Broker demo LAN:

```conf
listener 1883 0.0.0.0
allow_anonymous true
```

Không dùng cấu hình anonymous trên mạng công cộng.

## AUTO / MANUAL

AUTO:

```json
{"mode":"AUTO"}
```

MANUAL:

```json
{"mode":"MANUAL","relayMask":1,"acPower":true,"targetTemp":26}
```

Ở AUTO, app khóa các nút Relay/AC/temperature. Central vẫn là nơi quyết định policy AUTO.

## RTC

Nút `Sync phone time` gửi giờ local của điện thoại tới:

`smartroom/cmd/time`

Central tính `day_of_week` rồi gửi `time_sync_packet_t` tới Node 3.

## Nếu Android không kết nối broker

Kiểm tra:

```cmd
netstat -ano | findstr :1883
```

Cần thấy Mosquitto listen trên `0.0.0.0:1883`, firewall cho TCP 1883, và điện thoại dùng đúng IP LAN của laptop.
