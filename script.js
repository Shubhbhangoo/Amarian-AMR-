const reduceMotion=matchMedia('(prefers-reduced-motion: reduce)').matches;
const progress=document.querySelector('.scroll-progress');
const glow=document.querySelector('.cursor-glow');
const hero=document.querySelector('.hero');
const stage=document.querySelector('.hero-stage');
const diamond=document.querySelector('.diamond-shell');
const copy=document.getElementById('copy');
const command='cmake --preset dev && cmake --build build/dev && ctest --preset dev';
if(copy)copy.addEventListener('click',async()=>{try{await navigator.clipboard.writeText(command);copy.textContent='COPIED';setTimeout(()=>copy.textContent='COPY',1300)}catch{copy.textContent='SELECT'}});
const update=()=>{const max=document.documentElement.scrollHeight-innerHeight;if(progress)progress.style.transform=`scaleX(${max?scrollY/max:0})`};
addEventListener('scroll',update,{passive:true});update();
if(!reduceMotion&&matchMedia('(pointer:fine)').matches){addEventListener('pointermove',e=>{if(glow){glow.style.left=`${e.clientX}px`;glow.style.top=`${e.clientY}px`}});if(hero&&stage&&diamond){let tx=0,ty=0,x=0,y=0;hero.addEventListener('pointermove',e=>{const r=hero.getBoundingClientRect();tx=(e.clientX-r.left)/r.width-.5;ty=(e.clientY-r.top)/r.height-.5});hero.addEventListener('pointerleave',()=>{tx=ty=0});const frame=()=>{x+=(tx-x)*.035;y+=(ty-y)*.035;stage.style.transform=`translate(${x*12}px,${y*9}px)`;diamond.style.transform=`rotate(45deg) rotateX(${y*7}deg) rotateY(${x*7}deg)`;requestAnimationFrame(frame)};frame()}}
const reveal=document.querySelectorAll('.section,.asset-section,.protocol-card,.node,.terminal,.build-inner');
if(!reduceMotion&&'IntersectionObserver'in window){const io=new IntersectionObserver(entries=>entries.forEach(entry=>{if(entry.isIntersecting){entry.target.classList.add('is-visible');io.unobserve(entry.target)}}),{threshold:.08});reveal.forEach(el=>io.observe(el))}else reveal.forEach(el=>el.classList.add('is-visible'));
const navLinks=[...document.querySelectorAll('.nav nav a')];const sections=navLinks.map(a=>document.querySelector(a.getAttribute('href'))).filter(Boolean);if('IntersectionObserver'in window){const active=new IntersectionObserver(entries=>entries.forEach(e=>{if(e.isIntersecting){navLinks.forEach(a=>a.classList.toggle('active',a.getAttribute('href')===`#${e.target.id}`))}}),{rootMargin:'-35% 0px -55%'});sections.forEach(s=>active.observe(s))}
