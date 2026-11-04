# Run with:  source tests/scripts/source_me.sh
SOURCED_VAR="set by source_me.sh"
cd /tmp
echo "now in $(pwd); SOURCED_VAR=$SOURCED_VAR"
