#!/bin/sh
# Rebuilds ~/omni_video_generator/.venv: the same packages as the comfyenv
# conda environment ComfyUI was proven with (Python 3.14.6, torch 2.13.0
# cu130), but managed by uv and kept entirely inside this folder.
#
# The conda-only packages of comfyenv (openssl, sqlite, tk, zlib, ncurses,
# readline, libffi, ...) are the interpreter's own C libraries; uv's
# standalone Python build bundles them, so they are not installed separately.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
export UV_CACHE_DIR="$ROOT/.cache/uv"
export UV_PYTHON_INSTALL_DIR="$ROOT/.cache/uv-python"
export UV_PYTHON_PREFERENCE=only-managed
export XDG_CACHE_HOME="$ROOT/.cache"
export HF_HOME="$ROOT/.cache/hf"
export TMPDIR="$ROOT/.tmp"
mkdir -p "$ROOT/.cache" "$ROOT/.tmp"
cd "$ROOT"
uv python install 3.14.6
[ -x .venv/bin/python ] || uv venv --python 3.14.6 .venv
uv pip install --python .venv/bin/python -r env/comfyenv-requirements.txt \
    --extra-index-url https://download.pytorch.org/whl/cu130 --index-strategy unsafe-best-match
.venv/bin/python -I -c "import torch; print('torch', torch.__version__, 'CUDA', torch.cuda.is_available())"
