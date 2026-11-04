/* Embedded by scripts/embed-assets.mjs; deployed Worker has no runtime imports. */
const ASSETS = {}; // __GARDEN_GUARD_ASSETS__
const MAX_BODY_BYTES = 65536, STALE_MS = 3000, OFFLINE_MS = 10000;
const OUTPUT_STATUSES = ['online','stale','offline','terminated'];
const MAX_UART_LINES = 100, MAX_UART_LINE_BYTES = 2048, MAX_UART_TEXT_BYTES = 24000;
const MISSIONS = [
  {id:0,name:'Orchard Perimeter',description:'Defend the orchard approaches and track perimeter incursions.'},
  {id:1,name:'Evacuation Corridor',description:'Protect friendly evacuees while defending the escape route.'},
  {id:2,name:'Greenhouse Shelter',description:'Monitor shelter conditions and defend the greenhouse perimeter.'},
];
const SELECT_STATE = 'SELECT * FROM garden_guard_state WHERE drone_id = ?';
const INSERT_STATE = `INSERT INTO garden_guard_state
  (drone_id,revision,instance_id,synced_at,state_json) VALUES (?,1,?,?,?)
  ON CONFLICT(drone_id) DO NOTHING`;
const UPDATE_STATE = `UPDATE garden_guard_state
  SET revision=revision+1,synced_at=?,state_json=? WHERE drone_id=? AND revision=?`;
class HttpError extends Error { constructor(status,message){super(message);this.status=status;} }
const finite = value => typeof value==='number' && Number.isFinite(value);
const object = value => value!==null && typeof value==='object' && !Array.isArray(value);
const requireValue = (condition,field) => {if(!condition)throw new HttpError(400,`Invalid ${field}.`);};
function integer(value,field,low=0,high=0xffffffff){requireValue(Number.isSafeInteger(value)&&value>=low&&value<=high,field);return value;}
function choice(value,field,values){requireValue(typeof value==='string'&&values.includes(value),field);return value;}
function boolean(value,field){requireValue(typeof value==='boolean',field);return value;}
function timestamp(value,field,now){requireValue(finite(value)&&value>=0&&value<=now+300,field);return value;}
function quantity(value,field,low,high){if(value===null)return null;requireValue(finite(value)&&value>=low&&value<=high,field);return value;}
function identifier(value,field){requireValue(typeof value==='string'&&/^[A-Za-z0-9_-]{8,128}$/.test(value),field);return value;}

function emptyOutputs(){return{oled:null,uart:{lines:[],received_at:null,status:'offline'}};}
function validateOutputs(value,now){
  if(value===undefined)return emptyOutputs();
  requireValue(object(value),'outputs');
  let oled=null;
  if(value.oled!==null){
    const frame=value.oled;
    requireValue(object(frame)&&frame.width===128&&frame.height===64&&frame.format==='ssd1306-pages','OLED frame format');
    requireValue(typeof frame.data==='string'&&/^[A-Za-z0-9+/]{1366}==$/.test(frame.data),'OLED frame encoding');
    /* Exact, canonical base64 for the captured 1024-byte controller image. */
    let decoded;
    try{decoded=atob(frame.data);}catch(_){throw new HttpError(400,'Invalid OLED frame encoding.');}
    requireValue(decoded.length===1024&&btoa(decoded)===frame.data,'OLED frame bytes');
    const sequence=integer(frame.sequence,'OLED frame sequence',1);
    requireValue(sequence%2===0,'completed OLED frame sequence');
    oled={width:128,height:64,format:'ssd1306-pages',data:frame.data,
      received_at:timestamp(frame.received_at,'OLED receipt time',now),sequence,
      status:choice(frame.status,'OLED status',OUTPUT_STATUSES)};
  }
  const uart=value.uart;
  requireValue(object(uart)&&Array.isArray(uart.lines)&&uart.lines.length<=MAX_UART_LINES,'UART lines');
  const encoder=new TextEncoder();let textBytes=0,previousId=-1;
  const lines=uart.lines.map(line=>{
    requireValue(object(line)&&typeof line.text==='string','UART line');
    const bytes=encoder.encode(line.text).byteLength;textBytes+=bytes;
    requireValue(bytes<=MAX_UART_LINE_BYTES&&textBytes<=MAX_UART_TEXT_BYTES,'UART text limit');
    const id=integer(line.id,'UART line id',0,Number.MAX_SAFE_INTEGER);
    requireValue(id>previousId,'ordered UART line ids');previousId=id;
    return{id,at:timestamp(line.at,'UART line time',now),text:line.text};
  });
  const receivedAt=uart.received_at===null?null:timestamp(uart.received_at,'UART receipt time',now);
  const status=choice(uart.status,'UART status',OUTPUT_STATUSES);
  requireValue(receivedAt!==null||(lines.length===0&&['offline','terminated'].includes(status)),'UART capture time');
  return{oled,uart:{lines,received_at:receivedAt,status}};
}
function terminated(snapshot){
  return snapshot.link.status==='terminated'||snapshot.telemetry?.mode==='terminated'||
    snapshot.outputs?.oled?.status==='terminated'||snapshot.outputs?.uart?.status==='terminated';
}
function newestOutputTime(snapshot){
  return Math.max(snapshot.outputs?.oled?.received_at??-1,snapshot.outputs?.uart?.received_at??-1);
}

function validateTelemetry(value,now){
  if(value===null)return null;
  requireValue(object(value)&&value.type==='snapshot'&&value.id==='GG01','telemetry');
  const result={type:'snapshot',id:'GG01',
    seq:integer(value.seq,'telemetry sequence'),tick:integer(value.tick,'telemetry tick'),
    mode:choice(value.mode,'mode',['standby','battle','terminated']),attacked:boolean(value.attacked,'attack flag'),
    round:choice(value.round,'round',['active','won','lost']),target:choice(value.target,'target',['none','human','zombie']),
    energy:integer(value.energy,'energy',0,10),lives:integer(value.lives,'lives',0,3),
    hits:integer(value.hits,'hits',0,255),score:integer(value.score,'score',-32768,32767),
    rescue_ms:integer(value.rescue_ms,'rescue window',0,10000),target_ms:integer(value.target_ms,'target window',0,65535),
    valid:integer(value.valid,'valid sensors',0,63),abnormal:integer(value.abnormal,'sensor warnings',0,63),
    unavailable:integer(value.unavailable,'unavailable sensors',0,63),ir:boolean(value.ir,'IR flag'),
    mission:integer(value.mission,'mission',0,2),nfc:boolean(value.nfc,'NFC flag'),nfc_taps:integer(value.nfc_taps,'NFC taps'),
    wifi:boolean(value.wifi,'WiFi flag'),wifi_drop:integer(value.wifi_drop,'WiFi drops'),
    received_at:timestamp(value.received_at,'board receipt time',now)};
  for(const[key,low,high]of[['t',-273.15,1000],['p',0,2000],['h',0,100]])result[key]=quantity(value[key],key,low,high);
  for(const[key,bound]of[['a',1000],['g',10000],['m',100000]]){
    requireValue(Array.isArray(value[key])&&value[key].length===3,`${key} axes`);
    result[key]=value[key].map(component=>quantity(component,key,-bound,bound));
  }
  return result;
}
function validateSnapshot(value,now){
  requireValue(object(value)&&object(value.link),'snapshot');
  const result={server_instance:identifier(value.server_instance,'local server instance'),
    revision:integer(value.revision,'local revision',0,Number.MAX_SAFE_INTEGER),
    generated_at:timestamp(value.generated_at,'local generation time',now),telemetry:validateTelemetry(value.telemetry,now),
    outputs:validateOutputs(value.outputs,now)};
  const link=value.link;
  result.link={source:choice(link.source,'source',['none','wifi','uart']),
    status:choice(link.status,'link status',['online','stale','offline','terminated']),
    wifi:boolean(link.wifi,'WiFi link'),uart:boolean(link.uart,'UART link'),
    last_seen:link.last_seen===null?null:timestamp(link.last_seen,'last board time',now),
    age_ms:link.age_ms===null?null:integer(link.age_ms,'board age',0,Number.MAX_SAFE_INTEGER)};
  if(result.telemetry===null)requireValue(link.source==='none'&&['offline','terminated'].includes(link.status)&&link.age_ms===null,'empty board state');
  else requireValue(link.source!=='none'&&result.link.age_ms!==null&&result.link.last_seen!==null,'board link');
  requireValue(Array.isArray(value.events)&&value.events.length<=200,'events');
  result.events=value.events.map(event=>{
    requireValue(object(event),'event');
    requireValue(typeof event.kind==='string'&&/^[a-z][a-z0-9_]{0,63}$/.test(event.kind),'event kind');
    requireValue(typeof event.message==='string'&&event.message.length<=512,'event message');
    return{id:integer(event.id,'event id',0,Number.MAX_SAFE_INTEGER),at:timestamp(event.at,'event time',now),
      kind:event.kind,message:event.message,level:choice(event.level,'event level',['info','warning','critical']),
      source:choice(event.source,'event source',['wifi','uart'])};
  });
  return result;
}
function validateEnvelope(value,now){
  requireValue(object(value)&&value.version===1,'relay protocol version');
  requireValue(Array.isArray(value.acknowledgements)&&value.acknowledgements.length===0,'acknowledgements (read-only deployment)');
  return{relay_id:identifier(value.relay_id,'relay id'),snapshot:validateSnapshot(value.snapshot,now)};
}
function json(value,status=200){return new Response(JSON.stringify(value),{status,headers:{
  'content-type':'application/json; charset=utf-8','cache-control':'no-store','x-content-type-options':'nosniff'}});}
async function readBody(request){
  if(request.headers.get('content-type')?.split(';')[0].trim()!=='application/json')throw new HttpError(415,'Use application/json.');
  const declared=request.headers.get('content-length');
  if(declared!==null&&(!/^\d+$/.test(declared)||Number(declared)>MAX_BODY_BYTES))throw new HttpError(413,'Relay payload exceeds 64 KiB.');
  if(!request.body)throw new HttpError(400,'A relay payload is required.');
  const reader=request.body.getReader(),chunks=[];let length=0;
  while(true){const{value,done}=await reader.read();if(done)break;length+=value.byteLength;
    if(length>MAX_BODY_BYTES){await reader.cancel();throw new HttpError(413,'Relay payload exceeds 64 KiB.');}chunks.push(value);}
  const bytes=new Uint8Array(length);let offset=0;for(const chunk of chunks){bytes.set(chunk,offset);offset+=chunk.byteLength;}
  try{return JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(bytes));}
  catch(_){throw new HttpError(400,'Malformed relay JSON.');}
}
async function authorize(request,env){
  if(typeof env.RELAY_TOKEN!=='string'||env.RELAY_TOKEN.length<24)throw new HttpError(503,'Relay authentication is not configured.');
  const header=request.headers.get('authorization')||'';
  if(!header.startsWith('Bearer ')||header.length>1024)throw new HttpError(401,'A valid relay credential is required.');
  const encoder=new TextEncoder();
  const[actual,expected]=await Promise.all([
    crypto.subtle.digest('SHA-256',encoder.encode(header.slice(7))),crypto.subtle.digest('SHA-256',encoder.encode(env.RELAY_TOKEN))]);
  const left=new Uint8Array(actual),right=new Uint8Array(expected);let difference=0;
  for(let i=0;i<left.length;++i)difference|=left[i]^right[i];
  if(difference!==0)throw new HttpError(401,'A valid relay credential is required.');
}
async function readState(env){
  if(!env.DB?.prepare)throw new HttpError(503,'Cloud storage is unavailable.');
  return env.DB.prepare(SELECT_STATE).bind('GG01').first();
}
function nextStoredState(previous,envelope){
  const next=envelope.snapshot,retiredRelays=previous?.retired_relays||[],retiredServers=previous?.retired_servers||[];
  if(retiredRelays.includes(envelope.relay_id)||retiredServers.includes(next.server_instance))throw new HttpError(409,'This relay session has been superseded.');
  if(previous){const old=previous.snapshot;
    if(next.server_instance===old.server_instance){
      if(next.revision<old.revision||(next.revision===old.revision&&next.generated_at<old.generated_at))throw new HttpError(409,'A newer local snapshot is already stored.');
      if(next.revision===old.revision&&next.generated_at===old.generated_at)return null;
    }else if(next.generated_at<old.generated_at)throw new HttpError(409,'An older local session cannot replace the current one.');
    const wasTerminated=terminated(old),isTerminated=terminated(next);
    const oldEventId=Math.max(-1,...old.events.map(event=>event.id));
    const resetSession=next.server_instance!==old.server_instance||next.events.some(event=>event.kind==='reset'&&event.id>oldEventId);
    const telemetryReset=resetSession&&next.telemetry?.mode==='standby'&&next.link.status==='online'&&next.link.source==='wifi'&&next.telemetry.wifi&&next.telemetry.received_at>(old.telemetry?.received_at??-1);
    /* The raw-output collector has no game telemetry. An explicit new capture
     * session must observe newer non-terminal output before clearing the end. */
    const freshOutput=[next.outputs?.oled,next.outputs?.uart].some(output=>
      output?.status==='online'&&output.received_at!==null&&
      output.received_at>newestOutputTime(old)&&
      output.received_at>=next.generated_at-STALE_MS/1000);
    const outputReset=next.telemetry===null&&next.server_instance!==old.server_instance&&freshOutput;
    const reset=telemetryReset||outputReset;
    if(wasTerminated&&!isTerminated&&!reset)throw new HttpError(409,'A terminated board must reset to standby before a new session.');
    if(envelope.relay_id!==previous.relay_id)retiredRelays.push(previous.relay_id);
    if(next.server_instance!==old.server_instance)retiredServers.push(old.server_instance);
  }
  return{relay_id:envelope.relay_id,snapshot:next,retired_relays:retiredRelays.slice(-32),retired_servers:retiredServers.slice(-32)};
}
async function sync(envelope,env,now){
  /* Optimistic compare-and-swap also orders concurrent uploads across isolates. */
  for(let attempt=0;attempt<4;++attempt){
    const row=await readState(env),previous=row?JSON.parse(row.state_json):null,next=nextStoredState(previous,envelope);
    if(!next)return{ok:true,accepted:false,commands:[]};
    const payload=JSON.stringify(next),operation=row?
      env.DB.prepare(UPDATE_STATE).bind(now,payload,'GG01',row.revision):
      env.DB.prepare(INSERT_STATE).bind('GG01',crypto.randomUUID(),now,payload);
    const result=await operation.run();if(result.meta?.changes===1)return{ok:true,accepted:true,commands:[]};
  }
  throw new HttpError(409,'Another relay update arrived first. Retry with a current snapshot.');
}
function publicOutputs(snapshot,bridgeAge,now){
  const outputs=snapshot?.outputs||emptyOutputs(),ended=snapshot?terminated(snapshot):false;
  function status(output){
    if(output.status==='terminated')return 'terminated';
    if(ended&&output.received_at!==null)return 'terminated';
    if(output.status==='offline'||output.received_at===null)return 'offline';
    const age=Math.max(0,(now-output.received_at)*1000);
    if(age>OFFLINE_MS||bridgeAge===null||bridgeAge>OFFLINE_MS)return 'offline';
    return output.status==='stale'||age>STALE_MS||bridgeAge>STALE_MS?'stale':'online';
  }
  return{oled:outputs.oled?{...outputs.oled,status:status(outputs.oled)}:null,
    uart:{...outputs.uart,status:status(outputs.uart)}};
}
function publicState(row,origin,now){
  const stored=row?JSON.parse(row.state_json):null,snapshot=stored?.snapshot,telemetry=snapshot?.telemetry||null,original=snapshot?.link;
  const elapsed=snapshot?Math.max(0,(now-snapshot.generated_at)*1000):0;
  const age=original?.age_ms===null||!original?null:Math.max(0,Math.floor(original.age_ms+elapsed));
  const bridgeAge=row?Math.max(0,Math.floor((now-row.synced_at)*1000)):null;
  let status='offline';
  if(snapshot&&terminated(snapshot))status='terminated';
  else if(telemetry&&original.status!=='offline'&&age!==null&&age<=OFFLINE_MS&&bridgeAge<=OFFLINE_MS)
    status=original.status==='stale'||age>STALE_MS||bridgeAge>STALE_MS?'stale':'online';
  const source=telemetry?original.source:'none';
  return{revision:row?.revision||0,generated_at:now,server_instance:row?.instance_id||'garden-guard-cloud-empty',telemetry,
    outputs:publicOutputs(snapshot,bridgeAge,now),
    link:{source,status,wifi:status==='online'&&source==='wifi'&&original.wifi&&telemetry.wifi,
      uart:status==='online'&&source==='uart'&&original.uart,last_seen:telemetry?original.last_seen:null,age_ms:age},
    events:snapshot?.events||[],missions:MISSIONS,
    server:{url:origin,drone_id:'GG01',board_ingest_port:null,deployment:'cloud',mission_control:'local-only',
      relay_status:bridgeAge===null||bridgeAge>OFFLINE_MS?'offline':bridgeAge>STALE_MS?'stale':'online',relay_age_ms:bridgeAge},
    permissions:{mission_write:false}};
}
function asset(path){
  const name=['/','/drone/GG01','/drone/GG01/'].includes(path)?'index.html':path.slice(1);
  if(!Object.hasOwn(ASSETS,name))return null;
  const types={'index.html':'text/html; charset=utf-8','app.css':'text/css; charset=utf-8','app.js':'text/javascript; charset=utf-8'};
  return new Response(ASSETS[name],{headers:{'content-type':types[name],'cache-control':'no-store','x-content-type-options':'nosniff',
    'content-security-policy':"default-src 'self'; img-src 'self' data:; script-src 'self'; style-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'"}});
}
export function createWorker({now=()=>Date.now()/1000}={}){return{async fetch(request,env){
  const url=new URL(request.url);
  try{
    if(request.method==='GET'&&url.pathname==='/api/state')return json(publicState(await readState(env),url.origin,now()));
    if(request.method==='POST'&&url.pathname==='/api/relay/sync'){
      await authorize(request,env);const timestamp=now(),envelope=validateEnvelope(await readBody(request),timestamp);
      return json(await sync(envelope,env,timestamp),202);
    }
    if(url.pathname==='/api/mission')return json({error:'The cloud dashboard is read-only. Assign missions from the local board console.'},403);
    if(request.method==='GET'){const response=asset(url.pathname);if(response)return response;}
    return json({error:'Endpoint not found.'},404);
  }catch(error){return json({error:error instanceof HttpError?error.message:'Cloud storage is unavailable. Try again shortly.'},error instanceof HttpError?error.status:503);}
}};}
export default createWorker();
