#!/bin/bash

set -e

npm i
npm run lint:fix
npm run format
npm run build
npm run test
find e2e-test -iname '*.cc' -o -iname '*.h' | xargs clang-format -i
npm run copy-to-client:check
