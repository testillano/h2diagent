#!/bin/bash
set -e
registry=ghcr.io/testillano
docker build --target unit-test -t ${registry}/h2diagent_ut:latest .
docker run --rm ${registry}/h2diagent_ut:latest "$@"
