#!/bin/bash

set -eu

cd vgram
bash <(curl -s https://codecov.io/bash) -X gcov
cd ..
