#!/usr/bin/env bash
# Run from repo root so roms/ probes resolve (POM2 convention).
cd "$(dirname "${BASH_SOURCE[0]}")"
exec ./build/POMIIGS "$@"
