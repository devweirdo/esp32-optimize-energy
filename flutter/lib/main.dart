import 'package:flutter/material.dart';
import 'screens/home_screen.dart';

void main() => runApp(const SmartRoomApp());

class SmartRoomApp extends StatefulWidget {
  const SmartRoomApp({super.key});

  @override
  State<SmartRoomApp> createState() => _SmartRoomAppState();
}

class _SmartRoomAppState extends State<SmartRoomApp> {
  ThemeMode _themeMode = ThemeMode.system;

  void _toggleTheme() {
    setState(() {
      _themeMode =
          _themeMode == ThemeMode.dark ? ThemeMode.light : ThemeMode.dark;
    });
  }

  @override
  Widget build(BuildContext context) {
    const seed = Color(0xFF3F7A6B);

    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Smart Room',
      themeMode: _themeMode,
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(seedColor: seed),
        scaffoldBackgroundColor: const Color(0xFFF5F7F6),
      ),
      darkTheme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(
          seedColor: seed,
          brightness: Brightness.dark,
        ),
      ),
      home: HomeScreen(
        themeMode: _themeMode,
        onToggleTheme: _toggleTheme,
      ),
    );
  }
}
