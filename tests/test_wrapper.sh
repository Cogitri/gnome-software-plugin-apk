#!/bin/bash
#
# Copyright (C) 2024 Pablo Correa Gomez
#

set -ex

python3 -m dbusmock --session --template "$(dirname "$0")"/apkpolkit2.py &

sleep 1

trap 'kill %1' EXIT

"$@"
