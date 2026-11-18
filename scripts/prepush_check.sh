#!/bin/sh
# Fails if anything private or heavy is tracked: the ComfyUI copy and models,
# the user's reference project, environments, caches, generated media, user
# projects, model/audio/video files, or any file over 1 MB.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"
bad=0
files="$(git ls-files; git diff --cached --name-only)"
for f in $(printf '%s\n' "$files" | sort -u); do
    case "$f" in
        comfyui/*|reference/*|.venv/*|.cache/*|.tmp/*|.comfy-user/*|output/*|input/*|projects/*)
            echo "private path tracked: $f"; bad=1 ;;
        *.mp4|*.mkv|*.webm|*.mov|*.wav|*.flac|*.mp3|*.ogg|*.opus|*.m4a|*.aac|*.safetensors|*.gguf|*.ckpt|*.pt|*.pth|*.bin)
            echo "media or model file tracked: $f"; bad=1 ;;
    esac
    if [ -f "$f" ] && [ "$(stat -c %s "$f")" -gt 1048576 ]; then
        echo "file over 1 MB: $f"; bad=1
    fi
done
# Words from the earlier private project must not leak into the public repo.
if git grep -n -i -E 'hellfire|lastrow|molla|meena|danny|chase\b' -- . ':!scripts/prepush_check.sh' >/dev/null 2>&1; then
    echo "private project content found:"; git grep -n -i -E 'hellfire|lastrow|molla|meena|danny|chase\b' -- . ':!scripts/prepush_check.sh'; bad=1
fi
[ "$bad" -eq 0 ] && echo "prepush check passed ($(printf '%s\n' "$files" | sort -u | wc -l) files)"
exit "$bad"
