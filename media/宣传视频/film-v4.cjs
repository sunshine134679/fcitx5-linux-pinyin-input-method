#!/usr/bin/env node
'use strict';
// The native camera tour is conformed from the original 4K film by render-v4.py.
// Only the new motion-design sequences are rendered here; every frame is deterministic.
const fs=require('node:fs'),path=require('node:path'),{spawn}=require('node:child_process'),{once}=require('node:events');
let skia;try{skia=require(process.env.SKIA_CANVAS_MODULE||'skia-canvas')}catch{skia=require('/home/wsl/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/@oai/artifact-tool/node_modules/skia-canvas')}
const {Canvas,loadImage}=skia,ROOT=__dirname,OUT=path.join(ROOT,'output/v4');
const args=process.argv.slice(2),opt=(n,v)=>args.includes(n)?args[args.indexOf(n)+1]:v;
const W=Number(opt('--width',3840)),H=W*9/16,S=W/1920,FPS=60;
const canvas=new Canvas(W,H,{gpu:false}),g=canvas.getContext('2d'),images={};
const INK='#29233b',PURPLE='#7451ae',JADE='#087f63';
const clamp=x=>Math.max(0,Math.min(1,x)),mix=(a,b,p)=>a+(b-a)*p,r=(t,a,b)=>clamp((t-a)/(b-a));
const ease=x=>1-(1-clamp(x))**4,io=x=>(x=clamp(x),x<.5?8*x**4:1-(-2*x+2)**4/2);
const spring=x=>x<=0?0:x>=1?1:1-Math.exp(-7*x)*Math.cos(10*x);
function opacity(a,fn){if(a<=0)return;g.save();g.globalAlpha*=clamp(a);fn();g.restore()}
function box(x,y,w,h,rad,fill,stroke){g.beginPath();g.roundRect(x,y,w,h,rad);if(fill){g.fillStyle=fill;g.fill()}if(stroke){g.lineWidth=1.3;g.strokeStyle=stroke;g.stroke()}}
function text(v,x,y,size=40,color=INK,weight=400,align='left',family='Noto Sans CJK SC'){g.font=`${weight} ${size}px "${family}"`;g.textAlign=align;g.textBaseline='alphabetic';g.fillStyle=color;g.fillText(v,x,y)}
function measure(v,size=40,weight=400,family='Noto Sans CJK SC'){g.font=`${weight} ${size}px "${family}"`;return g.measureText(v).width}
function line(x1,y1,x2,y2,color,width=2){g.beginPath();g.moveTo(x1,y1);g.lineTo(x2,y2);g.strokeStyle=color;g.lineWidth=width;g.lineCap='round';g.stroke()}
function background(t,color='#f5f1fc',ink=INK){
 g.fillStyle=color;g.fillRect(0,0,1920,1080);
 const light=g.createLinearGradient(-100+t*6,0,1920,980);light.addColorStop(0,'#ffffffaa');light.addColorStop(.6,'#ffffff00');light.addColorStop(1,'#d7c9ee22');g.fillStyle=light;g.fillRect(0,0,1920,1080);
 g.drawImage(images.logo,75,53,34,34);text('ModernIME',123,80,24,ink,600,'left','Noto Sans');
}
function foot(note='输入流程动画演示',color='#8c7f9b'){text(note,86,1035,18,color)}
function headline(title,sub,t,color=INK){
 const p=ease(r(t,0,.42));g.save();g.beginPath();g.rect(75,105,1780,145);g.clip();
 text(title,88+35*(1-p),165+80*(1-p),54,color,650);g.restore();
 opacity(ease(r(t,.14,.48)),()=>text(sub,92,216+18*(1-p),25,color));
}
function kinetic(value,x,y,size,t,color=PURPLE,family='AR PL UKai CN',align='center'){
 const weight=family==='Noto Sans'?750:400,chars=[...value],width=measure(value,size,weight,family);let xx=align==='center'?x-width/2:x;
 chars.forEach((ch,i)=>{const p=spring(r(t-i*.055,0,.38)),cw=measure(ch,size,weight,family);opacity(r(t-i*.055,0,.12),()=>{
  g.save();g.translate(xx+cw/2,y+(1-p)*90);g.scale(.78+.22*p,.78+.22*p);text(ch,0,0,size,color,weight,'center',family);g.restore();});xx+=cw;});
}
function ring(x,y,age,color=PURPLE){if(age<0||age>.4)return;const p=age/.4;g.beginPath();g.arc(x,y,20+95*p,0,Math.PI*2);g.strokeStyle=color;g.lineWidth=3;opacity(1-p,()=>g.stroke())}
function keyboard(key,accent=PURPLE,entry=1){
 g.save();g.translate(0,80*(1-entry));g.globalAlpha*=entry;
 ['1234567890','qwertyuiop','asdfghjkl','zxcvbnm,.'].forEach((row,j)=>{
  const kw=52,gap=8,start=960-(row.length*(kw+gap)-gap)/2;
  [...row].forEach((k,i)=>{const x=start+i*(kw+gap),y=787+j*44,on=k===key;
   box(x,y+3,kw,37,7,'#cfc5de');box(x,y+(on?3:0),kw,35,7,on?accent:'#fff',on?accent:'#ded4e9');text(k.toUpperCase(),x+26,y+25+(on?3:0),18,on?'#fff':'#7e728d',500,'center','Noto Sans');
  });
 });
 const on=key==='SPACE';box(842,967,236,39,9,'#cfc5de');box(842,967+(on?3:0),236,36,9,on?accent:'#fff','#ded4e9');text('空格',960,992+(on?3:0),18,on?'#fff':'#827090',500,'center');g.restore();
}
function crop(key,clip,x,y,w,h,rotation=0){
 const im=images[key],ratio=im.width/980;
 g.save();g.translate(x,y);g.rotate(rotation);g.beginPath();g.roundRect(-w/2,-h/2,w,h,18);g.clip();g.drawImage(im,...clip.map(v=>v*ratio),-w/2,-h/2,w,h);g.restore();
}
function hook(t){
 background(t,'#eee6f8');
 const press=r(t,.28,.42),bloom=ease(r(t,.4,.95));
 g.save();g.translate(960,510+14*Math.sin(press*Math.PI));
 const z=.4+.6*spring(r(t,0,.38));g.scale(z,z);
 box(-mix(240,860,bloom),-mix(90,255,bloom),mix(480,1720,bloom),mix(180,510,bloom),mix(26,140,bloom),'#c9b7df');
 box(-mix(240,860,bloom),-mix(103,268,bloom),mix(480,1720,bloom),mix(180,510,bloom),mix(26,140,bloom),bloom>.06?PURPLE:'#fff');
 opacity(1-r(t,.4,.52),()=>text('空格',0,24,68,PURPLE,500,'center'));
 kinetic('你好。',0,85,230,t-.43,'#fff');g.restore();
 opacity(ease(r(t,.8,1.1)),()=>text('从一次按下，到一种表达。',960,860,36,PURPLE,450,'center'));
}
const typing=[{start:.16,interval:.11,pinyin:'nihao',commit:1.03,word:'你好',after:'，',afterAt:1.25},{start:1.55,interval:.105,pinyin:'shijie',commit:2.45,word:'世界',after:'。',afterAt:2.68}];
function typingState(t){let written='',pre='',target='',key=null;
 for(const p of typing){
  if(t>=p.commit){written+=p.word;if(t-p.commit<.13)key='SPACE';if(t>=p.afterAt){written+=p.after;if(t-p.afterAt<.13)key=p.after==='，'?',':'.'}}
  else if(t>=p.start){const n=Math.floor((t-p.start)/p.interval);pre=p.pinyin.slice(0,n+1);target=p.word;if(n<p.pinyin.length&&(t-p.start)%p.interval<.09)key=p.pinyin[n]}
 }return {written,pre,target,key};
}
function typingShot(t){
 background(t,'#efe8fa');headline('让输入，跟上你的想法。','拼音逐字输入，空格确认，汉字落下。',t);foot();
 const state=typingState(t),enter=ease(r(t,0,.42)),pull=io(r(t,3.2,5.13));
 const x=mix(1000,930,enter)+140*pull,y=508+22*pull,z=mix(.75,1.08,enter)-.15*pull;
 g.save();g.translate(x,y);g.scale(z,z);g.rotate(-.025*(1-enter)+.022*pull);
 box(-690,-215,1380,430,25,'#fdfcff','#ddd2e8');text('今天想写点什么',-626,-151,22,'#a094ac');line(-626,-125,626,-125,'#e9e3ef',1);
 const written=state.written,cx=-602+measure(written,108,500);text(written,-602,5,108,INK,500);box(cx+8,-98,5,114,3,PURPLE);
 if(state.pre){
  const xx=Math.min(cx+8,130);box(xx,43,590,126,18,'#f7f3ff','#d8cae9');text(state.pre,xx+24,86,32,INK,450,'left','Noto Sans');
  box(xx+14,108,184,48,10,PURPLE);text('1',xx+29,142,22,'#fff');text(state.target,xx+56,144,34,'#fff',500);text('空格确认',xx+460,144,20,INK);
 }
 if(t>2.9)kinetic('你的表达，自有风格。',-600,155,44,t-2.9,PURPLE,'AR PL UKai CN','left');
 g.restore();keyboard(state.key,PURPLE,enter);
}
function learning(t){
 background(t,'#eee6f8');headline('常用的，越来越顺手。','记住主动选择，让常用的表达慢慢靠前。',t);
 foot('学习过程示意 · 排序随词频、上下文和使用变化');
 const arrive=spring(r(t,0,.65)),promote=io(r(t,3.45,4.08)),push=io(r(t,4.4,5.25)),exit=io(r(t,6.7,7.35));
 const selected=[1.05,2.08,3.0].some(a=>t>=a&&t<a+.32);
 g.save();g.translate(960+15*push,510+(1-arrive)*330-80*push);g.scale(.87+.13*arrive+.1*push,.87+.13*arrive+.1*push);
 text('shi',-450*push,-115,48,PURPLE,450,'center','Noto Sans');
 ['是','时','试','事'].forEach((word,i)=>{
  const target=[1,2,0,3][i],xx=mix(-450+i*300,-450+target*300,promote),yy=i===2?-Math.sin(promote*Math.PI)*140:0,active=i===2&&(selected||promote>.01);
  opacity(i===2?1:1-push,()=>{g.save();g.translate(xx,yy);const scale=active&&promote===0?1.055:1;g.scale(scale,scale);
   box(-130,9,260,140,22,'#d2bfE5');box(-130,0,260,140,22,active?PURPLE:'#fff',active?PURPLE:'#d3c3e6');
   text(String(promote>.99?target+1:i+1),-102,34,19,active?'#ded0ef':'#9a8aa9');text(word,0,105,82,active?'#fff':INK,500,'center');g.restore();
  });
 });
 [1.05,2.08,3.0].forEach(a=>ring(150,70,t-a));g.restore();
 if(t<4.2){
  [1.05,2.08,3.0].forEach(a=>{const age=t-a,p=ease(r(age,0,.5));if(age>=0&&age<.9)opacity(1-r(age,.58,.9),()=>text('试',mix(1110,960,p),mix(615,827,p),mix(110,86,p),PURPLE,500,'center'))});
  text('一次次主动选择',960,934,24,'#9682a9',400,'center');
 }else{
  opacity(1-exit,()=>{kinetic('越用，',1290,490,90,t-4.45,PURPLE);kinetic('越合拍。',1290,620,118,t-4.65,PURPLE);text('多次选择后',460,690,24,'#9682a9',400,'center')});
  const sentence='试一下，新的灵感。',n=Math.min(sentence.length,Math.max(0,Math.floor((t-6.55)/.105)));
  opacity(ease(r(t,6.55,6.9)),()=>{box(300,735,1320,190,30,'#fff','#d5c4e7');text(sentence.slice(0,n),400,862,77,INK,500);});
 }
}
function dictionary(t){
 background(t);headline('你的词，随手就来。','人名、项目名、专业术语，收进个人词典。',t);foot('v0.2.3 真实界面 · 使用独立的演示词典');
 const enter=ease(r(t,.05,.65)),zoom=io(r(t,1.45,2.4));
 crop('dictionary-demo',[244,150,711,430],mix(2200,1270,enter)-230*zoom,610,1050+170*zoom,(1050+170*zoom)*430/711,-.035*(1-zoom));
 [['云笺','项目名'],['星野','人名'],['异步调用','专业术语']].forEach(([word,label],i)=>{
  const p=ease(r(t,i*.13,.45+i*.13)),depart=io(r(t,1.4+i*.1,2.2+i*.1));
  opacity(p*(1-depart),()=>{text(word,140+650*depart,420+i*160+45*(1-p),i===2?66:86,INK,550);text(label,145+650*depart,468+i*160+45*(1-p),24,'#9c88ae')});
 });
}
function privacy(t){
 background(t,'#e8f4ed');headline('记住什么，你来定。','候选记忆与上下文排序，可以分别开关。',t);foot('v0.2.3 真实界面 · 输入与学习在本地完成','#789283');
 const enter=ease(r(t,0,.48)),reframe=io(r(t,1.7,2.35)),change=ease(r(t,1.08,1.24));
 const x=960+800*(1-enter)-380*reframe,y=535,w=1580-620*reframe,h=w*194/711;
 crop('learning-lavender',[244,102,711,194],x,y,w,h);opacity(change,()=>crop('learning-private',[244,102,711,194],x,y,w,h));
 ring(x+(912-599.5)*w/711,y+(245-199)*w/711,t-1.08,JADE);
 opacity(reframe,()=>{kinetic('开关自己定',1410,545,80,t-1.9,JADE);text('输入与学习，留在本地。',1410,635,31,'#527b69',500,'center')});
}
function efficiency(t){
 const second=t>=3.433333333,local=second?t-3.433333333:t,accent=second?'#a65f16':'#2765a3';
 background(t,second?'#fcf3e8':'#eaf2fa');headline('少敲几下，表达更快。',second?'v 模式：金额转中文大写。':'英文前缀补全。',local);foot('输入流程动画演示 · 英文补全与 v 模式金额转换');
 const source=second?'v88.5':'syst',word=second?'捌拾捌元伍角整':'system',elapsed=local-.15,n=elapsed>=0?Math.min(source.length,Math.floor(elapsed/.12)+1):0;
 const committed=local>=1.22,p=committed?ease(r(local,1.22,1.55)):0;
 box(260,305,1400,442,32,'#ffffffd9',second?'#e5cfaf':'#c3d8ee');
 if(!committed){text(source.slice(0,n),960,442,76,accent,500,'center','Noto Sans');
  if(n){box(435,505,1050,132,21,second?'#fff7e9':'#edf4fd');box(453,565,1014,58,12,accent);text('1',478,605,23,'#fff');text(word,526,609,38,'#fff',500);text('空格确认',1438,543,20,accent,400,'right')}
 }else{
  g.save();g.translate(960,503);g.scale(1.12-.12*p,1.12-.12*p);text(word,0,0,second?83:122,INK,500,'center',second?'Noto Sans CJK SC':'Noto Sans');g.restore();
  kinetic(second?'把繁琐，写轻巧。':'表达，自然衔接。',960,661,56,local-1.55,accent);
 }
 const i=Math.floor(Math.max(0,elapsed)/.12),hit=elapsed>=0&&i<source.length&&elapsed%.12<.09?source[i]:local>=1.22&&local<1.36?'SPACE':null;
 keyboard(hit,accent,ease(r(local,0,.3)));
}
function ending(t){
 background(t);const p=spring(r(t,0,.55));
 g.save();g.translate(960,252);g.scale(.7+.3*p,.7+.3*p);g.drawImage(images.logo,-57,-57,114,114);g.restore();
 kinetic('ModernIME',960,475,128,t-.12,INK,'Noto Sans');
 kinetic('让每一次输入，都更像你。',960,576,54,t-.32,PURPLE);
 opacity(ease(r(t,.6,1)),()=>{
  box(235,638,1450,116,22,'#fff','#ded2ef');text('https://sunshine134679.github.io/',960,684,30,PURPLE,600,'center','Noto Sans');
  text('fcitx5-linux-pinyin-input-method/',960,729,32,PURPLE,550,'center','Noto Sans');
  text('GitHub · 源码与发行版',960,819,25,'#756584',500,'center');text('github.com/sunshine134679/fcitx5-linux-pinyin-input-method',960,864,28,INK,450,'center','Noto Sans');text('Linux 拼音输入法',960,938,25,'#8b7b9b',400,'center');
 });text('Music: Digital Clouds — Alejandro Magaña (A. M.) / Mixkit',960,1017,16,'#a89bb4',400,'center','Noto Sans');
}
const scenes=[[0,hook],[103,typingShot],[1749,learning],[2263,dictionary],[2469,privacy],[2674,efficiency],[3291,ending]];
const segments={opening:[0,411],learning:[1749,3086],ending:[3291,3600]};
function frame(n){canvas.width=W;g.setTransform(S,0,0,S,0,0);g.imageSmoothingEnabled=true;g.imageSmoothingQuality='high';const item=scenes.findLast(([start])=>n>=start);item[1]((n-item[0])/60)}
async function prepare(){fs.mkdirSync(OUT,{recursive:true});images.logo=await loadImage(path.join(ROOT,'../../config/icons/512x512/modernime.png'));
 if(opt('--segment','')==='opening'||opt('--segment','')==='ending'||args.includes('--poster'))return;
 for(const name of ['dictionary-demo','learning-lavender','learning-private'])images[name]=await loadImage(path.join(ROOT,'assets/retina/'+name+'.png'));
}
async function main(){await prepare();
 if(args.includes('--at')||args.includes('--poster')){const times=args.includes('--poster')?[58]:opt('--at','1,3,30,32.2,33.3,35,37,39.7,42.7,46.5,49.8,58').split(',').map(Number);for(const t of times){frame(Math.round(t*60));canvas.toFileSync(path.join(OUT,t===58?'节奏重剪版封面.png':`frame-${t}.png`))}return}
 const name=opt('--segment','opening'),[start,end]=segments[name]||[];if(start===undefined)throw Error('Unknown segment '+name);
 const file=path.join(OUT,`${name}-${H}p.mp4`),ff=process.env.FFMPEG||'ffmpeg';
 const proc=spawn(ff,['-y','-hide_banner','-loglevel','warning','-f','rawvideo','-pixel_format','rgba','-video_size',`${W}x${H}`,'-framerate','60','-i','pipe:0','-an','-c:v','libx264','-preset','fast','-crf','16','-pix_fmt','yuv420p','-threads','3','-vf','scale=in_range=full:out_range=tv:out_color_matrix=bt709','-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709','-video_track_timescale','15360','-movflags','+faststart',file],{stdio:['pipe','ignore','inherit']}),finished=once(proc,'close');
 proc.stdin.on('error',e=>{if(e.code!=='EPIPE')throw e});const clock=Date.now();
 for(let n=start;n<end;n++){frame(n);const raw=await canvas.toBuffer('raw',{colorType:'rgba'});if(!proc.stdin.write(raw))await once(proc.stdin,'drain');if((n-start)%120===0)console.log(`${name}: ${Math.round((n-start)/(end-start)*100)}% · ${Math.round((Date.now()-clock)/1000)}s`)}
 proc.stdin.end();const [code]=await finished;if(code)throw Error('FFmpeg failed '+code);console.log('Rendered '+file);
}
main().catch(e=>{console.error(e);process.exitCode=1});
