#!/usr/bin/env bash
# How many tokens does the Track creator's AI writer really use on a real model, and does the music harness use
# fewer than writing the song in one answer?
#
# What it does:
#   1. Makes a small brief (a short song with drums, bass, chords and a lead) from a description.
#   2. For each reasoning level (the model's own, low, medium, high) writes that song twice with your AI
#      provider: once in one answer ("one-shot") and once with the music harness's tools ("harness").
#   3. Prints, for each, what the provider reported it used (input, the part read from its cache, what it cleared
#      from old tool calls, output, reasoning), plus the checks' score of the song.
#
# Your key is asked for here and kept only in this run's memory: it is not saved, logged or printed.
# It uses real tokens on your account (8 small songs; more at high reasoning); the table shows exactly how
# many, as your provider reported them.
#
# Needs: uv (`sudo pacman -S uv`), internet, an API key.
#
# Usage (from the spinus-daw folder):
#   tools/owner-checks/ai-compose-tokens.sh                         # Anthropic, its default model
#   tools/owner-checks/ai-compose-tokens.sh anthropic claude-opus-5-5
#   tools/owner-checks/ai-compose-tokens.sh openai gpt-5            # an OpenAI key
#   tools/owner-checks/ai-compose-tokens.sh openrouter anthropic/claude-sonnet-5.5
# Paste the whole output into the pull request that asked for it.

set -u
PROVIDER=${1:-anthropic}
MODEL=${2:-}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "Spinus AI writer token check: provider $PROVIDER, model ${MODEL:-the default}"
read -r -s -p "Paste your $PROVIDER API key (it is not shown or saved): " SPINUS_CHECK_KEY
echo
if [ -z "$SPINUS_CHECK_KEY" ]; then
    echo "No key given; nothing was sent."
    exit 1
fi
export SPINUS_CHECK_KEY

cd "$ROOT/ai" || exit 1
uv run --python 3.12 python -m spinus_ai.harness brief \
    --describe "a short upbeat synth pop song at 118 BPM in A minor" \
    --parts drums:drums,bass:bass,chords:chords,lead:lead --seed 7 -o "$WORK/brief.json" || exit 1

ARGS=(--brief "$WORK/brief.json" --provider "$PROVIDER" --key-env SPINUS_CHECK_KEY --efforts ",low,medium,high")
[ -n "$MODEL" ] && ARGS+=(--model "$MODEL")
echo "Writing 8 songs (2 ways x 4 reasoning levels); this takes a few minutes..."
uv run --python 3.12 python -m spinus_ai.harness eval "${ARGS[@]}"
STATUS=$?
unset SPINUS_CHECK_KEY
echo
echo "Columns: total = input + output tokens as your provider reported them; cached = input read from its cache"
echo "(cheaper); cleared = old tool calls it cleared; issues = what Spinus's checks still find."
exit $STATUS
