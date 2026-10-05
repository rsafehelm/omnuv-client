// node app/omnuv/test/framerates_test.js — Stream settings' frame rates, without a Qt.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

// `FRAMERATES_JS` points at a broken copy when checks.sh proves this can fail.
const src = fs.readFileSync(process.env.FRAMERATES_JS || path.join(__dirname, '..', 'framerates.js'), 'utf8')
    .split('\n').filter((l) => !l.startsWith('.pragma')).join('\n');
const F = {};
vm.runInNewContext(src, F);
const plain = (v) => JSON.parse(JSON.stringify(v));

// The operator's 360 Hz laptop, V-Sync off (the default): the usual rates and
// 480, never 72 or 180.
assert.deepStrictEqual(plain(F.offered([360], 360, false)), [30, 60, 90, 120, 144, 165, 240, 360, 480]);
// A display's own rate joins them.
assert.deepStrictEqual(plain(F.offered([75], 75, false)), [30, 60, 75, 90, 120, 144, 165, 240, 360, 480]);
// V-Sync on: the rates even on a 360 Hz screen first, in order, then the rest.
assert.deepStrictEqual(plain(F.offered([360], 360, true)), [30, 60, 90, 120, 360, 144, 165, 240, 480]);
// No display read: still the usual list.
assert.deepStrictEqual(plain(F.offered([], 0, false)), [30, 60, 90, 120, 144, 165, 240, 360, 480]);
assert.strictEqual(F.evenOn(480, 240), true);
assert.strictEqual(F.evenOn(360, 144), false);
console.log('frame rates: 6 cases hold');
