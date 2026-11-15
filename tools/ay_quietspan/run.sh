#!/bin/bash
# Differential test for AySound.cpp's AY_SAMPLE quiet-span path: the four stereo
# mixers are cut out of the firmware source and built twice in one binary (as
# is, and with AY_NO_QUIET_SPAN = the plain tick loop); random register changes,
# output bytes and the whole chip state compared after every call.
#   bash tools/ay_quietspan/run.sh     -> "samples N, mismatching calls 0"
set -e
cd "$(dirname "$0")"
python3 - <<'PY'
s = open('../../src/speccy/devices/sound/AySound.cpp').read()
snip = s[s.index('#define AY_TICK_COUNTERS()'):s.index('// Cold path of the bounds check below')]
for m in ('AY_MIX_ABC ', 'AY_MIX_BAC ', 'AY_MIX_ACB ', 'AY_MIX_MONO '): snip = snip.replace(m, '')
snip = snip.replace('AySound::', 'T::')
open('/tmp/ay_snip.inc', 'w').write(snip + '\n#undef AY_TICK_COUNTERS\n#undef AY_HOIST_LOCALS\n#undef AY_WRITEBACK\n#undef AY_CHVOL\n#undef AY_SAMPLE\n')
PY
g++ -O2 -w -I/tmp -o /tmp/ay_quietspan_test ay_test.cpp && /tmp/ay_quietspan_test
