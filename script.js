const reduceMotion=matchMedia('(prefers-reduced-motion: reduce)').matches;
const copy=document.getElementById('copy');
const command='cmake --preset dev && cmake --build build/dev && ctest --preset dev';
if(copy)copy.onclick=async()=>{try{await navigator.clipboard.writeText(command);copy.textContent='COPIED';setTimeout(()=>copy.textContent='COPY',1400)}catch{copy.textContent='SELECT'}};
const progress=document.querySelector('.progress');
const updateProgress=()=>{const max=document.documentElement.scrollHeight-innerHeight;progress.style.transform=`scaleX(${max>0?scrollY/max:0})`};
addEventListener('scroll',updateProgress,{passive:true});updateProgress();
if(!reduceMotion&&matchMedia('(pointer:fine)').matches){const visual=document.querySelector('.hero-visual'),diamond=document.querySelector('.diamond');const hero=document.querySelector('.hero');if(hero&&visual){let tx=0,ty=0,x=0,y=0;hero.onpointermove=e=>{const r=hero.getBoundingClientRect();tx=(e.clientX-r.left)/r.width-.5;ty=(e.clientY-r.top)/r.height-.5};hero.onpointerleave=()=>{tx=ty=0};const loop=()=>{x+=(tx-x)*.045;y+=(ty-y)*.045;visual.style.transform=`translate(${x*16}px,${y*12}px)`;if(diamond)diamond.style.transform=`rotate(45deg) rotateX(${y*8}deg) rotateY(${x*8}deg)`;requestAnimationFrame(loop)};loop()}}
const revealEls=document.querySelectorAll('.section,.feature,.map-node,.token-layout,.evidence-grid,.build-inner');
const reveal=new IntersectionObserver(es=>es.forEach(e=>{if(e.isIntersecting){e.target.animate([{opacity:.18,transform:'translateY(18px)'},{opacity:1,transform:'translateY(0)'}],{duration:650,easing:'cubic-bezier(.2,.8,.2,1)',fill:'forwards'});reveal.unobserve(e.target)}}),{threshold:.08});
if(!reduceMotion)revealEls.forEach(e=>reveal.observe(e));
