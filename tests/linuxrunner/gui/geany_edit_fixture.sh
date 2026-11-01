#!/bin/sh
# Disposable guest fixture only: the host probe checks this exact test file.
set -eu
printf 'Before editor test\n' > /tmp/linuxrunner-geany-edit.txt
exec geany --new-instance /tmp/linuxrunner-geany-edit.txt
