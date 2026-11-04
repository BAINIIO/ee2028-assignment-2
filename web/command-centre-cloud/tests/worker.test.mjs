import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {DatabaseSync} from 'node:sqlite';
import {createWorker} from '../dist/server/index.js';

const BASE=1793768400,ORIGIN='https://garden-guard.test';
const TOKEN='isolated-worker-test-credential-not-a-real-secret';
const schema=readFileSync(new URL('../drizzle/0000_garden_guard_state.sql',import.meta.url),'utf8');
class D1 {
  constructor(){this.db=new DatabaseSync(':memory:');this.db.exec(schema);}
  prepare(sql){const statement=this.db.prepare(sql);return{bind:(...values)=>({
    async first(){return statement.get(...values)||null;},
    async run(){return{success:true,meta:{changes:Number(statement.run(...values).changes)}};},
  })};}
  count(){return this.db.prepare('SELECT COUNT(*) AS count FROM garden_guard_state').get().count;}
}
function telemetry(overrides={}){return{
  type:'snapshot',id:'GG01',seq:5,tick:5000,mode:'standby',attacked:false,round:'active',target:'none',
  energy:3,lives:3,hits:1,score:25,rescue_ms:0,target_ms:0,valid:63,abnormal:0,unavailable:0,
  t:27.5,p:1005.25,h:63,a:[1,-2,9.81],g:[0.1,-0.2,0.3],m:[10,-20,30],
  ir:true,mission:0,nfc:true,nfc_taps:2,wifi:true,wifi_drop:0,received_at:BASE,...overrides};}
function snapshot(overrides={}){return{
  revision:5,generated_at:BASE,server_instance:'local-server-one',telemetry:telemetry(),
  link:{source:'wifi',status:'online',wifi:true,uart:false,last_seen:BASE,age_ms:0},events:[],...overrides};}
function empty(overrides={}){return snapshot({telemetry:null,link:{source:'none',status:'offline',wifi:false,uart:false,last_seen:null,age_ms:null},...overrides});}
function outputs(overrides={}){return{
  oled:{width:128,height:64,format:'ssd1306-pages',
    data:Buffer.from(Array.from({length:1024},(_,i)=>i%256)).toString('base64'),
    received_at:BASE,sequence:2,status:'online'},
  uart:{lines:[{id:1,at:BASE,text:'  T=27.500 C\t<script>not HTML</script>  '}],received_at:BASE,status:'online'},
  ...overrides};}
function harness(){
  let now=BASE;const DB=new D1(),env={DB,RELAY_TOKEN:TOKEN},worker=createWorker({now:()=>now});
  return{DB,env,worker,setTime(value){now=value;},
    async get(){const response=await worker.fetch(new Request(`${ORIGIN}/api/state`),env);assert.equal(response.status,200);return response.json();},
    async upload(value,options={}){
      const body=JSON.stringify({version:1,relay_id:'relay-process-one',snapshot:value,acknowledgements:[],...options.envelope});
      const response=await worker.fetch(new Request(`${ORIGIN}/api/relay/sync`,{method:'POST',
        headers:{'content-type':'application/json',authorization:`Bearer ${TOKEN}`,...options.headers},body}),env);
      return{status:response.status,body:await response.json()};
    },
  };
}
function resetEvent(id=1){return{id,at:BASE+1,kind:'reset',message:'A new board session entered standby.',level:'info',source:'wifi'};}

test('schema journal identifies the initial bounded schema migration',()=>{
  const journal=JSON.parse(readFileSync(new URL('../drizzle/meta/_journal.json',import.meta.url),'utf8'));
  assert.equal(journal.dialect,'sqlite');assert.equal(journal.entries[0].tag,'0000_garden_guard_state');
  assert.doesNotMatch(schema,/INSERT|UPDATE|DELETE/i);assert.equal(new D1().count(),0);
});
test('an empty database returns offline null data and no synthetic readings',async()=>{
  const h=harness(),state=await h.get();assert.equal(state.telemetry,null);assert.equal(state.link.source,'none');
  assert.equal(state.link.status,'offline');assert.equal(state.link.age_ms,null);assert.deepEqual(state.events,[]);
  assert.equal(state.permissions.mission_write,false);assert.equal(state.server.relay_status,'offline');
  assert.deepEqual(state.outputs,{oled:null,uart:{lines:[],received_at:null,status:'offline'}});
});
test('embedded OLED and UART page has no game widgets or exposed relay credential',async()=>{
  const h=harness();
  for(const path of ['/','/drone/GG01','/drone/GG01/','/app.css','/app.js']){
    const response=await h.worker.fetch(new Request(ORIGIN+path),h.env);assert.equal(response.status,200);
    const source=await response.text();assert.ok(source.length>100);assert.ok(!source.includes(TOKEN));
    assert.ok(!source.includes('Sites Worker ESM starter'));
  }
  const page=await(await h.worker.fetch(new Request(ORIGIN),h.env)).text();
  assert.match(page,/<h1[^>]*>OLED<\/h1>/);assert.match(page,/<h2[^>]*>UART<\/h2>/);
  assert.equal((page.match(/<canvas\b/g)||[]).length,1);assert.equal((page.match(/<pre\b/g)||[]).length,1);
  assert.doesNotMatch(page,/id="(?:sensor-grid|mission-options|history-chart|energy-meter)"/);
  const script=await(await h.worker.fetch(new Request(`${ORIGIN}/app.js`),h.env)).text();
  assert.match(script,/\/api\/state/);assert.doesNotMatch(script,/new EventSource/);
  assert.doesNotMatch(script,/\/api\/mission/);
});
test('ingest requires the secret and never writes on failed authorization',async()=>{
  const h=harness();
  for(const authorization of ['', 'Bearer wrong-token']){
    const result=await h.upload(snapshot(),{headers:{authorization}});assert.equal(result.status,401);
  }
  delete h.env.RELAY_TOKEN;assert.equal((await h.upload(snapshot())).status,503);assert.equal(h.DB.count(),0);
});
test('authenticated sync preserves six sensors, axes, true receipt time and source',async()=>{
  const h=harness(),input=snapshot();input.server={private:'not-for-public'};input.telemetry.extra='discard';
  assert.deepEqual(await h.upload(input),{status:202,body:{ok:true,accepted:true,commands:[]}});
  const state=await h.get();assert.equal(state.telemetry.t,27.5);assert.equal(state.telemetry.p,1005.25);
  assert.equal(state.telemetry.h,63);assert.deepEqual(state.telemetry.a,[1,-2,9.81]);
  assert.deepEqual(state.telemetry.g,[0.1,-0.2,0.3]);assert.deepEqual(state.telemetry.m,[10,-20,30]);
  assert.equal(state.telemetry.received_at,BASE);assert.equal(state.link.source,'wifi');assert.equal(state.link.wifi,true);
  assert.equal(state.telemetry.extra,undefined);assert.equal(state.server.private,undefined);
  assert.equal(state.server.url,ORIGIN);assert.equal(state.server.board_ingest_port,null);
});
test('repeated public reads age a frozen upload to stale then offline',async()=>{
  const h=harness();await h.upload(snapshot());const original=await h.get();
  h.setTime(BASE+4);const stale=await h.get();assert.equal(stale.link.status,'stale');assert.equal(stale.link.age_ms,4000);
  assert.equal(stale.telemetry.received_at,BASE);assert.equal(stale.revision,original.revision);
  h.setTime(BASE+11);const offline=await h.get();assert.equal(offline.link.status,'offline');assert.equal(offline.link.age_ms,11000);
  assert.equal(offline.telemetry.received_at,BASE);assert.equal(offline.server.relay_status,'offline');
});
test('local offline is respected, existing board age is added, UART never proves WiFi',async()=>{
  const h=harness();await h.upload(snapshot({link:{source:'wifi',status:'offline',wifi:false,uart:false,last_seen:BASE,age_ms:0}}));
  assert.equal((await h.get()).link.status,'offline');
  h.setTime(BASE+1);await h.upload(snapshot({revision:6,generated_at:BASE+1,
    link:{source:'uart',status:'online',wifi:false,uart:true,last_seen:BASE-4,age_ms:5000}}));
  h.setTime(BASE+2);const state=await h.get();assert.equal(state.link.source,'uart');assert.equal(state.link.wifi,false);
  assert.equal(state.link.status,'stale');assert.equal(state.link.age_ms,6000);
});
test('an explicit empty offline relay upload remains empty with a live bridge',async()=>{
  const h=harness();assert.equal((await h.upload(empty())).status,202);
  const state=await h.get();assert.equal(state.telemetry,null);assert.equal(state.link.status,'offline');
  assert.equal(state.server.relay_status,'online');assert.equal(state.link.age_ms,null);
});
test('event-only termination with no board snapshot remains sticky and honest',async()=>{
  const h=harness(),value=empty();value.link.status='terminated';
  value.events=[{id:1,at:BASE,kind:'terminated',message:'Drone terminated.',level:'critical',source:'uart'}];
  assert.equal((await h.upload(value)).status,202);h.setTime(BASE+60);const state=await h.get();
  assert.equal(state.telemetry,null);assert.equal(state.link.status,'terminated');assert.equal(state.link.source,'none');
  assert.equal(state.events[0].kind,'terminated');
  assert.equal((await h.upload(empty({revision:6,generated_at:BASE+60}))).status,409);
});
test('terminal events precede final telemetry and require a new reset session to clear',async()=>{
  const h=harness(),value=snapshot({telemetry:telemetry({mode:'battle',target:'human'})});value.link.status='terminated';
  assert.equal((await h.upload(value)).status,202);h.setTime(BASE+1);
  assert.equal((await h.get()).link.status,'terminated');assert.equal((await h.get()).telemetry.mode,'battle');
  assert.equal((await h.upload(snapshot({revision:6,generated_at:BASE+1,telemetry:telemetry({received_at:BASE+1})}))).status,409);
  const reset=snapshot({revision:7,generated_at:BASE+1,telemetry:telemetry({received_at:BASE+1}),events:[resetEvent()]});
  assert.equal((await h.upload(reset)).status,202);assert.equal((await h.get()).link.status,'online');
});
test('new backend standby can reset termination but battle and UART cannot',async()=>{
  const h=harness(),end=snapshot({telemetry:telemetry({mode:'terminated'})});await h.upload(end);h.setTime(BASE+1);
  const next=snapshot({revision:0,server_instance:'local-server-two',generated_at:BASE+1,telemetry:telemetry({mode:'battle',received_at:BASE+1})});
  assert.equal((await h.upload(next)).status,409);next.telemetry.mode='standby';next.link.source='uart';
  assert.equal((await h.upload(next)).status,409);next.link.source='wifi';
  assert.equal((await h.upload(next)).status,202);assert.equal((await h.get()).link.status,'online');
});
test('old revisions, retired servers and retired relay processes cannot regress state',async()=>{
  const h=harness();await h.upload(snapshot());h.setTime(BASE+1);
  assert.equal((await h.upload(snapshot({revision:4,generated_at:BASE+1}))).status,409);
  const newer=snapshot({revision:0,generated_at:BASE+1,server_instance:'local-server-two'});
  assert.equal((await h.upload(newer,{envelope:{relay_id:'relay-process-two'}})).status,202);
  assert.equal((await h.upload(snapshot({revision:99,generated_at:BASE+1}))).status,409);
  assert.equal((await h.upload({...newer,revision:1},{envelope:{relay_id:'relay-process-one'}})).status,409);
});
test('duplicate sync is idempotent and does not refresh stale bridge time',async()=>{
  const h=harness(),value=snapshot();await h.upload(value);const first=await h.get();h.setTime(BASE+11);
  assert.deepEqual((await h.upload(value)).body,{ok:true,accepted:false,commands:[]});
  const state=await h.get();assert.equal(state.revision,first.revision);assert.equal(state.server.relay_status,'offline');
});
test('same-revision newer heartbeats are ordered without changing board receipt time',async()=>{
  const h=harness();await h.upload(snapshot());h.setTime(BASE+1);
  const value=snapshot({generated_at:BASE+1});value.link.age_ms=1000;
  assert.equal((await h.upload(value)).status,202);assert.equal((await h.get()).revision,2);
  assert.equal((await h.get()).telemetry.received_at,BASE);assert.equal((await h.upload(snapshot())).status,409);
});
test('concurrent SQLite compare-and-swap cannot overwrite a newer upload',async()=>{
  const h=harness();await h.upload(snapshot());h.setTime(BASE+1);
  await Promise.all([h.upload(snapshot({revision:6,generated_at:BASE+1})),h.upload(snapshot({revision:7,generated_at:BASE+1}))]);
  const row=h.DB.db.prepare('SELECT state_json FROM garden_guard_state').get();
  assert.equal(JSON.parse(row.state_json).snapshot.revision,7);
});
test('malformed bounded fields and streamed oversized payloads cannot write',async()=>{
  const h=harness();
  const invalid=[snapshot({telemetry:telemetry({g:[1,2]})}),snapshot({telemetry:telemetry({energy:11})}),
    snapshot({telemetry:telemetry({t:'NaN'})}),snapshot({revision:-1}),snapshot({events:Array(201).fill(resetEvent())})];
  for(const value of invalid)assert.equal((await h.upload(value)).status,400);
  const body=new ReadableStream({start(controller){controller.enqueue(new Uint8Array(40000));controller.enqueue(new Uint8Array(40000));controller.close();}});
  const response=await h.worker.fetch(new Request(`${ORIGIN}/api/relay/sync`,{method:'POST',duplex:'half',
    headers:{authorization:`Bearer ${TOKEN}`,'content-type':'application/json'},body}),h.env);
  assert.equal(response.status,413);assert.equal(h.DB.count(),0);
});
test('public mission writes stay forbidden and relay never produces commands',async()=>{
  const h=harness();for(const method of ['GET','POST']){
    const response=await h.worker.fetch(new Request(`${ORIGIN}/api/mission`,{method}),h.env);assert.equal(response.status,403);
  }
  assert.deepEqual((await h.upload(snapshot())).body.commands,[]);
  assert.equal((await h.upload(snapshot(),{envelope:{acknowledgements:[{id:'x',status:'delivered'}]}})).status,400);
});
test('legacy snapshots supply empty outputs without inventing frame or UART text',async()=>{
  const h=harness();await h.upload(snapshot());
  assert.deepEqual((await h.get()).outputs,{oled:null,uart:{lines:[],received_at:null,status:'offline'}});
});
test('output-only capture preserves exact frame bytes, raw text and source timestamps',async()=>{
  const h=harness(),captured=outputs();captured.oled.private='discard';captured.uart.lines[0].html='discard';
  const value=empty({outputs:captured});assert.equal((await h.upload(value)).status,202);
  const state=await h.get();assert.equal(state.telemetry,null);assert.equal(state.link.status,'offline');
  assert.equal(state.link.wifi,false);assert.equal(state.outputs.oled.status,'online');
  assert.equal(state.outputs.oled.received_at,BASE);assert.equal(state.outputs.oled.sequence,2);
  assert.deepEqual(Buffer.from(state.outputs.oled.data,'base64'),Buffer.from(captured.oled.data,'base64'));
  assert.equal(state.outputs.oled.private,undefined);assert.equal(state.outputs.uart.lines[0].html,undefined);
  assert.equal(state.outputs.uart.lines[0].text,captured.uart.lines[0].text);
  assert.equal(state.outputs.uart.received_at,BASE);
});
test('frozen raw outputs age independently and retain the last captured content',async()=>{
  const h=harness(),captured=outputs();await h.upload(empty({outputs:captured}));
  h.setTime(BASE+4);let state=await h.get();
  assert.equal(state.outputs.oled.status,'stale');assert.equal(state.outputs.uart.status,'stale');
  h.setTime(BASE+11);state=await h.get();
  assert.equal(state.outputs.oled.status,'offline');assert.equal(state.outputs.uart.status,'offline');
  assert.equal(state.outputs.oled.data,captured.oled.data);assert.deepEqual(state.outputs.uart.lines,captured.uart.lines);
  assert.equal(state.outputs.oled.received_at,BASE);assert.equal(state.outputs.uart.received_at,BASE);
});
test('a fresh relay upload never renews old output capture times or a closed channel',async()=>{
  const h=harness(),captured=outputs();await h.upload(empty({outputs:captured}));h.setTime(BASE+12);
  await h.upload(empty({revision:6,generated_at:BASE+12,outputs:captured}));
  let state=await h.get();assert.equal(state.server.relay_status,'online');
  assert.equal(state.outputs.oled.status,'offline');assert.equal(state.outputs.uart.status,'offline');
  captured.oled.received_at=BASE+12;captured.oled.sequence=4;
  captured.uart.status='offline';captured.uart.received_at=BASE+12;
  await h.upload(empty({revision:7,generated_at:BASE+12,outputs:captured}));state=await h.get();
  assert.equal(state.outputs.oled.status,'online');assert.equal(state.outputs.uart.status,'offline');
});
test('invalid OLED dimensions, byte count, encoding and incomplete sequences cannot write',async()=>{
  const h=harness();
  const invalid=[{width:64},{height:128},{format:'reconstructed-pixels'},{sequence:0},{sequence:3},
    {data:Buffer.alloc(1023).toString('base64')},{data:Buffer.alloc(1025).toString('base64')},
    {data:'!'.repeat(1366)+'=='},{received_at:BASE+301}];
  const noncanonical=Buffer.alloc(1024).toString('base64');
  invalid.push({data:noncanonical.slice(0,-3)+'B=='});
  for(const change of invalid){const captured=outputs();Object.assign(captured.oled,change);
    assert.equal((await h.upload(empty({outputs:captured}))).status,400);}
  assert.equal(h.DB.count(),0);
});
test('UART limits count UTF-8 bytes, preserve whitespace and require ordered capture ids',async()=>{
  const h=harness();
  const invalid=[
    {lines:Array.from({length:101},(_,id)=>({id,at:BASE,text:''}))},
    {lines:[{id:1,at:BASE,text:'\u00e9'.repeat(1025)}]},
    {lines:[{id:1,at:BASE,text:'a'},{id:1,at:BASE,text:'b'}]},
    {lines:[{id:1,at:BASE,text:'a'}],received_at:null},
    {lines:[],received_at:null,status:'online'},
    {lines:[{id:1,at:BASE,text:42}]},
    {lines:Array.from({length:100},(_,id)=>({id,at:BASE,text:'x'.repeat(id===99?241:240)}))},
  ];
  for(const change of invalid){const captured=outputs();Object.assign(captured.uart,change);
    assert.equal((await h.upload(empty({outputs:captured}))).status,400);}
  const captured=outputs({oled:null});captured.uart.lines=Array.from({length:100},(_,id)=>({id,at:BASE,text:' '.repeat(240)}));
  assert.equal((await h.upload(empty({outputs:captured}))).status,202);
  assert.equal((await h.get()).outputs.uart.lines[99].text,' '.repeat(240));
});
test('raw output termination is sticky and only a new session with fresh capture can clear it',async()=>{
  const h=harness(),captured=outputs();captured.oled.status='terminated';
  assert.equal((await h.upload(empty({outputs:captured}))).status,202);h.setTime(BASE+60);
  let state=await h.get();assert.equal(state.link.status,'terminated');
  assert.equal(state.outputs.oled.status,'terminated');assert.equal(state.outputs.uart.status,'terminated');
  captured.oled.status='online';captured.oled.sequence=4;captured.oled.received_at=BASE+60;
  assert.equal((await h.upload(empty({revision:6,generated_at:BASE+60,outputs:captured}))).status,409);
  assert.equal((await h.upload(empty({server_instance:'local-server-two',revision:0,generated_at:BASE+60}))).status,409);
  captured.oled.received_at=BASE;captured.uart.status='offline';
  assert.equal((await h.upload(empty({server_instance:'local-server-two',revision:0,generated_at:BASE+60,outputs:captured}))).status,409);
  captured.oled.received_at=BASE+60;
  assert.equal((await h.upload(empty({server_instance:'local-server-two',revision:0,generated_at:BASE+60,outputs:captured}))).status,202);
  state=await h.get();assert.equal(state.link.status,'offline');assert.equal(state.outputs.oled.status,'online');
});
test('a new UART capture session can resume after terminal silence with no OLED available',async()=>{
  const h=harness(),captured=outputs({oled:null});captured.uart.status='terminated';
  assert.equal((await h.upload(empty({outputs:captured}))).status,202);h.setTime(BASE+5);
  captured.uart={lines:[{id:1,at:BASE+5,text:'Zombie game ready; sensors=0x3f/0x3f'}],received_at:BASE+5,status:'online'};
  assert.equal((await h.upload(empty({server_instance:'local-server-two',revision:0,generated_at:BASE+5,outputs:captured}))).status,202);
  assert.equal((await h.get()).outputs.uart.status,'online');
});
