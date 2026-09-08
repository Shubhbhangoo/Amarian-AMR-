const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

const revealObserver = new IntersectionObserver((entries) => {
  entries.forEach((entry) => {
    if (entry.isIntersecting) entry.target.classList.add('visible');
  });
}, { threshold: 0.14 });
document.querySelectorAll('.reveal').forEach((el) => revealObserver.observe(el));

const progress = document.getElementById('progress');
const updateProgress = () => {
  if (!progress) return;
  const max = document.documentElement.scrollHeight - window.innerHeight;
  progress.style.width = `${max > 0 ? Math.min(100, window.scrollY / max * 100) : 0}%`;
};
window.addEventListener('scroll', updateProgress, { passive: true });
updateProgress();

const copy = document.getElementById('copy');
if (copy) {
  copy.addEventListener('click', async () => {
    const command = 'cmake --preset dev && cmake --build build/dev && ctest --preset dev';
    try {
      await navigator.clipboard.writeText(command);
      copy.textContent = 'COPIED';
      setTimeout(() => { copy.textContent = 'COPY'; }, 1400);
    } catch {
      copy.textContent = 'SELECT';
    }
  });
}

const portal = document.querySelector('.portal');
const core = document.querySelector('.core-stage');
if (portal && core && !reduceMotion && window.matchMedia('(pointer:fine)').matches) {
  let x = 0, y = 0, tx = 0, ty = 0;
  portal.addEventListener('pointermove', (event) => {
    const rect = portal.getBoundingClientRect();
    tx = (event.clientX - rect.left) / rect.width - 0.5;
    ty = (event.clientY - rect.top) / rect.height - 0.5;
  });
  portal.addEventListener('pointerleave', () => { tx = 0; ty = 0; });
  const tick = () => {
    x += (tx - x) * 0.045;
    y += (ty - y) * 0.045;
    core.style.transform = `translate(${x * 28}px, ${y * 22}px) rotateX(${y * -2}deg) rotateY(${x * 3}deg)`;
    portal.style.setProperty('--mx', `${x}`);
    portal.style.setProperty('--my', `${y}`);
    requestAnimationFrame(tick);
  };
  tick();
}

const cards = document.querySelectorAll('.protocol-card, .doc, .map-node');
if (!reduceMotion && window.matchMedia('(pointer:fine)').matches) {
  cards.forEach((card) => {
    card.addEventListener('pointermove', (event) => {
      const rect = card.getBoundingClientRect();
      card.style.setProperty('--px', `${event.clientX - rect.left}px`);
      card.style.setProperty('--py', `${event.clientY - rect.top}px`);
    });
  });
}

const sections = [...document.querySelectorAll('main section[id]')];
const navLinks = [...document.querySelectorAll('.nav nav a')];
const navObserver = new IntersectionObserver((entries) => {
  entries.forEach((entry) => {
    if (!entry.isIntersecting) return;
    navLinks.forEach((link) => link.classList.toggle('active', link.getAttribute('href') === `#${entry.target.id}`));
  });
}, { rootMargin: '-42% 0px -48% 0px' });
sections.forEach((section) => navObserver.observe(section));

// Give the protocol core a tiny reactive pulse when it enters the viewport.
const gem = document.querySelector('.core-gem');
if (gem && !reduceMotion) {
  const pulseObserver = new IntersectionObserver(([entry]) => {
    if (entry.isIntersecting) gem.classList.add('awake');
  }, { threshold: 0.2 });
  pulseObserver.observe(gem);
}
