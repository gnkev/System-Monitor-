# System Monitor

A simple Windows system monitor I built to experiment with displaying hardware performance in a dashboard, similar to the Performance tab in Task Manager. It's a personal learning project written in C++ with Dear ImGui, Win32, and DirectX 11.

## Features

- **CPU:** Overall usage and individual logical processor usage, with history graphs.
- **Memory:** Physical RAM usage and committed memory.
- **GPU:** 3D engine utilization and dedicated VRAM usage, with a history graph.
- **System info:** CPU and GPU names, logical processor count, installed RAM, and uptime.

Metrics refresh approximately once per second. GPU utilization depends on Windows performance counters being available.

Temperature readings aren't implemented yet; they're something I'd like to explore in a future update.
