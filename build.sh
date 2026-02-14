#!/bin/bash
podman run --rm -v "$(pwd):/project":Z -w /project docker.io/devkitpro/devkita64 \
  bash -c 'source /opt/devkitpro/switchvars.sh && make "$@"' -- "$@"
