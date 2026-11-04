#!/bin/bash
echo "this line must not run"
if [ 1 -eq 1 ]; then
    echo "missing fi below"
