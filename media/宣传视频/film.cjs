#!/usr/bin/env node
'use strict';
// Deterministic native-resolution motion design. All positions use a 1920×1080 stage.
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
let skia;
try { skia = require(process.env.SKIA_CANVAS_MODULE || 'skia-canvas'); }
catch { skia = require('/home/wsl/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/@oai/artifact-tool/node_modules/skia-canvas'); }
const { Canvas, loadImage } = skia;
const ROOT = __dirname, OUT = path.join(ROOT, 'output');
const args = process.argv.slice(2);
const option = (name, fallback) => args.includes(name) ? args[args.indexOf(name) + 1] : fallback;
const WIDTH = Number(option('--width', 3840)), HEIGHT = WIDTH * 9 / 16;
const FPS = Number(option('--fps', 60)), DURATION = 40;
const C = new Canvas(WIDTH, HEIGHT, { gpu: false });
const g = C.getContext('2d');
const S = WIDTH / 1920;
const images = {};
const INK = '#29223b', PURPLE = '#7854b4', WHITE = '#faf9fe';
const THEMES = [
  { id:'lavender', name:'雾紫', bg:'#f0ebf8', accent:'#7052ad', ink:INK, line:'让灵感，轻轻落下。' },
  { id:'blue', name:'晴空蓝', bg:'#edf3fe', accent:'#1262ff', ink:'#1c2d4a', line:'思路，在晴空里展开。' },
  { id:'jade', name:'青竹绿', bg:'#eaf4ee', accent:'#087f73', ink:'#173b32', line:'一抹清新，留在指尖。' },
  { id:'amber', name:'暖砂橙', bg:'#faf0e2', accent:'#a65f16', ink:'#43301f', line:'给日常，一点暖意。' },
  { id:'rose', name:'玫瑰粉', bg:'#fbeaf1', accent:'#a83d63', ink:'#482738', line:'温柔，也有自己的表达。' },
  { id:'night', name:'深海夜', bg:'#182536', accent:'#89b9ff', ink:'#f1f6ff', line:'夜深了，灵感还醒着。' },
  { id:'contrast', name:'高对比', bg:'#090909', accent:'#ffe066', ink:'#ffffff', line:'让每个字，都清晰。' }
];
const PALETTES = [
  ['雾紫','#f7f3ff','#33284b','#7052ad','#fff'],
  ['青竹绿','#f2faf6','#163c31','#087f63','#fff'],
  ['玫瑰粉','#fff4f7','#452634','#a83d63','#fff'],
  ['暖砂橙','#fff8ed','#3c2b1b','#a65f16','#fff'],
  ['经典蓝','#fff','#0d1726','#1266ed','#fff'],
  ['浅色','#fff','#212121','#2b2b2b','#fff'],
  ['深海夜','#182536','#edf4ff','#89b9ff','#142339'],
  ['深色','#1f2124','#f0f2f5','#e0e3e6','#1a1c1f'],
  ['高对比','#090909','#fff','#ffe066','#111']
];
const clamp = x => Math.max(0, Math.min(1, x));
const mix = (a,b,p) => a + (b-a)*p;
const smooth = x => { x=clamp(x); return x*x*(3-2*x); };
const ease = x => 1-Math.pow(1-clamp(x),4);
const io = x => { x=clamp(x); return x<.5 ? 8*x*x*x*x : 1-Math.pow(-2*x+2,4)/2; };
const spring = x => x<=0 ? 0 : x>=1 ? 1 : 1-Math.exp(-7*x)*Math.cos(10*x);
const range = (t,a,b) => clamp((t-a)/(b-a));
function alpha(a,fn){ if(a<=0)return;g.save();g.globalAlpha*=clamp(a);fn();g.restore(); }
function transform(x,y,scale,rotation,fn){ g.save();g.translate(x,y);g.rotate(rotation);g.scale(scale,scale);fn();g.restore(); }
function round(x,y,w,h,r,fill,stroke){g.beginPath();g.roundRect(x,y,w,h,r);if(fill){g.fillStyle=fill;g.fill();}if(stroke){g.strokeStyle=stroke;g.lineWidth=1.5;g.stroke();}}
function circle(x,y,r,fill){if(r<=0)return;g.beginPath();g.arc(x,y,r,0,Math.PI*2);g.fillStyle=fill;g.fill();}
function line(x1,y1,x2,y2,color,width=2){g.beginPath();g.moveTo(x1,y1);g.lineTo(x2,y2);g.strokeStyle=color;g.lineWidth=width;g.lineCap='round';g.stroke();}
function text(s,x,y,size=40,color=INK,weight=400,align='left',family='Noto Sans CJK SC'){
  g.font=`${weight} ${size}px "${family}"`;g.textAlign=align;g.textBaseline='alphabetic';g.fillStyle=color;g.fillText(s,x,y);
}
function revealText(s,x,y,size,t,delay=0,color=INK,weight=700){
  const p=range(t,delay,delay+.8);if(!p)return;
  g.save();g.beginPath();g.rect(x-5,y-size*1.2,1700,size*1.5);g.clip();
  text(s,x,y+size*1.3*(1-ease(p)),size,color,weight);g.restore();
}
function bg(color=WHITE,accent='#e9e1f6'){
  g.fillStyle=color;g.fillRect(0,0,1920,1080);
  const grad=g.createRadialGradient(1470,460,20,1470,460,1000);grad.addColorStop(0,accent);grad.addColorStop(1,color);
  g.fillStyle=grad;g.fillRect(0,0,1920,1080);
}
function branding(t,ink=INK){
  alpha(ease(range(t,.4,1.4)),()=>{g.drawImage(images.logo,94,64,36,36);text('ModernIME',144,92,26,ink,650,'left','Noto Sans');});
}
function footer(ink='#8e869b',native=false){
  text(native?'本机 v0.2.3 真实界面 · 转场为宣传动画':'ModernIME · Linux 拼音输入法',100,1027,20,ink,400);
}
function caret(x,y,h,color=PURPLE,angle=0){transform(x,y,1,angle,()=>round(-4,-h/2,8,h,4,color));}
function strokePath(points,color,width=3,opacity=1){alpha(opacity,()=>{g.beginPath();g.moveTo(...points[0]);for(let i=1;i<points.length;i++)g.lineTo(...points[i]);g.strokeStyle=color;g.lineWidth=width;g.lineCap='round';g.lineJoin='round';g.stroke();});}
function orbit(t,cx,cy,rx,ry,color,start=0,end=Math.PI*2,width=2){
  const pts=[];for(let i=0;i<=100;i++){const a=mix(start,end,i/100);pts.push([cx+Math.cos(a)*rx,cy+Math.sin(a)*ry]);}strokePath(pts,color,width);
}
function panelImage(key,x,y,w,rotation=0,scale=1,shadow=true){
  const im=images[key],h=w*im.height/im.width;
  transform(x+w/2,y+h/2,scale,rotation,()=>{
    if(shadow){g.save();g.shadowColor='#30254a22';g.shadowBlur=42;g.shadowOffsetY=24;round(-w/2,-h/2,w,h,18,'#fff');g.restore();}
    g.save();g.beginPath();g.roundRect(-w/2,-h/2,w,h,18);g.clip();g.drawImage(im,-w/2,-h/2,w,h);g.restore();
    round(-w/2,-h/2,w,h,18,null,'#ffffff88');
  });
}
function candidate(palette,x,y,w=1000,scale=1,rotation=0,{first='你好',words=['你号','拟好','拟合','霓虹'],radius=18}={}){
  const [name,panel,ink,accent,selectedInk]=palette;
  transform(x,y,scale,rotation,()=>{
    g.save();g.shadowColor='#30254a16';g.shadowBlur=24;g.shadowOffsetY=12;round(-w/2,-55,w,110,radius,panel);g.restore();
    round(-w/2,-55,w,110,radius,null,ink+'28');
    round(-w/2+14,-41,182,82,Math.min(radius,14),accent);
    text('1',-w/2+32,15,26,selectedInk,400);
    text(first,-w/2+62,17,43,selectedInk,500);
    words.forEach((word,i)=>{const px=-w/2+235+i*(w-260)/words.length;text(`${i+2}`,px,14,24,ink,400);text(word,px+30,17,39,ink,400);});
  });
}

function opening(t){
  bg();branding(t);footer();
  const gather=io(range(t,.35,1.8)),commit=io(range(t,2.7,3.6));
  const introFade=1-smooth(range(t,2.7,3.25));
  // A handwritten path straightens into a text insertion caret.
  const trace=[];const traceEnd=ease(range(t,0,1.15));
  for(let i=0;i<=90;i++){const u=i/90*traceEnd;trace.push([mix(140,1020,u),600-160*Math.sin(u*Math.PI*1.7)*(1-gather)-140*gather]);}
  strokePath(trace,'#b49bdc',3,1-smooth(range(t,.8,1.7)));
  alpha(introFade,()=>{
    'nihao'.split('').forEach((letter,i)=>{
      const p=spring(range(t,.3+i*.12,1.65+i*.06));
      const xx=mix(460+i*250,787+i*68,p),yy=mix(220+Math.sin(i*2.2)*220,430,p);
      transform(xx,yy, mix(.65,1,p),mix((i-2)*.2,0,p),()=>text(letter,0,0,80,INK,450,'center','Noto Sans'));
    });
    caret(mix(1020,1127,gather),430-32,80,PURPLE,Math.sin(t*3)*(1-gather)*.4);
    const p=spring(range(t,1.5,2.45));
    if(p>0)candidate(PALETTES[0],960,570+90*(1-p),940,.86+.14*p,0);
    alpha(range(t,2,2.3),()=>{round(860,732,200,54,14,'#fff','#ded5ed');text('空格',960,768,24,'#847294',450,'center');});
  });
  if(commit>0){
    const p=spring(range(t,2.7,3.75));
    transform(mix(621,470,p),mix(587,558,p),mix(.3,1,p),-.04*Math.sin(p*Math.PI),()=>text('你好',0,0,146,INK,700));
    const tail='，Linux。';const reveal=range(t,3.05,4.25);
    alpha(ease(reveal),()=>{g.save();g.beginPath();g.rect(765,380,790*ease(reveal),240);g.clip();text(tail,765,558,146,INK,700,'left','Noto Sans');g.restore();});
    const endX=mix(780,1420,ease(reveal));caret(endX,503,150,PURPLE);
  }
  alpha(smooth(range(t,3.9,4.6)),()=>text('下一句，从这里开始。',960,718,36,'#81728f',400,'center'));
}

function familiarity(t){
  bg('#faf9fe','#e7dff4');branding(2);footer('#8e869b',true);
  // The candidate bar is pulled open into the actual application window.
  const p=spring(range(t,0,1.2));
  const x=mix(500,740,p),y=mix(460,166,p),w=mix(940,1090,p),h=w*2160/2940;
  g.save();g.beginPath();g.roundRect(x,y,w,mix(112,h,clamp(p)),20);g.clip();
  panelImage(t<2.9?'candidates-lavender':'input-lavender',x,y,w,0,1,false);
  g.restore();
  if(t>2.2&&t<2.9){
    g.save();g.beginPath();g.arc(x+w*.16,y+h*.32,ease(range(t,2.2,2.9))*1500,0,Math.PI*2);g.clip();panelImage('input-lavender',x,y,w,0,1,false);g.restore();
  }
  revealText('一打，',110,368,94,t,.35);
  revealText('就熟悉。',110,486,94,t,.48);
  alpha(ease(range(t,1.1,1.8)),()=>text('为 Linux 桌面而做的拼音输入法',115,563,30,'#84738f'));
  const names=['全拼','小鹤双拼','自然码双拼'];
  names.forEach((name,i)=>{
    const q=spring(range(t,1.6+i*.18,2.6+i*.18));
    alpha(clamp(q),()=>transform(119,658+i*72+32*(1-q),1,0,()=>{
      round(0,-30,41,41,10,'#e9e0f5');text(i?'双':'拼',20,0,19,PURPLE,600,'center');text(name,62,2,30,INK,500);
    }));
  });
  // A single soft ring follows the input scheme selector without covering it.
  if(t>3.4){const q=range(t,3.4,4.5);alpha(Math.sin(q*Math.PI)*.6,()=>{g.beginPath();g.ellipse(1675,355,93+18*q,35+8*q,0,0,Math.PI*2);g.strokeStyle=PURPLE;g.lineWidth=3;g.stroke();});}
}

function themeFrame(theme,t){
  bg(theme.bg,theme.bg);branding(2,theme.ink);footer(theme.id==='contrast'?'#bcbcbc':theme.id==='night'?'#9eafc7':'#867c8e',true);
  const drift=Math.sin(t*.6)*3;
  const dark=theme.id==='night'||theme.id==='contrast';
  alpha(dark?.22:.34,()=>{
    orbit(t,1300,550,690,365,theme.accent,0,Math.PI*2,1.5);
    orbit(t,1300,550,740,405,theme.accent,0,Math.PI*2,1);
  });
  text('你的界面，',104,302,71,theme.ink,700);
  text('你的颜色。',104,397,71,theme.ink,700);
  round(110,452,50,6,3,theme.accent);
  text(theme.name,108,557,70,theme.accent,650);
  text(theme.line,111,622,28,theme.ink,400);
  text('七套界面主题',112,711,26,theme.ink,500);
  THEMES.forEach((th,i)=>{
    const selected=theme.id===th.id;
    circle(130+i*59,764,selected?20:13,th.accent);
    if(selected){g.beginPath();g.arc(130+i*59,764,28,0,Math.PI*2);g.strokeStyle=theme.accent;g.lineWidth=2;g.stroke();}
  });
  const dolly=smooth(range(t,0,10));
  panelImage('candidates-'+theme.id,744-22*dolly,174-17*dolly+drift,1070+34*dolly,Math.sin(t*.52)*.004,1,false);
}
function themes(t){
  const segment=1.3;const phase=Math.max(0,t)/segment;
  const idx=Math.min(6,Math.floor(phase));const frac=phase-idx;
  if(idx===0||phase>=7||frac>.34){themeFrame(THEMES[idx],t);return;}
  themeFrame(THEMES[idx-1],t);
  const q=ease(frac/.34);g.save();g.beginPath();g.arc(1725,214,2300*q,0,Math.PI*2);g.clip();themeFrame(THEMES[idx],t);g.restore();
  if(q>0&&q<1){g.beginPath();g.arc(1725,214,2300*q,0,Math.PI*2);g.strokeStyle=THEMES[idx].accent+'44';g.lineWidth=4;g.stroke();}
}

function palettes(t){
  bg('#f9f8fe','#e9e2f6');branding(2);footer();
  revealText('好看的，不止一种。',102,252,78,t,.1);
  alpha(ease(range(t,.55,1.1)),()=>text('九套候选配色，与界面主题独立搭配。',108,320,31,'#84738f'));
  // The candidate bars form a shallow, rotating fan, then resolve into a usable palette.
  const spread=ease(range(t,0,1.8)),settle=io(range(t,2.4,3.8));
  for(let i=8;i>=0;i--){
    const p=PALETTES[i],baseX=960+(i-4)*107*spread,baseY=620+(i-4)*(i-4)*10*spread;
    const col=i%3,row=Math.floor(i/3),targetX=367+col*595,targetY=480+row*171;
    const x=mix(baseX,targetX,settle),y=mix(baseY,targetY,settle);
    const rot=mix((i-4)*.09*spread,0,settle),scale=mix(.68,.52,settle);
    const enter=spring(range(t,i*.035,.8+i*.035));
    alpha(clamp(enter),()=>{
      candidate(p,x,y+120*(1-enter),1000,scale,rot,{words:['你号','拟好','拟合']});
      alpha(settle,()=>text(p[0],x,y+71,23,i>5?'#666074':'#82738d',400,'center'));
    });
  }
  alpha(smooth(range(t,4,4.7)),()=>text('配色、轮廓、字号、间距，都可以按你的习惯调整。',960,985,28,'#756780',400,'center'));
}

function local(t){
  bg('#f5f3fb','#e4daf3');branding(2);footer('#8e869b',true);
  revealText('文字的旅程，',105,303,75,t,.15);
  revealText('留在本机。',105,402,75,t,.3);
  alpha(ease(range(t,1,1.7)),()=>{
    text('本地解码 · 本地学习',110,492,34,PURPLE,550);
    text('让常用的表达，慢慢成为习惯。',110,551,28,'#81728f');
  });
  const arrival=spring(range(t,.3,1.7));
  panelImage('learning-lavender',mix(1920,840,arrival),170,960,0,1,false);
  // Input glyphs follow an orbit, then settle inside a symbolic local workspace.
  const cx=367,cy=760,spin=t*.7;
  orbit(t,cx,cy,214,124,'#c6b2e2',0,Math.PI*2,2);
  const chars=['你','好','灵','感','日','常'];
  chars.forEach((ch,i)=>{
    const a=i/6*Math.PI*2+spin;
    const depth=(Math.sin(a)+1)/2;
    const p=io(range(t,3+i*.1,4.4+i*.1));
    const x=mix(cx+Math.cos(a)*214,cx-78+i*30,p),y=mix(cy+Math.sin(a)*124,cy+4,p);
    transform(x,y,mix(.7+depth*.4,.7,p),0,()=>{circle(0,-12,29,'#fcfaff');text(ch,0,0,30,PURPLE,550,'center');});
  });
  alpha(ease(range(t,1.4,2.1)),()=>{
    round(cx-95,cy-69,190,114,14,null,PURPLE);line(cx-48,cy+62,cx+48,cy+62,PURPLE,5);line(cx,cy+45,cx,cy+62,PURPLE,5);
    text('本机',cx,cy-23,25,PURPLE,600,'center');
  });
  alpha(ease(range(t,3.5,4.4)),()=>text('学习开关与记录，由你管理。',110,956,28,'#81728f'));
}

function finale(t){
  bg('#faf9fe','#e6def4');
  const gather=io(range(t,0,1.45));
  THEMES.forEach((th,i)=>{
    const a=i/7*Math.PI*2-.5;
    const radius=mix(760,0,gather);
    const x=960+Math.cos(a+t*.22)*radius,y=350+Math.sin(a+t*.22)*radius*.48;
    alpha(1-smooth(range(t,1,1.5)),()=>caret(x,y,mix(74,12,gather),th.accent,a+Math.PI/2));
  });
  const mark=spring(range(t,.65,1.9));
  alpha(clamp(mark),()=>transform(960,277,mix(.55,1,mark),-.12*(1-mark),()=>g.drawImage(images.logo,-69,-69,138,138)));
  revealText('ModernIME',571,489,132,t,1.05,INK,750);
  alpha(ease(range(t,1.4,2.1)),()=>text('下一句，由你开始。',960,596,47,'#78618f',500,'center'));
  alpha(ease(range(t,1.9,2.6)),()=>{
    round(651,667,371,77,38,PURPLE);text('前往官网下载',836,717,30,'#fff',500,'center');
    g.drawImage(images.qr,1086,642,134,134);
    text('Linux 拼音输入法',960,828,28,'#897a96',400,'center');
    text('sunshine134679.github.io/fcitx5-linux-pinyin-input-method/',960,892,24,'#8d8097',400,'center','Noto Sans');
  });
  // An almost imperceptible caret breath keeps the final held frame alive.
  alpha(.65+.35*Math.sin(t*2),()=>caret(1345,451,111,PURPLE));
}
const starts=[0,6,12,22,28,34],scenes=[opening,familiarity,themes,palettes,local,finale];
function frame(time){
  C.width=WIDTH; // reset Skia's retained display list; prevents growing memory per frame
  g.setTransform(S,0,0,S,0,0);g.imageSmoothingEnabled=true;g.imageSmoothingQuality='high';
  let idx=starts.findLastIndex(start=>time>=start);idx=Math.max(0,idx);
  scenes[idx](time-starts[idx]);
  const next=idx+1;
  if(next<starts.length&&time>starts[next]-.65){
    const p=io(range(time,starts[next]-.65,starts[next]));
    g.save();g.beginPath();
    if(next===1){
      const x=mix(460,0,p),y=mix(486,0,p),w=mix(1000,1920,p),h=mix(112,1080,p);g.roundRect(x,y,w,h,20*(1-p));
    }else if(next===3){
      g.moveTo(1920*(1-p)+300*(1-p),0);g.lineTo(1920,0);g.lineTo(1920,1080);g.lineTo(1920*(1-p)-300*(1-p),1080);g.closePath();
    }else if(next===5){
      g.ellipse(960,540,1400*p,900*p,0,0,Math.PI*2);
    }else{g.arc(next===2?1720:367,next===2?220:760,2400*p,0,Math.PI*2);}
    g.clip();scenes[next](0);g.restore();
  }
}
async function prepare(){
  fs.mkdirSync(OUT,{recursive:true});
  images.logo=await loadImage(path.join(ROOT,'../../config/icons/512x512/modernime.png'));
  images.qr=await loadImage(path.join(ROOT,'assets/官网二维码.png'));
  for(const theme of THEMES)images['candidates-'+theme.id]=await loadImage(path.join(ROOT,'assets/candidates-'+theme.id+'.png'));
  for(const page of ['input','learning'])images[page+'-lavender']=await loadImage(path.join(ROOT,'assets/'+page+'-lavender.png'));
}
async function main(){
  await prepare();
  if(args.includes('--poster')){
    frame(38.5);C.toFileSync(path.join(OUT,'宣传片封面.png'));
    console.log(`Poster exported at ${WIDTH}×${HEIGHT}.`);return;
  }
  if(args.includes('--stills')){
    const times=[.8,2.25,4.8,8,10,12.9,14.4,15.8,17.2,18.6,20,21.7,23.9,26.6,30,32.5,36.5,38.5];
    for(const t of times){frame(t);C.toFileSync(path.join(OUT,`frame-${String(t).replace('.','-')}.png`));}
    frame(38.5);C.toFileSync(path.join(OUT,'宣传片封面.png'));
    console.log(`Exported ${times.length} story frames at ${WIDTH}×${HEIGHT}.`);return;
  }
  const ffmpeg=process.env.FFMPEG||'ffmpeg';
  const draft=args.includes('--draft');
  const filename=path.join(OUT,draft?'ModernIME-预览.mp4':`ModernIME-宣传片-${WIDTH===3840?'4K':HEIGHT+'p'}-${FPS}fps.mp4`);
  const ffargs=[
    '-y','-hide_banner','-loglevel','warning',
    '-f','rawvideo','-pixel_format','rgba','-video_size',`${WIDTH}x${HEIGHT}`,
    '-framerate',String(FPS),'-i','pipe:0','-i',path.join(OUT,'soundtrack.wav'),
    '-i',path.join(ROOT,'chapters.ffmeta'),'-map','0:v:0','-map','1:a:0',
    '-map_metadata','2','-map_chapters','2',
    '-c:v','libx264','-preset',draft?'ultrafast':'medium','-crf',draft?'23':'16',
    '-pix_fmt','yuv420p','-threads','3',
    '-vf','scale=in_range=full:out_range=tv:out_color_matrix=bt709',
    '-color_primaries','bt709','-color_trc','bt709','-colorspace','bt709',
    '-c:a','aac','-b:a','320k','-ar','48000','-movflags','+faststart',
    '-t',String(DURATION),filename
  ];
  const proc=spawn(ffmpeg,ffargs,{stdio:['pipe','ignore','inherit']});
  const finished=once(proc,'close');proc.stdin.on('error',e=>{if(e.code!=='EPIPE')throw e;});
  const start=Date.now(),total=Math.round(DURATION*FPS);
  for(let i=0;i<total;i++){
    frame(i/FPS);const buffer=await C.toBuffer('raw',{colorType:'rgba'});
    if(!proc.stdin.write(buffer))await once(proc.stdin,'drain');
    if(i%(FPS*2)===0)console.log(`${(100*i/total).toFixed(0)}% · ${(i/FPS).toFixed(0)}/${DURATION}s · ${((Date.now()-start)/1000).toFixed(0)}s rendering`);
  }
  proc.stdin.end();const [code]=await finished;if(code!==0)throw Error('FFmpeg exit '+code);
  console.log('Rendered '+filename);
}
main().catch(e=>{console.error(e);process.exitCode=1;});
