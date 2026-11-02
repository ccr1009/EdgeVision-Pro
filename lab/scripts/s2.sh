#!/bin/bash
cd /tmp/mppsrc/mpp-develop
echo "=== mpi.c 130-210 ==="
sed -n '130,210p' mpp/mpi.c
echo "=== demo release pattern (mpi_dec_test.c 300-330) ==="
sed -n '300,330p' test/mpi_dec_test.c
