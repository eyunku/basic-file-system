#!/bin/bash
make clean
make all
rm -rf mnt
mkdir mnt
./create_disk.sh
./mkfs.wfs disk
./mount.wfs -f -s disk mnt
