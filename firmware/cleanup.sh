#!/bin/bash
find . -type d -name "build" -exec rm -rf {} +
find . -type f -name "sdkconfig" -delete
find . -type f -name "sdkconfig.old" -delete
find . -type f -name "*.crc" -delete
rm -rf managed_components
rm dependencies.lock
