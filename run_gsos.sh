#!/usr/bin/env bash
# Boot GS/OS System 6.0.1 to the Finder desktop (slot-5 3.5" drive), instead of
# the default slot-7 Total Replay HDD. Pass a different 800K .2mg/.po to override
# the disk:  ./run_gsos.sh "disks35/Some Other GS-OS Disk.2mg"
# Run from repo root so roms/ + disks35/ probes resolve (POM2 convention).
cd "$(dirname "${BASH_SOURCE[0]}")"
exec ./build/POMIIGS --gsos "$@"
