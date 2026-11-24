#!/usr/bin/env sh
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

set -eu
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# Pass --baseline-webdriver for a paired-build library check before capturing sites.
exec python3 "$script_dir/SiteSmoke/site-smoke.py" ci "$@"
