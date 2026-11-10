#!/usr/bin/env node
'use strict';
// 原生 4K 逐帧动画；文字、候选与上下文共用连续运动轨迹。
const fs=require('node:fs'),path=require('node:path'),{spawn}=require('node:child_process'),{once}=require('node:events');
let skia;try{skia=require(process.env.SKIA_CANVAS_MODULE||'skia-canvas')}catch{skia=require('/home/wsl/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/@oai/artifact-tool/node_modules/skia-canvas')}
const {Canvas,loadImage}=skia,ROOT=__dirname,OUT=path.join(ROOT,'output/v5');
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
// Sampled Bézier strokes keep reveal, moving accents and character travel on one path.
function point(c,p){const q=1-p;return [q*q*q*c[0]+3*q*q*p*c[2]+3*q*p*p*c[4]+p*p*p*c[6],q*q*q*c[1]+3*q*q*p*c[3]+3*q*p*p*c[5]+p*p*p*c[7]]}
function stroke(c,end=1,color=PURPLE,width=3,start=0){
 if(end<=start)return;g.beginPath();for(let i=0;i<=100;i++){const [x,y]=point(c,mix(start,end,i/100));i?g.lineTo(x,y):g.moveTo(x,y)}g.lineWidth=width;g.lineCap='round';g.strokeStyle=color;g.stroke();
}
function dot(x,y,radius,color){g.beginPath();g.arc(x,y,radius,0,Math.PI*2);g.fillStyle=color;g.fill()}
function trail(c,t,color=PURPLE,duration=.8){const p=r(t,0,duration);stroke(c,p,color,3,Math.max(0,p-.32));const [x,y]=point(c,p);if(p>0&&p<1)dot(x,y,6,color)}
function flourish(t,color=PURPLE){
 opacity(.15,()=>stroke([1220,140,1600,30,1900,330,1720,660],ease(r(t,.1,1.3)),color,2));
 opacity(.14,()=>stroke([70,795,150,540,40,330,300,260],ease(r(t,.3,1.4)),color,2));
}
function letters(value,x,y,size,t,color=INK,family='Noto Serif CJK SC',spacing=0){
 let xx=x;[...value].forEach((ch,i)=>{const age=t-i*.045,p=spring(r(age,0,.52)),cw=measure(ch,size,600,family);
  opacity(r(age,0,.12),()=>{g.save();g.translate(xx+cw/2,y+48*(1-p));g.rotate((i%2?-.07:.07)*(1-p));g.scale(1+.13*(1-p),1-.2*(1-p));text(ch,0,0,size,color,600,'center',family);g.restore()});xx+=cw+spacing;
 });
}
const typed=[{start:.18,interval:.10,pinyin:'nihao',commit:.95,word:'你好',after:'，',afterAt:1.21},{start:1.48,interval:.095,pinyin:'shijie',commit:2.23,word:'世界',after:'。',afterAt:2.49}];
function typingState(t){let written='',pre='',target='',key=null;
 for(const p of typed){if(t>=p.commit){written+=p.word;if(t-p.commit<.13)key='SPACE';if(t>=p.afterAt){written+=p.after;if(t-p.afterAt<.13)key=p.after==='，'?',':'.'}}else if(t>=p.start){const n=Math.floor((t-p.start)/p.interval);pre=p.pinyin.slice(0,n+1);target=p.word;if(n<p.pinyin.length&&(t-p.start)%p.interval<.08)key=p.pinyin[n]}}return {written,pre,target,key};
}
function opening(t){
 background(t,'#f4effb');flourish(t);const state=typingState(t),reframe=io(r(t,3,3.85));
 opacity(1-reframe,()=>{text('从一次按下，到一句表达。',190,255,35,PURPLE,450);text('拼音输入 · 空格确认',1710,252,21,'#8f7c9d',400,'right')});
 // The sentence lives on the page itself; the only panel is the small candidate strip.
 g.save();g.translate(mix(190,1155,reframe),mix(515,268,reframe));g.scale(1-.64*reframe,1-.64*reframe);
 let cursor=0;
 for(const p of typed){if(t>=p.commit){letters(p.word,cursor,0,156,t-p.commit,INK);cursor+=measure(p.word,156,600,'Noto Serif CJK SC');if(t>=p.afterAt){letters(p.after,cursor,0,156,t-p.afterAt,PURPLE);cursor+=measure(p.after,156,600,'Noto Serif CJK SC')}}}
 if(reframe<1){const height=148+5*Math.sin(t*4);line(cursor+12,-height,cursor+12,16,PURPLE,5);}
 if(state.pre){const cx=Math.min(cursor,980);text(state.pre,cx+17,76,36,PURPLE,500,'left','Noto Sans');
  box(cx,103,480,76,15,'#ffffff','#ddcfee');box(cx+10,113,185,56,10,PURPLE);text('1',cx+25,151,21,'#ded0f3');text(state.target,cx+60,154,32,'#fff',500);text('空格确认',cx+440,153,20,'#8a759b',400,'right');
 }g.restore();
 opacity(1-reframe,()=>{g.save();g.translate(0,120*(1-ease(r(t,0,.35))));keyboard(state.key,PURPLE,ease(r(t,0,.35)));g.restore()});
 opacity(reframe,()=>{
  letters('让输入，',190,535,124,t-3.14,INK,'AR PL UKai CN');letters('自然发生。',445,713,154,t-3.36,PURPLE,'AR PL UKai CN');
  const c=[220,770,720,900,1410,850,1680,435];stroke(c,ease(r(t,3.65,5.05)),'#bca5d8',3);
  trail(c,t-4.55,PURPLE,1.55);
  text('ModernIME  /  Linux 拼音输入法',205,935,25,'#8d769f',450);
  const arc=point(c,clamp(.55+.16*Math.sin((t-4)*.8)));opacity(ease(r(t,4.1,4.5)),()=>{g.save();g.translate(arc[0],arc[1]-30);g.rotate(-.11);text('灵感',0,0,47,PURPLE,400,'center','AR PL UKai CN');g.restore()});
 });foot('输入流程动画演示');
}
function habits(t){
 const inP=ease(r(t,0,.4));g.save();g.translate(85*(1-inP),0);g.globalAlpha=inP;
 headline('常用的，慢慢靠前。','记住你的主动选择。',t);
 letters('越用，',170,480,125,t-.1,INK,'AR PL UKai CN');letters('越合拍。',285,660,147,t-.27,PURPLE,'AR PL UKai CN');
 const history=[['试一试新的配色。',1.0,1170,325],['试一下这个想法。',1.85,1240,792]];
 history.forEach(([sentence,at,x,y],i)=>{
  opacity(ease(r(t,at-.55,at-.2)),()=>{text(sentence,x,y,35,'#917aab',400,'center','Noto Serif CJK SC');
   const c=i?[x,y-45,1570,700,1500,585,1260,535]:[x,y+30,1560,360,1500,415,1260,480];
   opacity(.25,()=>stroke(c,1,'#ac91c8',2));trail(c,t-at,PURPLE,.7);
  });
 });
 const promoted=io(r(t,2.55,3.25)),x0=925,y0=535;
 text('shi',x0-15,y0-102,44,PURPLE,500,'left','Noto Sans');
 line(x0-30,y0+38,1700,y0+38,'#d5c3e7',2);
 ['是','时','试','事'].forEach((word,i)=>{
  const to=[1,2,0,3][i],x=x0+mix(i,to,promoted)*195,arc=i===2?-Math.sin(promoted*Math.PI)*100:Math.sin(promoted*Math.PI)*35;
  const selected=i===2&&(t>1||promoted>0),press=[1,1.85].some(at=>t>=at&&t<at+.16);
  if(selected){opacity(.12,()=>{g.save();g.translate(x+42,y0-26+arc);g.rotate(-.12);g.beginPath();g.ellipse(0,0,73,59,0,0,Math.PI*2);g.fillStyle=PURPLE;g.fill();g.restore()});}
  text(String(promoted===1?to+1:i+1),x-25,y0-31+arc,17,'#a28bb7');
  text(word,x+35,y0+arc+(selected&&press?4:0),70,selected?PURPLE:INK,500,'center','Noto Serif CJK SC');
 });
 opacity(ease(r(t,2.7,3.15)),()=>text('多次选择后，常用表达更靠前',1280,650,25,PURPLE,450,'center'));
 g.restore();
}
function contextPark(t){
 headline('同一个音，放进语境。','结合光标前的文字，调整候选排序。',t,INK);
 const enter=io(r(t,.55,1.45)),pick=io(r(t,1.8,2.55)),sentenceX=310,baseY=590;
 opacity(1-ease(r(t,1.8,2.3)),()=>{text('未提供前文',375,385,24,'#8e839f',400,'center');text('原',375,580,150,'#9d8bad',500,'center','Noto Serif CJK SC');text('yuan',375,665,38,PURPLE,400,'center','Noto Sans')});
 const sx=mix(780,sentenceX,pick);
 opacity(enter,()=>{
  letters('我们去公',sx,baseY,137,t-.6,INK,'Noto Serif CJK SC');
  const end=sx+548;line(sx,baseY+37,end,baseY+37,'#91bea7',5);
  text('光标前的文字',sx+265,baseY+100,24,'#638673',400,'center');
  if(pick<1)line(end+14,baseY-130,end+14,baseY+7,JADE,4);
 });
 const c=[1450,380,1770,390,1300,660,sentenceX+548+69,baseY-55];
 opacity(enter,()=>{stroke(c,Math.min(1,pick+.08),'#83b69c',3);const [x,y]=point(c,pick);
  g.save();g.translate(x,y);g.rotate(-.08*Math.sin(pick*Math.PI));text('园',0,55,137,JADE,600,'center','Noto Serif CJK SC');g.restore();
  opacity(1-pick,()=>text('yuan',1450,280,41,JADE,500,'center','Noto Sans'));
 });
 opacity(ease(r(t,2.6,3)),()=>{
  text('我们去公 + yuan → 园',330,831,29,'#527f6c',450);
  letters('接得上，才顺口。',1200,872,58,t-2.7,JADE,'AR PL UKai CN');
 });
}
function contextBalloon(t){
 headline('一个词，也有它的语境。','“气球”之后，chongqi 优先呈现“充气”。',t);
 const draw=ease(r(t,.1,.95)),n=Math.min(7,Math.max(0,Math.floor((t-.35)/.1)+1)),p=io(r(t,1.25,1.85));
 const c=[650,560,865,790,965,250,1165,470];opacity(.35,()=>stroke(c,draw,'#75aa91',3));trail(c,t-.95,JADE,.8);
 letters('气球',255,570,156,t-.1,INK,'Noto Serif CJK SC');text('前文',267,382,24,'#82958b');
 text('chongqi'.slice(0,n),1240,379,46,JADE,500,'center','Noto Sans');
 opacity(p,()=>{g.save();g.translate(1240,568+40*(1-p));g.scale(.82+.18*p,.82+.18*p);text('充气',0,0,156,JADE,600,'center','Noto Serif CJK SC');g.restore();text('上下文首选',1240,657,25,'#628b76',400,'center');});
 // The balloon is drawn from the semantic link; its string follows through after the word arrives.
 const by=380-20*Math.sin(t*1.5),b=ease(r(t,1.6,2.3));
 opacity(b,()=>{g.save();g.translate(1655,by+70*(1-b));g.rotate(.1*Math.sin(t*1.1));
  g.beginPath();g.ellipse(0,0,65,83,0,0,Math.PI*2);g.fillStyle='#dceee3';g.fill();g.strokeStyle='#75aa91';g.lineWidth=2;g.stroke();
  stroke([0,83,-45,149,45,210,-9,285],1,'#75aa91',2);g.restore();
 });
 letters('让前文，带出下一词。',310,852,63,t-1.55,JADE,'AR PL UKai CN');
}
function learning(t){
 const second=t>=4.3,third=t>=8.6; background(t,second?'#edf5f0':'#f4eefb');flourish(t,second?JADE:PURPLE);
 // The outgoing sentence and incoming one share a horizontal camera move.
 if(t<4.7){g.save();g.translate(-1920*io(r(t,4.05,4.7)),0);habits(t);g.restore();}
 if(t>=4.3&&t<8.95){g.save();g.translate(1920*(1-io(r(t,4.3,4.9)))-1920*io(r(t,8.45,8.95)),0);contextPark(t-4.3);g.restore();}
 if(third){g.save();g.translate(1920*(1-io(r(t,8.6,9.15))),0);contextBalloon(t-8.6);g.restore();}
 foot(second?'上下文辅助排序动画演示 · 实际结果随语境、词频与设置变化':'个性化学习动画演示 · 不代表固定选择次数或固定排序',second?'#758d7e':'#94809e');
}
function ending(t){
 g.fillStyle='#68469c';g.fillRect(0,0,1920,1080);
 const wash=g.createLinearGradient(0,0,1920,1080);wash.addColorStop(0,'#8e6abc44');wash.addColorStop(1,'#4c2c7e11');g.fillStyle=wash;g.fillRect(0,0,1920,1080);
 const p=spring(r(t,0,.65));g.save();g.translate(200,168+60*(1-p));g.scale(p,p);g.drawImage(images.logo,0,0,87,87);g.restore();
 letters('ModernIME',322,244,91,t-.12,'#fff8ee','Noto Sans');
 letters('下一句，',195,460,117,t-.3,'#fff8ee','AR PL UKai CN');
 letters('更像你。',385,638,153,t-.52,'#fff8ee','AR PL UKai CN');
 opacity(ease(r(t,.9,1.45)),()=>{
  text('获取 ModernIME',206,805,30,'#f3e4fc',500);text('Linux 拼音输入法',1715,805,26,'#d8c4ec',400,'right');
  line(206,833,1715,833,'#a78bc5',1.5);
  text('github.com/sunshine134679/fcitx5-linux-pinyin-input-method',206,895,34,'#fff8ee',500,'left','Noto Sans');
  text('下载与源码',206,949,21,'#d4bee7');
 });
 // Secondary motion begins only once all primary information has unfolded.
 const delay=t-1.9,loop=[1130,560,1030,100,1740,90,1720,520];
 const loop2=[1720,520,1705,770,1150,800,1130,560];
 opacity(.7,()=>{stroke(loop,ease(r(delay,0,1.1)),'#d2b3ed',2.5);stroke(loop2,ease(r(delay,.7,1.8)),'#d2b3ed',2.5)});
 opacity(ease(r(delay,.45,1.05)),()=>{
  g.save();g.translate(1415,550+8*Math.sin(delay*1.45));g.rotate(-.05+.025*Math.sin(delay));
  text('你',0,0,250,'#eee0fa',400,'center','AR PL UKai CN');g.restore();
 });
 if(delay>1.2){
  const life=delay-1.2,fade=ease(r(life,0,.5));
  [['，',1135,345,62,'#c2ead4'],['。',1665,660,65,'#efc58c'],['a',1700,300,48,'#e5cdf9']].forEach(([ch,x,y,size,col],i)=>{
   opacity(fade,()=>{g.save();g.translate(x+12*Math.sin(life*(.8+i*.13)+i),y+15*Math.sin(life*1.15+i));g.rotate(.15*Math.sin(life+i));text(ch,0,0,size,col,400,'center',i===2?'Noto Serif':'AR PL UKai CN');g.restore()});
  });
  const phase=(life*.15)%1,[dx,dy]=phase<.5?point(loop,phase*2):point(loop2,(phase-.5)*2);dot(dx,dy,6,'#f7dba9');
 }
 text('Music: Digital Clouds — Alejandro Magaña (A. M.) / Mixkit',206,1030,15,'#c3acd9',400,'left','Noto Sans');
}
const scenes=[[0,opening],[1749,learning],[3497,ending]],segments={opening:[0,411],learning:[1749,2469],ending:[3497,3909]};
function frame(n){canvas.width=W;g.setTransform(S,0,0,S,0,0);g.imageSmoothingEnabled=true;g.imageSmoothingQuality='high';const item=scenes.findLast(([start])=>n>=start);item[1]((n-item[0])/60)}
async function main(){
 fs.mkdirSync(OUT,{recursive:true});images.logo=await loadImage(path.join(ROOT,'../../config/icons/512x512/modernime.png'));
 if(args.includes('--at')||args.includes('--poster')){
  for(const t of (args.includes('--poster')?[63.7]:opt('--at','.55,1.7,2.7,4.8,30.7,32.7,35.8,37.1,39.8,59.9,63.7').split(',').map(Number))){frame(Math.round(t*60));canvas.toFileSync(path.join(OUT,args.includes('--poster')?'灵动版封面.png':`frame-${t}.png`))}return;
 }
 const name=opt('--segment','opening'),[start,end]=segments[name]||[];if(start===undefined)throw Error('Unknown segment '+name);
 const file=path.join(OUT,`${name}-${H}p.mp4`),ff=process.env.FFMPEG||'ffmpeg';
 const proc=spawn(ff,['-y','-hide_banner','-loglevel','warning','-f','rawvideo','-pixel_format','rgba','-video_size',`${W}x${H}`,'-framerate','60','-i','pipe:0','-an','-c:v','libx264','-preset','fast','-crf','16','-pix_fmt','yuv420p','-threads','3','-vf','scale=in_range=full:out_range=tv:out_color_matrix=bt709','-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709','-video_track_timescale','15360','-movflags','+faststart',file],{stdio:['pipe','ignore','inherit']}),finished=once(proc,'close');
 proc.stdin.on('error',e=>{if(e.code!=='EPIPE')throw e});const clock=Date.now();
 for(let n=start;n<end;n++){frame(n);const raw=await canvas.toBuffer('raw',{colorType:'rgba'});if(!proc.stdin.write(raw))await once(proc.stdin,'drain');if((n-start)%120===0)console.log(`${name}: ${Math.round((n-start)/(end-start)*100)}% · ${Math.round((Date.now()-clock)/1000)}s`)}
 proc.stdin.end();const [code]=await finished;if(code)throw Error('FFmpeg failed '+code);console.log('Rendered '+file);
}
main().catch(e=>{console.error(e);process.exitCode=1});
