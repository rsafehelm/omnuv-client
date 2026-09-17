// node app/omnuv/test/estate_test.js — the estate's words, without a Qt.
//
// `estate.js` is a QML JavaScript library, whose first line (`.pragma
// library`) is not JavaScript. It is dropped and the rest evaluated as it is.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

// `ESTATE_JS` points at a deliberately broken copy when CI proves this can fail.
const src = fs.readFileSync(process.env.ESTATE_JS || path.join(__dirname, '..', 'estate.js'), 'utf8')
    .split('\n').filter((l) => !l.startsWith('.pragma') && !l.startsWith('.import')).join('\n');
const E = {};
vm.runInNewContext(src, E);

// Arrays made inside the context carry that context's prototype, which a
// strict deep comparison refuses; compare plain copies.
const plain = (v) => JSON.parse(JSON.stringify(v));

let failed = 0;
function check(name, fn) {
    try { fn(); console.log(`ok   ${name}`); }
    catch (e) { failed++; console.log(`FAIL ${name}\n     ${e.message}`); }
}

const row = (o) => Object.assign({ epoch: 1, at: '2026-09-17T10:00:00Z', actor: 'you',
                                   resource: 'instances', resource_id: '5e30afa9-0000-4000-8000-000000000000',
                                   resource_name: null, intent: 'create', reason: null, chained: true }, o);

check('a named machine reads as its name and the verb', () => {
    assert.strictEqual(E.sentence(row({ resource_name: 'gpu-2' })), 'gpu-2 requested');
});
check('a uuid is never printed; the subject stands in for it', () => {
    assert.strictEqual(E.sentence(row({ resource: 'api_keys' })), 'An API key was created');
});
check('the sentinel `?` is as unprintable as a uuid', () => {
    assert.strictEqual(E.sentence(row({ resource: 'devices', resource_id: '?', intent: 'delete' })), 'A device was removed');
});
check('an address is its own name', () => {
    assert.strictEqual(E.sentence(row({ resource: 'network_addresses', resource_id: '10.200.1.5', intent: 'delete' })),
                       '10.200.1.5 released');
});
check('a reason that repeats the verb is dropped', () => {
    assert.strictEqual(E.sentence(row({ resource: 'api_keys', reason: 'the buyer created an api key' })),
                       'An API key was created');
});
check('a reason that adds something survives, without its actor', () => {
    assert.strictEqual(E.sentence(row({ resource_name: 'gpu-3', reason: 'the marketplace parked it: no free RTX 3090, nothing is charged' })),
                       'gpu-3 requested — parked it: no free RTX 3090, nothing is charged');
});
check('an unmapped table is still said, in the least wrong words', () => {
    assert.strictEqual(E.sentence(row({ resource: 'volumes', intent: 'update' })), 'A volume was changed');
});
check('an unknown intent is a change, never an invented verb', () => {
    assert.strictEqual(E.sentence(row({ resource_name: 'gpu-1', intent: 'migrate' })), 'gpu-1 changed');
});

const now = Date.parse('2026-09-17T12:00:00Z');
check('ago: the console’s vocabulary', () => {
    assert.strictEqual(E.ago('2026-09-17T11:59:40Z', now), 'just now');
    assert.strictEqual(E.ago('2026-09-17T11:56:00Z', now), '4m ago');
    assert.strictEqual(E.ago('2026-09-17T09:00:00Z', now), '3h ago');
    assert.strictEqual(E.ago('2026-09-10T12:00:00Z', now), '7d ago');
    assert.strictEqual(E.ago('2026-09-24T12:00:00Z', now), 'in 7d');
    assert.strictEqual(E.ago(null, now), 'never');
    assert.strictEqual(E.ago('not a date', now), '');
});
check('money: cents, and below a cent never rounds to zero', () => {
    assert.strictEqual(E.money('3.4213', 'EUR'), '€3.42');
    assert.strictEqual(E.money('0.0013', 'EUR'), '€0.0013');
    assert.strictEqual(E.money('0', 'USD'), '$0.00');
    assert.strictEqual(E.money('x', 'EUR'), '—');
});
check('series: thirty days, a missing day is zero, oldest first', () => {
    const s = E.series({ by_day: [{ day: '2026-09-17', cost: '1.5' }, { day: '2026-08-19', cost: '0.25' }] }, now);
    assert.strictEqual(s.length, 30);
    assert.strictEqual(s[29], 1.5);
    assert.strictEqual(s[0], 0.25);
    assert.strictEqual(s.filter((v) => v !== 0).length, 2);
    assert.deepStrictEqual(plain(E.series(undefined, now)), []);
});
check('pulse: an unread source is left out, never drawn as zero', () => {
    assert.deepStrictEqual(plain(E.pulse({}, false, undefined)), []);
    const p = E.pulse({ Running: 2, Stopped: 1 }, true, undefined);
    assert.deepStrictEqual(plain(p.map((c) => c.label)), ['running', 'starting', 'stopped']);
    const r = E.pulse({ Running: 1, 'Needs attention': 2 }, true, undefined);
    assert.deepStrictEqual(plain(r.map((c) => [c.label, c.n])), [['running', 1], ['starting', 0], ['attention', 2], ['stopped', 0]]);
    const q = E.pulse({}, false, [{ status: 'WAITING' }, { status: 'PLACED' }]);
    assert.deepStrictEqual(plain(q.map((c) => [c.label, c.n])), [['waiting', 1]]);
});
check('one banner: only stale reads are named', () => {
    assert.strictEqual(E.staleOf([{ state: 'current' }, { state: 'stale' }, null, { state: 'unavailable' }]).length, 1);
});

console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
