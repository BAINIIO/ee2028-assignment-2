(() => {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const definitions = [
    { key:'t', name:'Temperature', short:'Temperature', unit:'°C', bit:1, min:10, max:45, threshold:'NORMAL 10–45 °C', digits:2 },
    { key:'p', name:'Air pressure', short:'Pressure', unit:'hPa', bit:2, min:850, max:1100, threshold:'NORMAL 850–1100 hPa', digits:2 },
    { key:'h', name:'Humidity', short:'Humidity', unit:'%RH', bit:4, min:20, max:85, threshold:'NORMAL 20–85 %RH', digits:2 },
    { key:'a', name:'Acceleration', short:'Acceleration', unit:'m/s²', bit:8, min:0, max:30, threshold:'MAGNITUDE ≤ 30 m/s²', vector:true, digits:2 },
    { key:'g', name:'Angular rate', short:'Gyroscope', unit:'°/s', bit:16, min:0, max:250, threshold:'MAGNITUDE ≤ 250 °/s', vector:true, digits:2 },
    { key:'m', name:'Magnetic field', short:'Magnetometer', unit:'µT', bit:32, min:0, max:200, threshold:'MAGNITUDE ≤ 200 µT', vector:true, digits:2 }
  ];
  const defaultMissions = [
    { id:0, name:'Orchard Perimeter', description:'Watch the outer garden and hold the perimeter.' },
    { id:1, name:'Evacuation Corridor', description:'Protect the route. Let friendly survivors pass.' },
    { id:2, name:'Greenhouse Shelter', description:'Keep the last growing space under watch.' }
  ];
  const icons = {
    zombie:'<svg viewBox="0 0 48 48" aria-hidden="true" shape-rendering="crispEdges"><path d="M13 4h25v19H13zM7 11h6v10H7zm10 16h16v15H17zM7 29h10v5H7zm25-3h9v5h-9zM16 41h6v7h-6zm12 0h6v7h-6z"/><path d="M18 10h6v5h-6zm12 0h5v5h-5zm-7 10h10v3H23z" fill="#122019"/></svg>',
    human:'<svg viewBox="0 0 48 48" aria-hidden="true" shape-rendering="crispEdges"><path d="M14 2h20v17H14zm4 21h12v17H18zm-9 2h8v13H9zm22 0h8v13h-8zM17 40h6v8h-6zm10 0h6v8h-6z"/><path d="M18 7h4v4h-4zm9 0h4v4h-4zm-7 8h9v2h-9z" fill="#122019"/></svg>',
    none:'<svg viewBox="0 0 48 48" aria-hidden="true"><circle cx="24" cy="24" r="17" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M24 3v11m0 20v11M3 24h11m20 0h11" fill="none" stroke="currentColor" stroke-width="1.5"/></svg>'
  };
  let state = null, serverReachable = false, lastApiAt = 0, selectedMission = null;
  let missionRequest = null, missionSending = false, selectedHistory = 't', historySeconds = 60;
  let history = [], historyKey = null, warningFilter = false, pollTimer = null, fetching = false;
  let snapshotRevision = 0, renderedLinkStatus = null;
  let snapshotOrder = null;
  const retiredServerInstances = new Set();

  const finite = (value) => typeof value === 'number' && Number.isFinite(value);
  const number = (value, digits=0) => finite(value) ? value.toLocaleString(undefined,{minimumFractionDigits:digits,maximumFractionDigits:digits}) : '—';
  const clamp = (value, min, max) => Math.max(min, Math.min(max, value));
  const text = (id, value) => { $(id).textContent = value; };
  const telemetry = () => state && state.telemetry && typeof state.telemetry === 'object' ? state.telemetry : null;
  const live = () => serverReachable && effectiveLinkStatus() === 'online';
  const bit = (field, mask) => finite(field) && (field & mask) !== 0;
  const clock = (epoch) => finite(epoch) ? new Date(epoch*1000).toLocaleTimeString(undefined,{hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false}) : '—';
  const scalar = (t, def) => {
    if (!t || !bit(t.valid,def.bit) || bit(t.unavailable,def.bit)) return null;
    const value = t[def.key];
    return def.vector ? (Array.isArray(value) && value.length===3 && value.every(finite) ? Math.hypot(...value) : null) : (finite(value) ? value : null);
  };
  const missionName = (id) => (state?.missions || defaultMissions).find((m) => m.id===id)?.name || '—';
  const canSetMission = () => false; // Public cloud view never issues board commands.

  function setupSensors() {
    definitions.forEach((def) => {
      const card=document.createElement('article'); card.className='sensor-card unavailable'; card.id=`sensor-${def.key}`; card.dataset.vector=!!def.vector;
      const head=document.createElement('div'); head.className='sensor-head';
      const title=document.createElement('h3'); title.textContent=def.name;
      const tag=document.createElement('span'); tag.className='sensor-tag'; tag.id=`tag-${def.key}`; tag.textContent='NO DATA'; head.append(title,tag);
      const value=document.createElement('div'); value.className='sensor-number';
      const reading=document.createElement('b'); reading.id=`value-${def.key}`; reading.textContent='—';
      const unit=document.createElement('span'); unit.textContent=def.unit; value.append(reading,unit);
      const threshold=document.createElement('p'); threshold.className='sensor-threshold'; threshold.textContent=def.threshold;
      const axes=document.createElement('div'); axes.className='sensor-axes';
      if(def.vector) ['X','Y','Z'].forEach((label,i)=> { const axis=document.createElement('span'); axis.append(document.createTextNode(`${label} `)); const v=document.createElement('strong'); v.id=`axis-${def.key}-${i}`; v.textContent='—';axis.append(v);axes.append(axis); });
      card.append(head,value,threshold,axes); $('sensor-grid').append(card);
      const tab=document.createElement('button');tab.className='history-tab';tab.type='button';tab.setAttribute('role','tab');tab.setAttribute('aria-selected',String(def.key===selectedHistory));tab.dataset.key=def.key;tab.textContent=def.short;tab.addEventListener('click',()=>{selectedHistory=def.key;renderHistory();});$('history-tabs').append(tab);
    });
  }

  function ageMs() {
    if(!state?.link) return null;
    if(finite(state.link.age_ms)) return Math.max(0,state.link.age_ms+Date.now()-lastApiAt);
    return finite(state.link.last_seen) ? Math.max(0,Date.now()-state.link.last_seen*1000) : null;
  }
  function effectiveLinkStatus() {
    const status=state?.link?.status || 'offline';
    if(status==='terminated' || telemetry()?.mode==='terminated')return 'terminated';
    if(!serverReachable)return 'offline';
    if(status==='online' || status==='stale'){
      const age=ageMs();
      if(age===null || age>10000)return 'offline';
      if(age>3000)return 'stale';
    }
    return status;
  }
  function updateConnection() {
    const link=state?.link || {}, t=telemetry(), status=effectiveLinkStatus();
    let kind='offline',badge='NO BOARD LINK',title='Waiting for GG01',description='The board has not sent a telemetry packet yet.';
    if(status==='terminated') {kind='ended';badge='SESSION ENDED';title='Drone communications ended';description='The board terminated. Reset it to start a new session.';}
    else if(!serverReachable && lastApiAt) {badge='SERVER OFFLINE';title='Command centre unavailable';description='Cannot reach the hosted server. The last board snapshot is retained.';}
    else if(status==='online' && link.source==='wifi') {kind='wifi';badge='BOARD WI-FI / RELAY';title='Board Wi-Fi connected';description='Board Wi-Fi readings are forwarded through the authenticated relay.';}
    else if(status==='online' && link.source==='uart') {kind='uart';badge='UART FALLBACK';title='USB UART fallback active';description='The relay is forwarding USB serial. Board Wi-Fi is not confirmed.';}
    else if(status==='stale') {kind='stale';badge='STALE DATA';title='Waiting for a fresh board packet';description='The last readings are retained. Check the board connection.';}
    else if(status==='offline' && t) {title='Board link offline';description='Last known state shown. Live readings are unavailable.';}
    const badgeNode=$('source-badge');badgeNode.className=`status-pill ${kind}`;badgeNode.replaceChildren();const dot=document.createElement('i');badgeNode.append(dot,document.createTextNode(badge));
    $('connection-dot').className=`signal-dot ${kind}`;text('connection-title',title);text('connection-description',description);
    const age=ageMs();text('packet-age',age===null?'—':age<1000?'Just now':age<60000?`${(age/1000).toFixed(1)}s ago`:`${Math.floor(age/60000)}m ago`);
    text('transport-detail',link.source==='wifi'?'Board Wi-Fi':link.source==='uart'?'USB serial':'No source');
    document.body.classList.toggle('server-unavailable',!serverReachable);
    text('server-status',serverReachable?'HOSTED COMMAND CENTRE / CONNECTED':'HOSTED COMMAND CENTRE / UNREACHABLE');
  }

  function renderDrone() {
    const t=telemetry(); const mode=state?.link?.status==='terminated'?'terminated':t?.mode;
    text('unit-id',state?.server?.drone_id || 'GG01');
    const modeLabels={standby:'STANDBY',battle:'BATTLE',terminated:'TERMINATED'};
    text('mode-badge',t?.attacked && mode==='battle'?'ATTACKED':modeLabels[mode] || 'AWAITING SIGNAL');
    const phases=mode==='terminated'?['A final signal.','The board ended this session. Reset it to return to standby.']:!t?['Awaiting orders.','Connect the board to bring this console online.']:t.attacked?['The drone needs you.','Get to the physical USER button. Double press to rescue.']:mode==='standby'?['Holding the perimeter.','Double press USER on the board to begin the patrol.']:t.round==='won'?['The garden holds.','Six zombies stopped. Return to standby for a new round.']:t.round==='lost'?['The garden was overrun.','The drone is still in battle mode. Double press for standby.']:['Patrol in progress.','Charge for zombies. Let friendly humans pass.'];
    text('drone-title',phases[0]);text('drone-description',phases[1]);text('current-mission',t?missionName(t.mission):'—');
    text('energy-value',number(t?.energy));text('score-value',number(t?.score));text('hits-value',number(t?.hits));
    const energy=finite(t?.energy)?clamp(t.energy,0,10):0;$('energy-fill').style.width=`${energy*10}%`;
    if(finite(t?.energy)) $('energy-meter').setAttribute('aria-valuenow',String(t.energy)); else $('energy-meter').removeAttribute('aria-valuenow');
    const lives=$('lives-value');lives.replaceChildren();
    if(finite(t?.lives)){for(let i=0;i<3;i++){const heart=document.createElement('span');heart.textContent=i<t.lives?'♥':'♡';if(i>=t.lives)heart.className='empty';lives.append(heart);}lives.setAttribute('aria-label',`${t.lives} of 3 lives`);}else{lives.textContent='—';lives.removeAttribute('aria-label');}
    const target=t?.attacked || mode!=='battle' || t?.round!=='active'?'none':t.target;
    const targetPanel=$('target-heading').closest('.target-panel');targetPanel.classList.remove('human','zombie');if(target==='human'||target==='zombie')targetPanel.classList.add(target);
    $('target-icon').innerHTML=icons[target] || icons.none;
    text('target-name',target==='zombie'?'Zombie approaching':target==='human'?'Friendly human':mode==='terminated'?'Session ended':t?'No active target':'No target data');
    text('target-instruction',target==='zombie'?'Single press USER to charge and auto fire.':target==='human'?'Hold fire. Give the survivor safe passage.':mode==='standby'?'Double press the board button to start.':t?.attacked?'Rescue takes priority over targets.':'Target identity comes from the STM32.');
    text('round-badge',t?({active:'ROUND ACTIVE',won:'VICTORY',lost:'ROUND LOST'}[t.round] || '—'):'—');
    text('ir-state',typeof t?.ir==='boolean'?(t.ir?'AIM LOCKED / +5':'Clear / no bonus'):'—');
    $('art-human').toggleAttribute('hidden',target!=='human');$('art-zombie').toggleAttribute('hidden',target==='human');
    renderCountdowns();
  }

  function renderCountdowns() {
    const t=telemetry(),elapsed=live()?(ageMs()||0):0,ended=state?.link?.status==='terminated'||t?.mode==='terminated';
    const attacked=!!t?.attacked && t.mode==='battle'&&!ended;$('attack-alert').hidden=!attacked;
    if(attacked){const remaining=finite(t.rescue_ms)?Math.max(0,t.rescue_ms-elapsed):null;text('rescue-clock',remaining===null?'—':`${(remaining/1000).toFixed(1)}s`);text('rescue-caption',!live()?'LAST REPORTED WINDOW':remaining===0?'AWAITING BOARD UPDATE':'RESCUE WINDOW');}
    const active=t?.mode==='battle' && t.round==='active' && !t.attacked && t.target!=='none'&&!ended;
    const remaining=active && finite(t.target_ms)?Math.max(0,t.target_ms-elapsed):null;
    text('target-time',remaining===null?'—':`${(remaining/1000).toFixed(1)}s`);$('target-progress').style.width=`${remaining===null?0:clamp(remaining/4000,0,1)*100}%`;
  }

  function renderSensors() {
    const t=telemetry();let available=0,warnings=0;
    definitions.forEach((def)=>{const value=scalar(t,def);const unavailable=value===null;const abnormal=!unavailable && bit(t?.abnormal,def.bit);if(!unavailable)available++;if(abnormal)warnings++;
      const card=$(`sensor-${def.key}`);card.classList.toggle('unavailable',unavailable);card.classList.toggle('warning',abnormal);card.classList.toggle('stale',!!t&&!live());
      text(`value-${def.key}`,number(value,def.digits));text(`tag-${def.key}`,unavailable?(t?'UNAVAILABLE':'NO DATA'):abnormal?'WARNING':live()?'NORMAL':'LAST READING');
      if(def.vector)['X','Y','Z'].forEach((_,i)=>text(`axis-${def.key}-${i}`,unavailable?'—':number(t[def.key][i],2)));
    });
    text('sensor-summary',!t?'Waiting for readings':warnings?`${warnings} warning${warnings===1?'':'s'} / ${available} of 6 available`:`${available} of 6 sensors available`);
  }

  function recordHistory() {
    const t=telemetry();if(!t || !live())return;
    const key=`${t.id}:${t.seq}:${t.tick}:${t.received_at}`;if(key===historyKey)return;historyKey=key;
    const at=finite(t.received_at)?t.received_at:Date.now()/1000;
    history.push({at,values:Object.fromEntries(definitions.map((def)=>[def.key,scalar(t,def)]))});
    const cutoff=Date.now()/1000-310;history=history.filter((row)=>row.at>=cutoff).slice(-600);
  }
  function renderHistory() {
    const def=definitions.find((d)=>d.key===selectedHistory);
    document.querySelectorAll('.history-tab').forEach((tab)=>{const selected=tab.dataset.key===selectedHistory;tab.setAttribute('aria-selected',String(selected));tab.tabIndex=selected?0:-1;});
    const now=Date.now()/1000,from=now-historySeconds;
    const samples=history.filter((row)=>row.at>=from), valid=samples.filter((row)=>finite(row.values[def.key]));
    const current=scalar(telemetry(),def);text('history-value',number(current,def.digits));text('history-unit',def.unit);
    $('history-chart').setAttribute('aria-label',`${def.name} history from actual board packets`);
    text('history-start',clock(from));text('history-end',live()?'NOW':'LAST KNOWN DATA');
    if(!valid.length){text('history-range','No samples yet');$('history-line').setAttribute('d','');$('history-area').setAttribute('d','');$('history-dot').setAttribute('hidden','');$('history-empty').hidden=false;return;}
    const values=valid.map((row)=>row.values[def.key]),min=Math.min(...values),max=Math.max(...values);
    text('history-range',`MIN ${number(min,2)} / MAX ${number(max,2)} ${def.unit}`);
    $('history-empty').hidden=true;
    const padding=Math.max((max-min)*.18,Math.abs(max)*.002,.1),low=min-padding,high=max+padding;
    const point=(row)=>[clamp((row.at-from)/historySeconds,0,1)*800,130-(row.values[def.key]-low)/(high-low)*110];
    let path='',drawing=false; samples.forEach((row)=>{if(!finite(row.values[def.key])){drawing=false;return;}const [x,y]=point(row);path+=`${drawing?'L':'M'}${x.toFixed(2)},${y.toFixed(2)} `;drawing=true;});
    $('history-line').setAttribute('d',path);
    const first=point(valid[0]),last=point(valid[valid.length-1]);
    $('history-area').setAttribute('d',valid.length>1 && valid.length===samples.length?`${path}L${last[0].toFixed(2)},145 L${first[0].toFixed(2)},145 Z`:'');
    $('history-dot').removeAttribute('hidden');$('history-dot').setAttribute('cx',last[0]);$('history-dot').setAttribute('cy',last[1]);
  }

  function renderMissions() {
    const missions=Array.isArray(state?.missions)&&state.missions.length?state.missions:defaultMissions,t=telemetry();
    selectedMission=Number.isInteger(t?.mission)?t.mission:null;
    const container=$('mission-options');container.replaceChildren();
    missions.forEach((mission)=>{
      const button=document.createElement('button');button.type='button';button.className=`mission-option${selectedMission===mission.id?' selected':''}`;button.disabled=true;
      button.setAttribute('aria-pressed',String(selectedMission===mission.id));
      const index=document.createElement('span');index.textContent=`0${mission.id+1}`;
      const content=document.createElement('span'),name=document.createElement('strong'),description=document.createElement('p');
      name.textContent=mission.name;description.textContent=mission.description;content.append(name,description);button.append(index,content);
      if(t?.mission===mission.id){const label=document.createElement('span');label.className='mission-current';label.textContent='ON BOARD';button.append(label);}
      container.append(button);
    });
    text('mission-requirement','Cloud view is read-only. Assign missions from the local board console.');
    $('apply-mission').disabled=true;text('apply-mission','Local console only');
  }
  function feedback(message,error=false){text('mission-feedback',message);$('mission-feedback').classList.toggle('error',error);}
  function applyMission(){feedback('Assign missions from the local board console.');}
  function renderNfc() {
    const t=telemetry(),ended=effectiveLinkStatus()==='terminated';text('nfc-status',ended?'NFC / SESSION ENDED':t&&!live()?'NFC / LAST READING':typeof t?.nfc==='boolean'?(t.nfc?'NFC TAG ONLINE':'NFC NOT AVAILABLE'):'NFC —');$('nfc-status').classList.toggle('good',t?.nfc===true&&live());
    text('nfc-taps',number(t?.nfc_taps));
    if(state?.server?.url){try{const url=new URL(`/drone/${encodeURIComponent(state.server.drone_id||'GG01')}`,state.server.url);$('phone-url').value=url.href;$('copy-url').disabled=false;}catch(_){$('phone-url').value='Phone URL unavailable';$('copy-url').disabled=true;}}
  }
  function renderEvents() {
    const events=Array.isArray(state?.events)?state.events:[];const filtered=events.filter((e)=>!warningFilter||e.level==='warning'||e.level==='critical').slice().sort((a,b)=>(b.at||0)-(a.at||0)||(b.id||0)-(a.id||0)).slice(0,16);
    $('export-events').disabled=!events.length;const list=$('event-list');list.replaceChildren();
    if(!filtered.length){const item=document.createElement('li');item.className='events-empty';item.textContent=warningFilter?'No warning events in this session.':'Board events will appear here as they arrive.';list.append(item);return;}
    filtered.forEach((event)=>{const item=document.createElement('li');item.className=`event-item ${['warning','critical'].includes(event.level)?event.level:'info'}`;const time=document.createElement('time');time.textContent=clock(event.at);if(finite(event.at))time.dateTime=new Date(event.at*1000).toISOString();const dot=document.createElement('span');dot.className='event-level';const message=document.createElement('p');message.textContent=event.message||event.kind||'Board event';const source=document.createElement('span');source.textContent=event.source||'board';item.append(time,dot,message,source);list.append(item);});
  }
  function acceptSnapshot(next) {
    if(!next||typeof next!=='object'||!next.link)return false;
    const instance=next.server_instance,revision=next.revision,generatedAt=next.generated_at;
    if(typeof instance!=='string'||!instance||!Number.isSafeInteger(revision)||revision<0||!finite(generatedAt))return false;
    if(retiredServerInstances.has(instance))return false;
    if(snapshotOrder){
      if(instance===snapshotOrder.instance){
        if(revision<snapshotOrder.revision || (revision===snapshotOrder.revision&&generatedAt<=snapshotOrder.generatedAt))return false;
      }else retiredServerInstances.add(snapshotOrder.instance);
    }
    snapshotOrder={instance,revision,generatedAt};
    state=next;serverReachable=true;lastApiAt=Date.now();++snapshotRevision;recordHistory();
    const t=telemetry();
    if(missionRequest && live() && state.link.source==='wifi' && t?.wifi===true && t?.mission===missionRequest.id && t?.id===missionRequest.baseline.id && finite(t.received_at) && finite(missionRequest.baseline.receivedAt) && t.received_at>missionRequest.baseline.receivedAt){selectedMission=missionRequest.id;missionRequest=null;feedback('Mission confirmed by GG01.');}
    updateConnection();renderDrone();renderSensors();renderHistory();renderMissions();renderNfc();renderEvents();
    renderedLinkStatus=effectiveLinkStatus();
    return true;
  }
  async function fetchState() {
    if(fetching)return;fetching=true;$('refresh-button').disabled=true;
    const expectedRevision=snapshotRevision,controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),4000);
    try{const response=await fetch('/api/state',{cache:'no-store',signal:controller.signal});if(!response.ok)throw new Error(`Server response ${response.status}`);acceptSnapshot(await response.json());}
    catch(_){if(expectedRevision===snapshotRevision){serverReachable=false;updateConnection();renderSensors();renderMissions();}}
    finally{clearTimeout(timeout);fetching=false;$('refresh-button').disabled=false;}
  }
  function startPolling(){if(!pollTimer){fetchState();pollTimer=setInterval(fetchState,1000);}}
  function connectStream(){startPolling();}
  async function copyUrl(){const input=$('phone-url');let copied=false;try{if(navigator.clipboard?.writeText){await navigator.clipboard.writeText(input.value);copied=true;}}catch(_){}if(!copied){input.focus();input.select();input.setSelectionRange(0,input.value.length);try{copied=document.execCommand('copy');}catch(_){}}
    text('copy-feedback',copied?'Phone link copied. Open the hosted dashboard on your phone.':'Link selected. Copy it and open the hosted dashboard on your phone.');}
  function exportEvents(){const events=Array.isArray(state?.events)?state.events:[];if(!events.length)return;const escape=(v)=>`"${String(v??'').replaceAll('"','""')}"`;const rows=[['time','kind','level','source','message'],...events.map((e)=>[finite(e.at)?new Date(e.at*1000).toISOString():'',e.kind,e.level,e.source,e.message])];const blob=new Blob([rows.map((row)=>row.map(escape).join(',')).join('\r\n')],{type:'text/csv;charset=utf-8'});const url=URL.createObjectURL(blob);const a=document.createElement('a');a.href=url;a.download=`garden-guard-${state?.server?.drone_id||'GG01'}-events.csv`;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}

  setupSensors();renderMissions();renderHistory();
  $('history-chart').setAttribute('preserveAspectRatio','none');
  $('history-tabs').addEventListener('keydown',(event)=>{if(!['ArrowLeft','ArrowRight','Home','End'].includes(event.key))return;event.preventDefault();const tabs=Array.from(document.querySelectorAll('.history-tab'));let index=tabs.findIndex((tab)=>tab.dataset.key===selectedHistory);index=event.key==='Home'?0:event.key==='End'?tabs.length-1:(index+(event.key==='ArrowRight'?1:-1)+tabs.length)%tabs.length;tabs[index].click();tabs[index].focus();});
  text('today-label',new Date().toLocaleDateString(undefined,{day:'2-digit',month:'short',year:'numeric'}).toUpperCase());
  $('refresh-button').addEventListener('click',fetchState);$('apply-mission').addEventListener('click',applyMission);$('copy-url').addEventListener('click',copyUrl);$('export-events').addEventListener('click',exportEvents);
  $('filter-events').addEventListener('click',()=>{warningFilter=!warningFilter;$('filter-events').setAttribute('aria-pressed',String(warningFilter));renderEvents();});
  $('history-window').addEventListener('change',(event)=>{historySeconds=Number(event.target.value);renderHistory();});
  setInterval(()=>{updateConnection();renderCountdowns();const status=effectiveLinkStatus();if(status!==renderedLinkStatus){renderSensors();renderHistory();renderMissions();renderNfc();renderedLinkStatus=status;}if(lastApiAt&&Date.now()-lastApiAt>2500&&status!=='terminated')startPolling();if(missionRequest&&Date.now()-missionRequest.at>10000){missionRequest=null;feedback('No board confirmation yet. Check the link before retrying.',true);renderMissions();}},250);
  fetchState();connectStream();
})();
