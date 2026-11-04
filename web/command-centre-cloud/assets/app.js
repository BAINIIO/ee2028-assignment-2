/* SSD1306 page layout: byte=(y/8)*128+x, bit=y%8. */
function decodeOledFrame(frame) {
  if(!frame || frame.width!==128 || frame.height!==64 || frame.format!=='ssd1306-pages' ||
     typeof frame.data!=='string' || frame.data.length!==1368)throw new Error('Invalid OLED frame.');
  let bytes;
  try{bytes=atob(frame.data);}catch(_){throw new Error('Invalid OLED frame.');}
  if(bytes.length!==1024 || btoa(bytes)!==frame.data)throw new Error('Invalid OLED frame.');
  const pixels=new Uint8ClampedArray(128*64*4);
  for(let y=0;y<64;++y)for(let x=0;x<128;++x){
    const lit=(bytes.charCodeAt((y>>3)*128+x)&(1<<(y&7)))!==0,index=(y*128+x)*4;
    pixels[index]=lit?184:0;pixels[index+1]=lit?239:0;pixels[index+2]=lit?160:0;pixels[index+3]=255;
  }
  return pixels;
}

function rawUartText(lines) {
  return Array.isArray(lines)?lines.slice(-100).filter(line=>line&&typeof line.text==='string').map(line=>line.text).join('\n'):'';
}

(() => {
  'use strict';
  if(typeof document==='undefined')return;
  const $=id=>document.getElementById(id),canvas=$('oled-canvas'),context=canvas.getContext('2d',{alpha:false});
  const validTime=value=>typeof value==='number'&&Number.isFinite(value)&&value>=0;
  const states=['online','stale','offline','terminated'];
  let outputs=null,lastApiAt=0,generatedAt=null,reachable=false,fetching=false;
  let frameKey=null,frameValid=false,frameError=false,lastUartText=null,order=null;
  const retiredServers=new Set();

  function scaleCanvas(){
    const scale=Math.max(1,Math.floor(($('oled-display').clientWidth-2)/128));
    canvas.style.width=`${128*scale}px`;canvas.style.height=`${64*scale}px`;
  }
  function formatTime(value){return new Date(value*1000).toLocaleTimeString(undefined,{hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false});}
  function channelStatus(channel,hasData){
    if(channel?.status==='terminated')return 'terminated';
    if(!reachable || !hasData || !states.includes(channel?.status))return 'offline';
    let status=channel.status;
    if(status==='online'||status==='stale'){
      const elapsed=Date.now()-lastApiAt;
      const age=validTime(channel.received_at)&&validTime(generatedAt)?Math.max(0,(generatedAt-channel.received_at)*1000)+elapsed:elapsed;
      if(age>10000)status='offline';else if(age>3000)status='stale';
    }
    return status;
  }
  function statusText(id,channel,hasData,label){
    const status=channelStatus(channel,hasData),node=$(id);
    let text={online:'Live',stale:'Stale',offline:'Offline',terminated:'Ended'}[status];
    if(hasData&&validTime(channel?.received_at))text+=` · ${label} ${formatTime(channel.received_at)}`;
    node.textContent=text;node.dataset.state=status;
  }
  function renderStatus(){
    statusText('oled-status',outputs?.oled,frameValid,'last frame');
    if(frameError){$('oled-status').textContent='Invalid OLED frame';$('oled-status').dataset.state='offline';}
    const lines=outputs?.uart?.lines;
    statusText('uart-status',outputs?.uart,Array.isArray(lines)&&lines.length>0,'last message');
  }
  function renderOutputs(){
    const frame=outputs?.oled;
    frameError=false;
    if(!frame){frameKey=null;frameValid=false;}
    else if(`${frame.width}:${frame.height}:${frame.format}:${frame.data}`!==frameKey){
      try{
        const pixels=decodeOledFrame(frame),image=context.createImageData(128,64);image.data.set(pixels);
        context.imageSmoothingEnabled=false;context.putImageData(image,0,0);
        frameKey=`${frame.width}:${frame.height}:${frame.format}:${frame.data}`;frameValid=true;
      }catch(_){frameError=true;frameValid=false;frameKey=null;}
    }
    canvas.hidden=!frameValid;$('oled-empty').hidden=frameValid;
    $('oled-empty').textContent=frameError?'OLED frame unavailable.':'No OLED frame received.';
    scaleCanvas();
    const lines=outputs?.uart?.lines,text=rawUartText(lines),display=$('uart-display');
    const atBottom=display.scrollHeight-display.scrollTop-display.clientHeight<24;
    const changed=text!==lastUartText;
    if(changed){$('uart-text').textContent=text;lastUartText=text;}
    const hasLines=Array.isArray(lines)&&lines.length>0;
    $('uart-text').hidden=!hasLines;$('uart-empty').hidden=hasLines;
    if(changed&&atBottom)display.scrollTop=display.scrollHeight;
    renderStatus();
  }
  function acceptState(next){
    if(!next||typeof next!=='object')throw new Error('Invalid output response.');
    const instance=next.server_instance,revision=next.revision,time=next.generated_at;
    if(typeof instance!=='string'||!Number.isSafeInteger(revision)||revision<0||!validTime(time))throw new Error('Invalid output response.');
    if(retiredServers.has(instance))return;
    if(order){
      if(instance===order.instance&&(revision<order.revision||(revision===order.revision&&time<=order.time)))return;
      if(instance!==order.instance)retiredServers.add(order.instance);
    }
    order={instance,revision,time};generatedAt=time;lastApiAt=Date.now();reachable=true;
    outputs=next.outputs&&typeof next.outputs==='object'?next.outputs:null;
    renderOutputs();
  }
  async function poll(){
    if(fetching)return;fetching=true;
    const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),5000);
    try{
      const response=await fetch('/api/state',{cache:'no-store',signal:controller.signal});
      if(!response.ok)throw new Error('Output unavailable.');
      acceptState(await response.json());
    }catch(_){reachable=false;renderStatus();}
    finally{clearTimeout(timeout);fetching=false;}
  }
  window.addEventListener('resize',scaleCanvas);
  if(typeof ResizeObserver!=='undefined')new ResizeObserver(scaleCanvas).observe($('oled-display'));
  scaleCanvas();poll();setInterval(poll,1000);setInterval(renderStatus,250);
})();
