import {advance,seed,size} from './life.mjs';
const en=document.body.dataset.lang==='en';
const text=en?{copy:'Copy',copied:'Copied',copyFail:'Select the code to copy it.',play:'Play',pause:'Pause',correct:'Yes.',wrong:'That’s not the right answer. The chapter explains why.',read:'Read',done:'Mark as read'}:{copy:'Copier',copied:'Copié',copyFail:'Sélectionnez le code pour le copier.',play:'Lancer',pause:'Pause',correct:'Oui.',wrong:'Ce n’est pas la bonne réponse. L’explication se trouve dans le chapitre.',read:'Lu',done:'Marquer comme lu'};
for(const button of document.querySelectorAll('.copy'))button.addEventListener('click',async()=>{try{await navigator.clipboard.writeText(button.closest('.code').querySelector('code').textContent);button.textContent=text.copied;}catch{button.textContent=text.copyFail;}setTimeout(()=>button.textContent=text.copy,2500);});
const quiz=document.querySelector('.quiz');
if(quiz)for(const button of quiz.querySelectorAll('button'))button.addEventListener('click',()=>{
 for(const b of quiz.querySelectorAll('button')){b.removeAttribute('data-result');b.setAttribute('aria-pressed','false');}
 const right=button.dataset.answer===quiz.dataset.right;button.dataset.result=right?'correct':'wrong';button.setAttribute('aria-pressed','true');quiz.querySelector('.quiz-result').textContent=right?text.correct:text.wrong;
});
let progress=[];try{const saved=JSON.parse(localStorage.getItem('webfront-book-v1')||'[]');if(Array.isArray(saved))progress=saved.filter(x=>typeof x==='string');}catch{}
const mark=document.querySelector('#mark-read');
function updateProgress(){const ids=[...document.querySelectorAll('nav[aria-label] .chapter-link')].filter(link=>link.querySelector('span').textContent!=='↳').map(link=>link.dataset.id);const total=ids.filter(x=>progress.includes(x)).length;const bar=document.querySelector('#progress');if(bar)bar.value=total;const count=document.querySelector('#progress-count');if(count)count.textContent=total+' / '+ids.length;for(const link of document.querySelectorAll('.chapter-link'))link.querySelector('.read-indicator').textContent=progress.includes(link.dataset.id)?'✓':'';if(mark){const read=progress.includes(document.body.dataset.chapter);mark.setAttribute('aria-pressed',String(read));mark.textContent='✓ '+(read?text.read:text.done);}}
updateProgress();mark?.addEventListener('click',()=>{const id=document.body.dataset.chapter;progress=progress.includes(id)?progress.filter(x=>x!==id):[...progress,id];try{localStorage.setItem('webfront-book-v1',JSON.stringify(progress));}catch{}updateProgress();});
const search=document.querySelector('#chapter-search');
const normalise=s=>s.toLocaleLowerCase().normalize('NFD').replace(/[\u0300-\u036f]/g,'');
search?.addEventListener('input',()=>{let matches=0;for(const link of document.querySelectorAll('.chapter-link')){link.hidden=!normalise(link.textContent).includes(normalise(search.value));if(!link.hidden)matches++;}document.querySelector('#no-results').hidden=matches>0;});
const walk=document.querySelector('#walkthrough-toggle');walk?.addEventListener('click',()=>{const playing=walk.getAttribute('aria-pressed')==='true';walk.setAttribute('aria-pressed',String(!playing));document.querySelector('#walkthrough-image').src=playing?'/assets/life-preview.jpg':'/assets/life-bootstrap.gif';walk.querySelector('span').textContent=playing?(en?'▶ Watch the development session':'▶ Voir la session de développement'):(en?'■ Stop animation':'■ Arrêter l’animation');});
const canvas=document.querySelector('#life');
if(canvas){
 let cells=seed(),generation=0,playing=false,flat=false,timer=null;
 const context=canvas.getContext('2d');const play=document.querySelector('#play'),speed=document.querySelector('#speed');
 function tile(points,fill,stroke){context.beginPath();points.forEach(([x,y],i)=>i?context.lineTo(x,y):context.moveTo(x,y));context.closePath();context.fillStyle=fill;context.fill();if(stroke){context.strokeStyle=stroke;context.lineWidth=.7;context.stroke();}}
 function draw(){
  const box=canvas.getBoundingClientRect(),ratio=devicePixelRatio||1;canvas.width=Math.round(box.width*ratio);canvas.height=Math.round(box.height*ratio);context.setTransform(canvas.width/900,0,0,canvas.height/580,0,0);context.clearRect(0,0,900,580);
  for(let y=0;y<size;y++)for(let x=0;x<size;x++){
   const live=cells[y*size+x];if(flat){context.fillStyle=live?'#d7fa85':'#294638';context.fillRect(193+x*16,24+y*16,14,14);continue;}
   const px=450+(x-y)*12,py=62+(x+y)*6.3,h=live?18:0;
   if(live){tile([[px-11,py+6-h],[px,py+12-h],[px,py+12],[px-11,py+6]],'#72934c');tile([[px,py+12-h],[px+11,py+6-h],[px+11,py+6],[px,py+12]],'#9bb76c');}
   tile([[px,py-h],[px+11,py+6-h],[px,py+12-h],[px-11,py+6-h]],live?'#d7fa85':'#284535',live?'#ecffc5':'#3a5641');
  }
  document.querySelector('#generation').textContent=generation;document.querySelector('#population').textContent=cells.reduce((a,b)=>a+b,0);
 }
 function stop(){playing=false;clearTimeout(timer);play.textContent='▶ '+text.play;play.setAttribute('aria-pressed','false');}
 function step(){cells=advance(cells);generation++;draw();}
 function schedule(){timer=setTimeout(()=>{if(!playing)return;step();schedule();},1000/Number(speed.value));}
 play.addEventListener('click',()=>{if(playing){stop();return;}playing=true;play.textContent='Ⅱ '+text.pause;play.setAttribute('aria-pressed','true');schedule();});
 document.querySelector('#step').addEventListener('click',()=>{stop();step();});
 function reset(){stop();cells=seed(Number(document.querySelector('#pattern').value));generation=0;draw();}
 document.querySelector('#reset').addEventListener('click',reset);document.querySelector('#pattern').addEventListener('change',reset);
 speed.addEventListener('input',()=>{document.querySelector('#speed-value').value=speed.value;});
 document.querySelector('#view').addEventListener('click',event=>{flat=!flat;event.currentTarget.setAttribute('aria-pressed',String(flat));draw();});
 canvas.addEventListener('click',event=>{stop();const b=canvas.getBoundingClientRect(),px=(event.clientX-b.left)*900/b.width,py=(event.clientY-b.top)*580/b.height;let x,y;if(flat){x=Math.floor((px-193)/16);y=Math.floor((py-24)/16);}else{x=Math.floor(((px-450)/12+(py-62)/6.3)/2);y=Math.floor(((py-62)/6.3-(px-450)/12)/2);}if(x>=0&&x<size&&y>=0&&y<size){cells[y*size+x]^=1;draw();}});
 document.addEventListener('visibilitychange',()=>{if(document.hidden)stop();});
 new ResizeObserver(draw).observe(canvas);draw();
}
