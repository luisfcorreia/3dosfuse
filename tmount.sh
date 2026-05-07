#!/bin/bash

fusermount -u ./mnt

./build/plus3fuse ./spectrum.img ./mnt -f
