#!/bin/sh
# Flex Launcher entry: Steam Link (app profile "steamlink"). The daemon hands the Steam
# Controller to Steam Link while it runs and takes it back afterwards.
exec /usr/local/bin/steampi-cec --launch steamlink
