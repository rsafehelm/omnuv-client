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
    const state = { epoch: 1, machines: [{id:'a',host:'a.internal'},{id:'b',host:'b.internal'}], added: [], opened: [], completed:0 };
    const s = {
        activeTarget:null, pendingTarget:null, delivering:false, chooseApp:false, justPaired:'',
        settle:{stop(){}}, appWait:{stop(){}}, pairing:{close(){}}, message:{show(){}},
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
    for (const name of ['cancelConnection','validTarget','connectTo','connectTarget','onComputerAddCompleted','pairingFinished','onConnectionContextChanged'])
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
    s.connectTo(1,false); // old add still in flight: no second task
    assert.equal(state.added.length,1);
    s.onComputerAddCompleted(true,false); assert.equal(state.opened.length,0);
    s.connectTo(1,false); assert.equal(state.added[1],'b.internal');
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
