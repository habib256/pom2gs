#!/usr/bin/env bash
# Boot GS/OS System 6.0.1 to the Finder desktop from the slot-5 3.5" drive,
# instead of the slot-7 ProDOS hard disk pomiigs.cfg boots by default
# (`boot = hdd`, `hdd = hdv/GSOS.hdv`). The disk comes from `disk35 =` in
# pomiigs.cfg; pass a different 800K .2mg/.po to override it:
#   ./run_gsos.sh "disks35/Some Other GS-OS Disk.2mg"
# Run from repo root so roms/ + disks35/ probes resolve (POM2 convention).
cd "$(dirname "${BASH_SOURCE[0]}")"
exec ./build/POMIIGS --gsos "$@"
