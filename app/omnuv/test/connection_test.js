// Execute the shipped QML orchestration functions with controlled backends.
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const source = fs.readFileSync(__dirname + '/../OmnuvView.qml', 'utf8');
function extract(name) {
    const start = source.indexOf('function ' + name + '(');
    assert(start >= 0, name);
    const body = source.indexOf('{', start);
    let depth = 1, end = body + 1;
    while (depth && end < source.length) {
        if (source[end] === '{') depth++;
        if (source[end] === '}') depth--;
        end++;
    }
    assert.equal(depth, 0);
    return source.slice(start, end);
}
function setup() {
    const state = { epoch: 1, machines: [{id:'a',host:'a.internal'},{id:'b',host:'b.internal'}], added: [], opened: [], completed:0, logs: [], shown: [] };
    const s = {
        activeTarget:null, pendingTarget:null, delivering:false, chooseApp:false, justPaired:'',
        busySince:0, busyNoticed:false, staleAfterMs:20000, abandonedAdds:0,
        qsTr(t){ const tr=v=>{ const x=new String(v); x.arg=w=>tr(x.replace('%1',w)); return x; }; return tr(t); },
        console:{info(m){state.logs.push(m);}},
        settle:{stop(){}}, appWait:{stop(){}}, pairing:{close(){}}, message:{show(m){state.shown.push(m);}},
        networkMove:{close(){}}, revokeEnrollment:{close(){}},
        Omnuv:{
            connectionTarget(row){ return {...state.machines[row],context:state.epoch}; },
            targetRow(t){return t && t.context===state.epoch ? state.machines.findIndex(m=>m.id===t.id && m.host===t.host) : -1;},
            finishPairing(){state.completed++;},
            machines:{streamedAt(){return true;}},
        },
        ComputerManager:{addNewHostManually(host){state.added.push(host);}, cancelPairing(host){state.cancelled=host;}},
        hostIndexFor(){return -1;}, openHost(t){state.opened.push(t);},
    };
    s.root=s; vm.createContext(s);
    for (const name of ['cancelConnection','validTarget','busyTarget','connectTo','connectTarget','onComputerAddCompleted','pairingFinished','onConnectionContextChanged'])
        vm.runInContext(extract(name),s);
    return {s,state};
}
{
    const {s,state}=setup();
    s.connectTo(0,false); s.connectTo(1,true);
    assert.deepEqual(state.added,['a.internal']); assert.equal(s.chooseApp,false);
    state.machines.reverse(); s.onComputerAddCompleted(true,false);
    assert.equal(state.opened[0].id,'a'); assert.equal(s.Omnuv.targetRow(state.opened[0]),1);
}
{
    const {s,state}=setup();
    s.connectTo(0,false); state.epoch++; s.onConnectionContextChanged();
    // The old add is still in flight. The new press starts its own at once,
    // and the old one's completion is consumed, never finishing the new one.
    s.connectTo(1,false);
    assert.equal(state.added[1],'b.internal');
    s.onComputerAddCompleted(true,false); assert.equal(state.opened.length,0, 'the abandoned add must not open the new target');
    s.onComputerAddCompleted(true,false); assert.equal(state.opened.length,1); assert.equal(state.opened[0].host,'b.internal');
}
{
    const {s,state}=setup();
    s.connectTo(0,false); const target=s.activeTarget;
    s.pendingTarget=null; s.delivering=true; s.pairing.host=target.host; s.pairing.target=target;
    s.pairingFinished('b.internal',undefined); assert(s.delivering); assert.equal(state.opened.length,0);
    s.cancelConnection(); assert.equal(state.cancelled, target.host);
    s.pairingFinished('a.internal',undefined); assert(!s.delivering); assert.equal(state.opened.length,0);
}
{
    const {s,state}=setup();
    s.connectTo(0,false); s.cancelConnection();
    // Same model later reappears; cancellation still wins.
    s.onComputerAddCompleted(true,false); assert.equal(state.opened.length,0);
    state.machines[0].host='changed.internal';
    s.connectTarget({id:'a',host:'a.internal',context:1},false); assert.equal(state.added.length,1);
}
console.log('connection_test: 4 asynchronous identity and single-flight scenarios passed');

// **Play never does nothing silently** (30 September 2026).
{
    // A second press while one is in progress says so, and the press after
    // that starts over, as the message promises.
    const {s,state}=setup();
    s.connectTo(0,false);
    s.connectTo(0,false);
    assert.equal(state.added.length,1, 'a press while busy must not start a second attempt');
    assert.ok(state.shown.some(m=>/Still connecting/.test(m)), 'a press while busy must say so');
    assert.ok(state.logs.some(m=>/not starting another/.test(m)), 'and log it');
    s.connectTo(0,false);
    assert.equal(state.added.length,2, 'the press after the notice starts over');
    assert.ok(state.logs.some(m=>/asked to start over/.test(m)));
}
{
    // An attempt left pending past the limit is cleared by the next press.
    const {s,state}=setup();
    s.connectTo(0,false);
    s.busySince = Date.now() - 60000;
    s.connectTo(0,false);
    assert.equal(state.added.length,2, 'a stale attempt must not block Play');
    assert.equal(state.shown.length,0, 'and needs no notice');
}
{
    // An attempt whose machine has gone is cleared, whatever its age.
    const {s,state}=setup();
    s.connectTo(0,false);
    state.machines.shift();
    s.connectTo(0,false);
    assert.equal(state.added.length,2);
    assert.ok(state.logs.some(m=>/its machine is gone/.test(m)));
}
{
    // A machine no longer listed is said, not ignored.
    const {s,state}=setup();
    s.connectTarget({id:'z',host:'z.internal',context:1},false);
    assert.equal(state.added.length,0);
    assert.ok(state.shown.some(m=>/no longer listed/.test(m)));
}
console.log('connection tests: ok');

