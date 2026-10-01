'use strict';
const el=id=>document.getElementById(id);let blob=null,image=null,items=[],busy=false;
function setBusy(value){busy=value;['run','file','sample','operation'].forEach(id=>{el(id).disabled=value;});}
function draw(){if(!image)return;const c=el('canvas');c.width=image.width;c.height=image.height;const ctx=c.getContext('2d');ctx.drawImage(image,0,0);ctx.strokeStyle='#ed3535';ctx.lineWidth=Math.max(1.5,image.width/700);ctx.font=`${Math.max(14,image.width/45)}px sans-serif`;items.forEach((b,i)=>{ctx.beginPath();ctx.moveTo(b.x1,b.y1);ctx.lineTo(b.x2,b.y2);ctx.lineTo(b.x3,b.y3);ctx.lineTo(b.x4,b.y4);ctx.closePath();ctx.stroke();ctx.fillStyle='#ed3535';ctx.fillText(String(i+1),b.x1,Math.max(18,b.y1-4));});}
// Browsers may apply EXIF orientation while stb does not. Remove JPEG APP1
// metadata for BOTH preview and upload without decoding/re-encoding JPEG pixels.
async function rawOrientation(value){
 if(value.size>20*1024*1024)throw Error('网页图片上限为 20 MiB');
 const bytes=new Uint8Array(await value.arrayBuffer());if(bytes[0]!==255||bytes[1]!==216)return value;
 let offset=2,kept=0,changed=false;const parts=[];
 while(offset+1<bytes.length){const start=offset;if(bytes[offset++]!==255)throw Error('JPEG 头格式错误');while(bytes[offset]===255)offset++;const marker=bytes[offset++];if(marker===218||marker===217)break;
  if(marker===1||(marker>=208&&marker<=215))continue;
  if(offset+2>bytes.length)throw Error('JPEG 头截断');const length=(bytes[offset]<<8)|bytes[offset+1];if(length<2||offset+length>bytes.length)throw Error('JPEG 段长度错误');
  const end=offset+length;if(marker===225){parts.push(bytes.slice(kept,start));kept=end;changed=true;}offset=end;
 }
 if(!changed)return value;parts.push(bytes.slice(kept));return new Blob(parts,{type:'image/jpeg'});
}
async function load(value){if(busy)return;setBusy(true);try{value=await rawOrientation(value);const bitmap=await createImageBitmap(value);if(bitmap.width*bitmap.height>40000000||bitmap.width>20000||bitmap.height>20000){bitmap.close();throw Error('图片超过 4000 万像素或单边 20000 限制');}if(image)image.close();image=bitmap;blob=value;items=[];el('text').textContent='';el('json').textContent='';el('timing').textContent='';draw();el('status').textContent=`已加载 ${image.width} × ${image.height}（原始像素方向）`;}finally{setBusy(false);}}
function failure(e){el('status').textContent=`失败：${e.message}`;}
el('file').addEventListener('change',async e=>{try{if(e.target.files[0])await load(e.target.files[0]);}catch(error){failure(error);}});
el('sample').addEventListener('click',async()=>{try{const r=await fetch('/sample.jpg');if(!r.ok)throw Error('测试图片加载失败');await load(await r.blob());}catch(error){failure(error);}});
el('run').addEventListener('click',async()=>{if(busy)return;if(!blob){failure(Error('请先选择图片'));return;}setBusy(true);el('status').textContent='正在识别…';const started=performance.now();
 try{const headers={'Content-Type':'application/octet-stream'};if(el('key').value)headers['X-API-Key']=el('key').value;const r=await fetch(`/api/${el('operation').value}`,{method:'POST',headers,body:blob});const data=await r.json();if(!r.ok||!data.ok)throw Error(`${r.status} ${data.error_code}: ${data.error} [${data.request_id||'transport'}]`);const result=data.result;items=data.operation==='ocr'?result.items:[];draw();el('text').textContent=items.length?items.map(i=>i.text).join('\n'):(result.text||'未检测到文字');el('json').textContent=JSON.stringify(data,null,2);const timing=result.timing;el('timing').textContent=`浏览器往返 ${(performance.now()-started).toFixed(1)} ms · 服务端 ${data.server_total_ms.toFixed(1)} ms`+(timing?` · GPU DET ${timing.det_ms.toFixed(1)} / CLS ${timing.cls_ms.toFixed(1)} / REC ${timing.rec_ms.toFixed(1)} ms`:` · GPU REC ${result.gpu_rec_ms.toFixed(1)} ms`);el('status').textContent=`完成 · 请求 ${data.request_id}`;
 }catch(error){failure(error);}finally{setBusy(false);}});
el('donate').addEventListener('click',()=>el('sponsor').showModal());el('close').addEventListener('click',()=>el('sponsor').close());
// Native dialog handles Escape and focus restoration; click outside the card to dismiss.
el('sponsor').addEventListener('click',event=>{
 if(event.target!==el('sponsor'))return;
 const rect=el('sponsor').getBoundingClientRect();
 if(event.clientX<rect.left||event.clientX>rect.right||event.clientY<rect.top||event.clientY>rect.bottom)el('sponsor').close();
});
