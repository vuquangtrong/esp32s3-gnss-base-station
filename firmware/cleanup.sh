#!/bin/bash
find . -type d -name "build" -exec rm -rf {} +
find . -type f -name "sdkconfig" -delete
