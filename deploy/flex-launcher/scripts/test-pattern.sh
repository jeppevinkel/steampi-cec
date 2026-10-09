#!/bin/sh
# Flex Launcher test entry: shows a test pattern for 5 seconds, then returns to the launcher.
exec mpv --fullscreen --no-osc --no-input-default-bindings \
    "av://lavfi:testsrc=duration=5:size=1920x1080:rate=30"