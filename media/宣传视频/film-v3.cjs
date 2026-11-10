#!/usr/bin/env node
'use strict';
const fs=require('node:fs'),path=require('node:path'),{spawn}=require('node:child_process'),{once}=require('node:events');
let skia;try{skia=require(process.env.SKIA_CANVAS_MODULE||'skia-canvas')}catch{skia=require('/home/wsl/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/@oai/artifact-tool/node_modules/skia-canvas')}
const {Canvas,loadImage}=skia,ROOT=__dirname,OUT=path.join(ROOT,'output/v3'),timing=require('./timing-v3.json');
const args=process.argv.slice(2),opt=(n,v)=>args.includes(n)?args[args.indexOf(n)+1]:v;
const W=Number(opt('--width',3840)),H=W*9/16,FPS=Number(opt('--fps',60)),S=W/1920,DURATION=96,FROM=Number(opt('--from',0));
const canvas=new Canvas(W,H,{gpu:false}),g=canvas.getContext('2d'),images={};
const INK='#29233b',PURPLE='#7451ae';
const themes=[
 ['lavender','雾紫','#7052ad','#eee7f8','#29233b'],['blue','晴空蓝','#1262ff','#e8f0ff','#182e51'],
 ['jade','青竹绿','#087f73','#e5f2eb','#173b32'],['amber','暖砂橙','#a65f16','#f8edda','#43301f'],
 ['rose','玫瑰粉','#a83d63','#f8e5ee','#482738'],['night','深海夜','#89b9ff','#182536','#f1f6ff'],
 ['contrast','高对比','#ffe066','#090909','#fff']
];
const pal=[['雾紫','#f7f3ff','#33284b','#7052ad','#fff'],['青竹绿','#f2faf6','#163c31','#087f63','#fff'],['玫瑰粉','#fff4f7','#452634','#a83d63','#fff'],['暖砂橙','#fff8ed','#3c2b1b','#a65f16','#fff'],['经典蓝','#fff','#0d1726','#1266ed','#fff'],['浅色','#fff','#212121','#2b2b2b','#fff'],['深海夜','#182536','#edf4ff','#89b9ff','#142339'],['深色','#1f2124','#f0f2f5','#e0e3e6','#1a1c1f'],['高对比','#090909','#fff','#ffe066','#111']];
const clamp=x=>Math.max(0,Math.min(1,x)),mix=(a,b,p)=>a+(b-a)*p;
const smooth=x=>{x=clamp(x);return x*x*(3-2*x)},ease=x=>1-(1-clamp(x))**4;
const io=x=>{x=clamp(x);return x<.5?8*x**4:1-(-2*x+2)**4/2};
const spring=x=>x<=0?0:x>=1?1:1-Math.exp(-6*x)*Math.cos(8*x);
const r=(t,a,b)=>clamp((t-a)/(b-a));
function opacity(a,fn){if(a<=0)return;g.save();g.globalAlpha*=clamp(a);fn();g.restore()}
function box(x,y,w,h,rad,fill,stroke){g.beginPath();g.roundRect(x,y,w,h,rad);if(fill){g.fillStyle=fill;g.fill()}if(stroke){g.lineWidth=1.3;g.strokeStyle=stroke;g.stroke()}}
function circle(x,y,radius,color){if(radius<=0)return;g.beginPath();g.arc(x,y,radius,0,Math.PI*2);g.fillStyle=color;g.fill()}
function text(value,x,y,size=40,color=INK,weight=400,align='left',family='Noto Sans CJK SC'){g.font=`${weight} ${size}px "${family}"`;g.textAlign=align;g.textBaseline='alphabetic';g.fillStyle=color;g.fillText(value,x,y)}
function measure(value,size=80,weight=500){g.font=`${weight} ${size}px "Noto Sans CJK SC"`;return g.measureText(value).width}
function line(x1,y1,x2,y2,color,width=2){g.beginPath();g.moveTo(x1,y1);g.lineTo(x2,y2);g.strokeStyle=color;g.lineWidth=width;g.lineCap='round';g.stroke()}
function background(t,color='#f5f1fc',ink=INK){
 g.fillStyle=color;g.fillRect(0,0,1920,1080);
 // Broad moving light, shared by the film's surfaces rather than a decorative particle layer.
 if(!['#182536','#090909'].includes(color)){let h=g.createLinearGradient(0,0,1920,980);h.addColorStop(0,'#ffffffa0');h.addColorStop(.5,'#ffffff00');h.addColorStop(1,'#d7c9ee30');g.fillStyle=h;g.fillRect(0,0,1920,1080)}
 g.drawImage(images.logo,75,53,34,34);text('ModernIME',123,80,24,ink,600,'left','Noto Serif');
}
function caption(title,sub='',color=INK){text(title,88,166,58,color,600,'left','Noto Serif CJK SC');if(sub)text(sub,92,220,25,color,400)}
function foot(native=false,color='#8c7f9b',note=''){
 text('字里，见自己。',86,1040,28,color,400,'left','AR PL UKai CN');
 text(note||(native?'v0.2.3 真实界面 · 运镜与操作为动画演示':'输入流程动画演示'),1834,1037,17,color,400,'right');
}
function caret(x,y,height,color){box(x,y-height,5,height,2.5,color)}
function keyEvents(scene){const a=[];timing.typing.filter(x=>x.scene===scene).forEach(x=>{
 for(let i=0;i<x.pinyin.length;i++)a.push({at:x.start+i*x.interval,key:x.pinyin[i]});
 a.push({at:x.commit,key:'SPACE'});if(x.after)a.push({at:x.afterAt,key:x.after==='，'?',':'.'});
});return a.sort((x,y)=>x.at-y.at)}
const events={0:keyEvents(0),5:keyEvents(5)};
function typingState(scene,t){let written='',preedit='',target='',pulse=0,lastWord='';
 for(const p of timing.typing.filter(x=>x.scene===scene)){
  if(t>=p.commit){written+=p.word;if(t-p.commit<.36){pulse=1-(t-p.commit)/.36;lastWord=p.word}if(p.after&&t>=p.afterAt)written+=p.after}
  else if(t>=p.start){preedit=p.pinyin.slice(0,Math.floor((t-p.start)/p.interval)+1);target=p.word}
 }
 const keys=events[scene].filter(e=>e.at<=t),last=keys.at(-1);
 return {written,preedit,target,pulse,lastWord,key:last&&t-last.at<.16?last.key:null};
}
// Real projective transforms. The camera's pivot is a feature's native GTK coordinate.
function project(x,y,c){
 let u=(x-c.fx)*c.z,v=(y-c.fy)*c.z,w=0;
 let y1=v*Math.cos(c.rx),z1=v*Math.sin(c.rx);v=y1;w=z1;
 let x2=u*Math.cos(c.ry)+w*Math.sin(c.ry),z2=-u*Math.sin(c.ry)+w*Math.cos(c.ry);u=x2;w=z2;
 let xx=u*Math.cos(c.rz)-v*Math.sin(c.rz),yy=u*Math.sin(c.rz)+v*Math.cos(c.rz);
 let k=2400/(2400+w);return [c.x+xx*k,c.y+yy*k];
}
const cam=(extra={})=>({fx:490,fy:360,z:1.3,rx:0,ry:0,rz:0,x:960,y:565,...extra});
function tween(a,b,p){const o={};for(const k of Object.keys(a))o[k]=mix(a[k],b[k],p);return o}
function track(t,keys){let prev=keys[0];for(let i=1;i<keys.length;i++){const next=keys[i];if(t<=next[0])return tween(prev[1],next[1],io(r(t,prev[0],next[0])));prev=next}return prev[1]}
function projected(width,height,c,fn){
 const q=[[0,0],[width,0],[width,height],[0,height]].flatMap(([x,y])=>project(x,y,c));
 g.save();g.transform(g.createProjection(q,[width,height]));fn();g.restore();
}
function pointer(x,y,press=0){
 const scale=1-.16*press;g.save();g.translate(x,y);g.scale(scale,scale);
 g.beginPath();g.moveTo(0,0);g.lineTo(2,26);g.lineTo(9,20);g.lineTo(14,31);g.lineTo(20,28);g.lineTo(15,18);g.lineTo(26,17);g.closePath();g.fillStyle='#fff';g.fill();g.strokeStyle='#514563';g.lineWidth=1.4;g.stroke();g.restore();
}
function click(c,x,y,local,at){
 const p=r(local,at,at+.55),pt=project(x,y,c);
 if(local>=at&&local<at+.55){g.beginPath();g.arc(...pt,8+p*40,0,Math.PI*2);g.strokeStyle=`rgba(116,81,174,${.7*(1-p)})`;g.lineWidth=3;g.stroke()}
 pointer(pt[0],pt[1],local>=at&&local<at+.13?Math.sin((local-at)/.13*Math.PI):0);
}
function native(key,c,clip=[0,0,980,720],layer=null){
 g.save();g.beginPath();g.roundRect(42,245,1836,725,28);g.clip();
 projected(980,720,c,()=>{
  g.save();g.beginPath();g.roundRect(...clip,12);g.clip();const scale=images[key].width/980;g.drawImage(images[key],...clip.map(v=>v*scale),...clip);
  if(layer)layer();g.restore();
 });g.restore();
}
function nativeBlend(a,b,p,c,clip){if(p>=1){native(b,c,clip);return}native(a,c,clip);if(a!==b)opacity(p,()=>native(b,c,clip))}
function nativeReveal(a,b,p,c,clip,origin=[880,42]){
 native(b,c,clip);
 // Draw the old surface with a shrinking complementary mask, revealing the actual new capture.
 if(p<1){g.save();g.beginPath();g.roundRect(42,245,1836,725,28);g.clip();projected(980,720,c,()=>{
  g.beginPath();g.rect(...clip);g.clip();
  const radius=Math.max(.01,p*1250);
  g.beginPath();g.rect(0,0,980,720);g.moveTo(origin[0]+radius,origin[1]);g.arc(origin[0],origin[1],radius,0,Math.PI*2);g.clip('evenodd');g.drawImage(images[a],0,0,980,720);
 });g.restore()}
}
function keyboard(t,key,accent=PURPLE){
 const rows=['1234567890','qwertyuiop','asdfghjkl','zxcvbnm,.'];
 rows.forEach((row,j)=>{
  const kw=52,gap=8,width=row.length*(kw+gap)-gap,start=960-width/2;
  [...row].forEach((k,i)=>{const x=start+i*(kw+gap),y=787+j*44,active=key===k;
   box(x,y+3,kw,37,7,'#cfc5de');box(x,y+(active?3:0),kw,35,7,active?accent:'#fff',active?accent:'#ded4e9');
   text(k.toUpperCase(),x+kw/2,y+25+(active?3:0),18,active?'#fff':'#7e728d',500,'center','Noto Sans');
  });
 });
 const on=key==='SPACE';box(842,967,236,39,9,'#cfc5de');box(842,967+(on?3:0),236,36,9,on?accent:'#fff','#ded4e9');
 text('空格',960,992+(on?3:0),18,on?'#fff':'#827090',500,'center');
}
function liveCandidate(state,x,y,palette=0){
 const p=pal[palette],w=state.target.length>2?680:590;
 g.save();g.translate(x,y);
 box(0,0,w,126,palette===1?38:18,p[1],p[2]+'30');
 text(state.preedit,24,43,32,p[2],450,'left','Noto Sans');line(24,53,24+measure(state.preedit,32),53,p[3],1.4);
 box(14,65,Math.max(148,32+state.target.length*38),48,palette===1?24:10,p[3]);
 text('1',29,99,22,p[4]);text(state.target,56,101,34,p[4],500);
 if(state.target==='你好'){text('2 你号',210,100,29,p[2]);text('3 拟好',365,100,29,p[2]);}
 else {text('空格确认',w-128,99,20,p[2]);}
 g.restore();
}
function editor(t,scene){
 const second=scene===5,state=typingState(scene,t),accent=second?'#087f63':PURPLE;
 background(t,second?'#e8f4ed':'#efe8fa');foot();
 const camKeys=second?[
 [0,cam({fx:690,fy:215,z:.8,rx:-.2,ry:.5,rz:.08,x:1700,y:550})],
 [.9,cam({fx:690,fy:215,z:1.08,rx:.06,ry:-.06,rz:-.015,x:960,y:510})],
 [3,cam({fx:680,fy:215,z:1.10,rx:.04,ry:.07,rz:.01,x:940,y:520})],
 [7.1,cam({fx:680,fy:215,z:1.12,rx:0,ry:0,rz:0,x:960,y:520})],
 [9.9,cam({fx:680,fy:215,z:1.09,rx:0,ry:0,rz:0,x:960,y:520})]
 ]:[
 [0,cam({fx:690,fy:215,z:.62,rx:.3,ry:-.4,rz:-.065,x:1000,y:600})],
 [.85,cam({fx:690,fy:215,z:1.05,rx:.06,ry:-.08,rz:-.012,x:960,y:520})],
 [3,cam({fx:675,fy:215,z:1.10,rx:.015,ry:.05,rz:.006,x:940,y:520})],
 [5.9,cam({fx:690,fy:215,z:1.12,rx:0,ry:0,rz:0,x:960,y:520})],
 [8.57,cam({fx:690,fy:215,z:.83,rx:-.17,ry:.28,rz:.04,x:790,y:520})]
 ];
 const c=track(t,camKeys);
 caption(second?'把灵感，写成日常。':'落笔，自成一格。',second?'刚刚选好的外观，现在用起来。':'拼音逐字输入，空格确认，汉字落下。');
 g.save();g.beginPath();g.rect(0,245,1920,735);g.clip();
 projected(1380,430,c,()=>{
  box(0,0,1380,430,25,'#fdfcff','#ddd2e8');
  text(second?'新的想法':'今天想写点什么',64,64,22,'#a094ac');
  line(64,91,1316,91,'#e9e3ef',1);
  const size=second?92:108;
  const written=state.written;
  text(written,88,220,size,INK,500);
  const cx=88+measure(written,size,500);
  const blink=state.preedit||state.key?1:.6+.4*Math.cos(t*5);
  opacity(blink,()=>caret(cx+7,228,112,accent));
  if(state.preedit){
   const xx=Math.min(cx+8,730);
   liveCandidate(state,xx,262,second?1:0);
  }
  if(state.pulse>0){opacity(state.pulse*.18,()=>box(cx-measure(state.lastWord,size),121,measure(state.lastWord,size)+10,120,14,accent));}
  if(!state.preedit&&written&&t>(second?7:5.5)){
   opacity(ease(r(t,second?7:5.5,second?7.6:6.1)),()=>text(second?'你的表达，自有风格。':'每一次输入，都有自己的习惯。',90,371,32,accent,450));
  }
 });
 g.restore();keyboard(t,state.key,accent);
}
function entry(t){
 background(t);foot(true);caption('靠近一点，看见你的选择。','主题、候选、字号，每一处都可以自己调整。');
 const c=track(t,[[0,cam({z:.65,rx:.27,ry:-.55,rz:-.11,x:1500,y:730})],
 [1.05,cam({z:1.05,rx:.14,ry:-.18,rz:-.025,x:1000,y:630})],
 [3.1,cam({fx:594,fy:238,z:2.0,rx:.04,ry:.12,rz:.018,x:970,y:615})],
 [5.0,cam({fx:825,fy:90,z:2.8,rx:-.07,ry:-.15,rz:-.02,x:1130,y:525})],
 [6.85,cam({fx:805,fy:105,z:2.65,rx:.02,ry:0,rz:0,x:1070,y:540})]]);
 const p=io(r(t,1.25,3.1)),clip=[mix(0,240,p),mix(0,10,p),mix(980,720,p),mix(720,385,p)];
 native('candidates-lavender',c,clip);
 if(t>3.55)click(c,891,42,t,4.8);
 // The option list unfolds in the plane of the photographed theme button.
 const open=spring(r(t,4.9,5.75));
 if(open>0)projected(980,720,c,()=>{
  g.save();g.beginPath();g.roundRect(782,63,174,238*clamp(open),8);g.clip();box(782,63,174,238,8,'#fff','#c7bbd8');
  themes.forEach((th,i)=>{const y=90+i*31;circle(801,y-6,5,th[2]);text(th[1],817,y,13,'#4a4059',i===0?600:400)});g.restore();
 });
}
function themeTour(t){
 const phase=Math.max(0,t-.15)/.86,idx=Math.min(6,Math.floor(phase)),th=themes[idx],p=phase>=7?1:ease(r((phase-idx)*.86,0,.29));
 background(t,th[3],th[4]);foot(true,idx>4?'#a2a9bb':'#8c7f9b');caption('颜色，也是一种表达。','七套界面主题；候选栏配色可以独立选择。',th[4]);
 const c=track(t,[[0,cam({fx:805,fy:100,z:2.65,rx:.02,ry:0,rz:0,x:1070,y:540})],
 [1.4,cam({fx:700,fy:170,z:2.3,rx:.1,ry:.18,rz:.02,x:960,y:575})],
 [3.1,cam({fx:596,fy:220,z:2.15,rx:-.08,ry:-.14,rz:-.025,x:960,y:590})],
 [5.8,cam({fx:599,fy:220,z:2.1,rx:.03,ry:0,rz:0,x:960,y:570})],
 [6.85,cam({fx:615,fy:325,z:2.2,rx:.08,ry:.15,rz:.025,x:960,y:585})]]);
 const clip=[242,14,715,374];
 if(idx&&p<1)nativeReveal('candidates-'+themes[idx-1][0],'candidates-'+th[0],p,c,clip);else native('candidates-'+th[0],c,clip);
 // A color rail is a film annotation, not an invented native control.
 box(440,918,1040,66,33,idx>4?'#ffffff16':'#ffffffc9');
 themes.forEach((item,i)=>{const x=485+i*146;circle(x,951,idx===i?14:9,item[2]);text(item[1],x+22,959,21,idx===i?th[4]:(idx>4?'#b6b6c3':'#908496'),idx===i?600:400)});
}
function appearance(t){
 background(t,'#f3edfa');foot(true);
 const stage=t<2.2?0:t<4.15?1:t<6.25?2:3;
 const labels=[['字大一点，读起来更舒服。','字体和字号，都可以按习惯设置。'],['选一种候选配色。','界面保持雾紫，候选栏可以是青竹绿。'],['轮廓，也能改变。','从圆润到胶囊，让边缘更柔和。'],['给文字，多一点呼吸。','宽松间距，留出你喜欢的空间。']];caption(...labels[stage]);
 const c=track(t,[[0,cam({fx:770,fy:520,z:2.7,rx:.13,ry:-.18,rz:-.025,x:1050,y:585})],
 [1.1,cam({fx:778,fy:520,z:2.75,rx:0,ry:-.04,rz:-.008,x:1040,y:590})],
 [2.2,cam({fx:603,fy:238,z:2.1,rx:-.12,ry:.1,rz:.015,x:960,y:565})],
 [3.4,cam({fx:454,fy:352,z:2.7,rx:.07,ry:.19,rz:.025,x:850,y:620})],
 [4.4,cam({fx:595,fy:250,z:2.1,rx:.06,ry:-.05,rz:-.012,x:960,y:555})],
 [5.5,cam({fx:600,fy:352,z:2.6,rx:-.08,ry:-.14,rz:-.025,x:1020,y:610})],
 [6.35,cam({fx:670,fy:530,z:2.35,rx:.1,ry:.1,rz:.015,x:990,y:595})],
 [7.3,cam({fx:835,fy:588,z:2.8,rx:0,ry:0,rz:0,x:1180,y:590})],
 [8.57,cam({fx:590,fy:250,z:2.03,rx:-.03,ry:.12,rz:.018,x:960,y:560})]]);
 const clip=[244,102,711,522];
 let a='candidates-lavender',b=a,p=0;
 if(t>=.95){a='candidates-lavender';b='candidates-font24';p=ease(r(t,.95,1.2))}
 if(t>=3.3){a='candidates-font24';b='candidates-jade24';p=ease(r(t,3.3,3.6))}
 if(t>=5.25){a='candidates-jade24';b='candidates-capsule24';p=ease(r(t,5.25,5.55))}
 if(t>=7.0){a='candidates-capsule24';b='candidates-spacious24';p=ease(r(t,7,7.3))}
 nativeBlend(a,b,p,c,clip);
 if(t<1.8)click(c,896,520,t,.95);else if(t>2.7&&t<4)click(c,469,350,t,3.3);else if(t>4.7&&t<6.2)click(c,636,351,t,5.25);else if(t>6.4&&t<7.9)click(c,911,588,t,7);
}
function schemes(t){
 background(t);foot(true);caption('换一种输入方案，习惯依然在。','全拼、小鹤双拼、自然码双拼。');
 const c=track(t,[[0,cam({fx:599,fy:390,z:1.5,rx:-.25,ry:.4,rz:.08,x:1550,y:650})],
 [.7,cam({fx:640,fy:388,z:2.25,rx:.07,ry:-.12,rz:-.018,x:1000,y:620})],
 [3.42,cam({fx:700,fy:390,z:2.5,rx:-.05,ry:.1,rz:.012,x:1040,y:610})]]);
 const state=t<1.3?'input-lavender':t<2.35?'input-xiaohe':'input-ziranma';native(state,c,[244,313,712,152]);
 if(t>.8)click(c,913,390,t,t<2.1?1.3:2.35);
 ['全拼','小鹤双拼','自然码双拼'].forEach((name,i)=>{const active=(i===0&&t<1.3)||(i===1&&t>=1.3&&t<2.35)||(i===2&&t>=2.35);box(470+i*345,853,295,62,31,active?PURPLE:'#fff');text(name,617+i*345,895,27,active?'#fff':'#88789a',550,'center')});
}
function artText(value,x,y,size=90,color=INK,align='center'){
 text(value,x,y,size,color,400,align,'AR PL UKai CN');
}
function waterword(value,t,color=PURPLE){
 opacity(.045,()=>text(value,1500+Math.sin(t*.16)*25,740,440,color,400,'center','Noto Serif CJK SC'));
}
function learning(t){
 background(t,'#f2ecfa');waterword('习惯',t);
 foot(false,'#8c7f9b','学习过程示意 · 排序随词频、上下文和使用变化');
 caption('越用，越合拍。','记住主动选择，让常用的表达慢慢靠前。');
 const intro=1-smooth(r(t,2.4,3.7));
 opacity(intro,()=>{
  artText('你一次次选中的，',960,480,86);artText('慢慢成为默契。',960,600,100,PURPLE);
  const p=ease(r(t,.5,2.5));line(690,658,690+540*p,658,'#b8a4d0',2);
 });
 opacity(smooth(r(t,2.7,3.8)),()=>{
  const promote=io(r(t,10.4,13.3));
  text('shi',960,382,48,PURPLE,450,'center','Noto Sans');
  const picked=t>=4.8&&t<5.5||t>=7.1&&t<7.8||t>=9.1&&t<9.8;
  const words=['是','时','试','事'],startX=510,gap=300,w=260;
  words.forEach((word,i)=>{
   const target=[1,2,0,3][i];
   const x=mix(startX+i*gap,startX+target*gap,promote);
   const y=449-(i===2?Math.sin(promote*Math.PI)*85:0);
   const active=i===2&&(picked||promote>.04);
   box(x-130,y+9,w,134,22,'#d8cbe8');box(x-130,y,w,134,22,active?PURPLE:'#fff',active?PURPLE:'#d8cae9');
   text(String(promote>.99?target+1:i+1),x-100,y+34,19,active?'#dfcff3':'#a395af');
   text(word,x,y+96,74,active?'#fff':INK,500,'center','Noto Serif CJK SC');
  });
  if(t<10.4){
   const taps=[4.8,7.1,9.1],at=taps.filter(x=>x<=t).at(-1);
   if(at!==undefined){const p=r(t,at,at+.6);opacity(1-p,()=>{g.beginPath();g.arc(1110,515,40+p*110,0,Math.PI*2);g.strokeStyle='#b196d1';g.lineWidth=3;g.stroke()})}
   opacity(ease(r(t,3.6,4.3)),()=>{text('日常里，一次次主动选词',960,743,34,'#82718e',400,'center');
    text(picked?'试':'',960,845,73,PURPLE,500,'center','Noto Serif CJK SC');});
  }else{
   opacity(ease(r(t,10.5,11.3)),()=>text('多次选择后',960,727,26,'#8b7999',450,'center'));
   opacity(ease(r(t,12.6,14.2)),()=>artText('让输入，更懂你的习惯。',960,836,62,PURPLE));
  }
 });
}
function personalDictionary(t){
 background(t,'#f5f0fb');waterword('词',t);foot(true,'#8c7f9b','v0.2.3 真实界面 · 使用独立的演示词典');
 caption('你的词，自有位置。','人名、项目名、专业术语，收进个人词典。');
 const cards=[['云笺','项目名'],['星野','人名'],['异步调用','专业术语']];
 const enter=ease(r(t,0,1.2)),leave=io(r(t,3.3,4.8));
 cards.forEach(([word,tag],i)=>{
  const x=mix(400+i*560,210+i*30,leave),y=mix(520,550+i*93,leave)+(1-enter)*(i%2?140:-140);
  const size=mix(72,36,leave);
  opacity(enter*(1-.8*leave),()=>{
   if(leave<.99)box(x-mix(220,140,leave),y-130,mix(440,270,leave),mix(230,80,leave),22,'#fff','#e0d6eb');
   text(word,x,y,size,INK,500,'center','Noto Serif CJK SC');text(tag,x,y+58*(1-leave),23,'#947fa6',450,'center');
  });
 });
 if(t>3.3){
  const c=track(t,[[3.3,cam({fx:595,fy:345,z:1.2,rx:.13,ry:-.3,rz:-.025,x:2240,y:620})],[5,cam({fx:604,fy:300,z:1.93,rx:.04,ry:.08,rz:.012,x:1090,y:610})],[7.4,cam({fx:599,fy:330,z:2.0,rx:-.03,ry:-.06,rz:-.01,x:1050,y:580})],[10.28,cam({fx:540,fy:566,z:2.4,rx:.04,ry:.08,rz:.012,x:1030,y:640})]]);
  native('dictionary-demo',c,[244,100,711,534]);
 }
}
function choicesAndPrivacy(t){
 background(t,'#edf6f0');foot(true,'#7b8e82');
 const later=smooth(r(t,4.7,6.2));
 caption('记住什么，由你决定。',t<5.4?'候选记忆与上下文排序，可以分别开关。':'输入和学习，在本地完成。');
 const c=track(t,[[0,cam({fx:649,fy:196,z:1.7,rx:-.12,ry:.22,rz:.025,x:1700,y:610})],[1.25,cam({fx:653,fy:196,z:2.28,rx:.035,ry:-.07,rz:-.01,x:1000,y:610})],[4.7,cam({fx:700,fy:205,z:2.4,rx:0,ry:0,rz:0,x:1060,y:610})],[6.4,cam({fx:600,fy:200,z:1.32,rx:0,ry:0,rz:0,x:580,y:550})],[10.28,cam({fx:600,fy:200,z:1.32,rx:0,ry:0,rz:0,x:580,y:550})]]);
 nativeBlend('learning-lavender','learning-private',ease(r(t,2.9,3.25)),c,[244,102,711,194]);
 if(t>1.6&&t<4.5)click(c,912,245,t,2.9);
 opacity(later,()=>{
  text('输入与学习',1410,480,62,'#24483c',600,'center','Noto Serif CJK SC');
  artText('留在本地',1410,590,92,'#087f63');
  text('无需云端输入服务',1410,674,26,'#5d8070',450,'center');
  line(1125,725,1695,725,'#b6ccbf',1.4);
  text('学习记录可备份后清空',1410,779,27,'#5d8070',450,'center');
 });
}
function efficiency(t){
 const second=t>=36/7,local=second?t-36/7:t;
 background(t,second?'#fcf3e8':'#eaf2fa');waterword(second?'快捷':'流畅',t,second?'#a65f16':'#2765a3');
 foot(false,'#8c7f9b','输入流程动画演示 · 英文补全与 v 模式金额转换');
 caption(second?'一笔金额，直接写好。':'中英文，接得上思路。',second?'输入 v88.5，转换为大写金额。':'英文前缀补全，让常用单词少敲几下。');
 const typed=second?'v88.5':'syst',target=second?'捌拾捌元伍角整':'system';
 const elapsed=Math.max(0,local-.6),count=Math.min(typed.length,Math.floor(elapsed/.3)+1),pre=local>=.6?typed.slice(0,count):'';
 const committed=local>3.05,accent=second?'#a65f16':'#2765a3';
 const arrival=ease(r(local,0,.55));
 g.save();g.translate((1-arrival)*120,0);g.globalAlpha=arrival;
 box(290,310,1340,438,30,'#fff',second?'#e8d9c3':'#ccdae9');
 text(second?'金额快捷输入':'英文前缀补全',345,367,23,'#8e8199',400);
 if(committed){text(target,960,510,second?76:108,INK,500,'center',second?'Noto Sans CJK SC':'Noto Sans');
  artText(second?'把繁琐，写得轻巧。':'让表达，自然衔接。',960,650,43,accent);
 }else{
  text(pre,960,478,68,accent,450,'center','Noto Sans');
  if(pre){box(475,538,970,133,22,second?'#fff8ed':'#f1f6fd',second?'#ebd1ab':'#cadbee');
   box(494,585,930,66,12,accent);text('1',518,630,26,'#fff');text(target,565,633,40,'#fff',500);text('空格确认',1388,570,20,'#8b8093',400,'right');}
 }
 g.restore();
 const lastIndex=Math.floor(elapsed/.3),hit=local>=.6&&lastIndex<typed.length&&(elapsed%.3)<.14?typed[lastIndex]:local>=3.05&&local<3.22?'SPACE':null;
 keyboard(local,hit,accent);
}
function ending(t){
 background(t);
 const p=spring(r(t,0,.8));
 g.save();g.translate(960,270);g.scale(p,p);g.drawImage(images.logo,-60,-60,120,120);g.restore();
 const label='ModernIME';text(label,960,480,125,INK,750,'center','Noto Sans');
 text('前往官网，下载 ModernIME。',960,582,44,PURPLE,600,'center','Noto Serif CJK SC');
 opacity(ease(r(t,.3,.9)),()=>{
  box(235,638,1450,116,22,'#fff','#ded2ef');
  text('https://sunshine134679.github.io/',960,684,30,PURPLE,600,'center','Noto Sans');
  text('fcitx5-linux-pinyin-input-method/',960,729,32,PURPLE,550,'center','Noto Sans');
  text('GitHub · 源码与发行版',960,819,25,'#756584',500,'center');
  text('github.com/sunshine134679/fcitx5-linux-pinyin-input-method',960,864,28,INK,450,'center','Noto Sans');
  text('Linux 拼音输入法',960,938,25,'#8b7b9b',400,'center');
 });
 text('Music: Digital Clouds — Alejandro Magaña (A. M.) / Mixkit',960,1017,16,'#a89bb4',400,'center','Noto Sans');
}
const sceneFns=[t=>editor(t,0),learning,personalDictionary,choicesAndPrivacy,efficiency,entry,t=>themeTour(t/1.5),t=>appearance(t/1.2),t=>schemes(t/1.5),ending];
function frame(time){
 canvas.width=W;g.setTransform(S,0,0,S,0,0);g.imageSmoothingEnabled=true;g.imageSmoothingQuality='high';
 let idx=timing.starts.findLastIndex(x=>time>=x);idx=Math.max(0,idx);sceneFns[idx](time-timing.starts[idx]);
 // Directional whip/wipe: new shots enter with a camera already in motion, not a frozen blank scene.
 if(idx<sceneFns.length-1){const start=timing.starts[idx+1],d=idx===1?.22:.36;if(time>start-d){const p=io(r(time,start-d,start));g.save();g.beginPath();
 if(idx===0||idx===3||idx===4){g.moveTo(1920*(1-p)+250*(1-p),0);g.lineTo(1920,0);g.lineTo(1920,1080);g.lineTo(1920*(1-p)-250*(1-p),1080);g.closePath()}
 else{g.ellipse(960,540,1400*p,900*p,0,0,Math.PI*2)}
 g.clip();sceneFns[idx+1](0);g.restore()}}
}
async function prepare(){fs.mkdirSync(OUT,{recursive:true});images.logo=await loadImage(path.join(ROOT,'../../config/icons/512x512/modernime.png'));
 if(args.includes('--poster'))return;
 const files=[...themes.map(x=>'candidates-'+x[0]),'candidates-font24','candidates-jade24','candidates-capsule24','candidates-spacious24','input-lavender','input-xiaohe','input-ziranma','learning-lavender','learning-private','dictionary-demo'];
 for(const file of files)images[file]=await loadImage(path.join(ROOT,'assets/retina/'+file+'.png'));
}
async function main(){await prepare();
 if(args.includes('--stills')||args.includes('--poster')||args.includes('--at')){
 const times=args.includes('--at')?opt('--at','0').split(',').map(Number):args.includes('--poster')?[94.7]:[1,2.8,5.1,7.5,11.7,14.9,17.8,20.6,23.6,25.6,28.8,31.5,35.2,39.5,42.1,45.5,49.5,52.4,55,57,60,63,67,70,73,77,80,83,87,89.5,94.7];
 for(const t of times){frame(t);canvas.toFileSync(path.join(OUT,t===94.7?'个性化篇封面.png':`frame-${t}.png`))}console.log('Rendered review stills');return}
 const draft=args.includes('--draft'),file=path.join(OUT,FROM>0?'ModernIME-个性化篇-片尾修订.mp4':draft?'ModernIME-个性化篇-预览.mp4':`ModernIME-个性化篇-${W===3840?'4K':H+'p'}-${FPS}fps.mp4`);
 const ff=process.env.FFMPEG||'ffmpeg';
 const command=['-y','-hide_banner','-loglevel','warning','-f','rawvideo','-pixel_format','rgba','-video_size',`${W}x${H}`,'-framerate',String(FPS),'-i','pipe:0','-ss',String(FROM),'-i',path.join(OUT,'soundtrack.wav'),'-i',path.join(ROOT,'chapters-v3.ffmeta'),'-map','0:v','-map','1:a','-map_metadata','2','-map_chapters','2','-c:v','libx264','-preset',draft?'ultrafast':'medium','-crf',draft?'23':'16','-pix_fmt','yuv420p','-threads','3','-vf','scale=in_range=full:out_range=tv:out_color_matrix=bt709','-colorspace','bt709','-color_primaries','bt709','-color_trc','bt709','-c:a','aac','-b:a','320k','-ar','48000','-movflags','+faststart','-t',String(Math.min(DURATION,Number(opt('--to',DURATION)))-FROM),file];
 const proc=spawn(ff,command,{stdio:['pipe','ignore','inherit']}),finished=once(proc,'close');proc.stdin.on('error',e=>{if(e.code!=='EPIPE')throw e});
 const start=Date.now(),count=Math.round((Math.min(DURATION,Number(opt('--to',DURATION)))-FROM)*FPS);
 for(let i=0;i<count;i++){frame(FROM+i/FPS);const raw=await canvas.toBuffer('raw',{colorType:'rgba'});if(!proc.stdin.write(raw))await once(proc.stdin,'drain');if(i%(FPS*2)===0)console.log(`${Math.round(i/count*100)}% ${(FROM+i/FPS).toFixed(0)}/96s · ${Math.round((Date.now()-start)/1000)}s elapsed`)}
 proc.stdin.end();const [code]=await finished;if(code)throw Error('FFmpeg failed '+code);console.log('Rendered '+file);
}
main().catch(error=>{console.error(error);process.exitCode=1});
