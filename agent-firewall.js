const q=s=>document.querySelector(s);
const install=q('.install-box code');
const installButton=q('#copy-install');
if(installButton&&install){installButton.addEventListener('click',async()=>{try{await navigator.clipboard.writeText(install.textContent.trim());const old=installButton.textContent;installButton.textContent='COPIED';setTimeout(()=>installButton.textContent=old,1400)}catch{installButton.textContent='COPY FAILED';setTimeout(()=>installButton.textContent='COPY',1400)}})}
const core=q('.firewall-core');
document.addEventListener('pointermove',e=>{if(!core||matchMedia('(max-width:800px)').matches)return;const x=(e.clientX/innerWidth-.5)*2,y=(e.clientY/innerHeight-.5)*2;core.style.transform=`translate3d(${x*10}px,${y*10}px,0)`});
const cards=document.querySelectorAll('.control-map article,.flow-node,.release-list article');
cards.forEach(card=>card.addEventListener('pointermove',e=>{const r=card.getBoundingClientRect();card.style.setProperty('--px',`${((e.clientX-r.left)/r.width-.5)*2}`);card.style.setProperty('--py',`${((e.clientY-r.top)/r.height-.5)*2}`)}));
