const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

const revealObserver = new IntersectionObserver((entries) => {
  entries.forEach((entry) => {
    if (entry.isIntersecting) {
      entry.target.classList.add('visible');
      revealObserver.unobserve(entry.target);
    }
  });
}, { threshold: 0.12 });
document.querySelectorAll('.reveal').forEach((el) => revealObserver.observe(el));

const progress = document.getElementById('progress');
const updateProgress = () => {
  if (!progress) return;
  const max = document.documentElement.scrollHeight - window.innerHeight;
  progress.style.width = `${max > 0 ? (window.scrollY / max) * 100 : 0}%`;
};
window.addEventListener('scroll', updateProgress, { passive: true });
updateProgress();

const copy = document.getElementById('copy');
const command = 'cmake --preset dev && cmake --build build/dev && ctest --preset dev';
if (copy) copy.addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(command);
    copy.textContent = 'COPIED';
    setTimeout(() => { copy.textContent = 'COPY'; }, 1400);
  } catch { copy.textContent = 'SELECT'; }
});

const hero = document.querySelector('.hero');
const core = document.querySelector('.hero-core');
if (hero && core && !reduceMotion && matchMedia('(pointer:fine)').matches) {
  let mx = 0, my = 0, x = 0, y = 0;
  hero.addEventListener('pointermove', (e) => {
    const r = hero.getBoundingClientRect();
    mx = (e.clientX - r.left) / r.width - .5;
    my = (e.clientY - r.top) / r.height - .5;
  });
  hero.addEventListener('pointerleave', () => { mx = 0; my = 0; });
  const animate = () => {
    x += (mx - x) * .045; y += (my - y) * .045;
    core.style.transform = `translate(${x * 22}px,${y * 18}px) rotateX(${y * -2}deg) rotateY(${x * 2}deg)`;
    requestAnimationFrame(animate);
  };
  animate();
}

const sections = [...document.querySelectorAll('main section[id]')];
const links = [...document.querySelectorAll('.nav nav a')];
const navObserver = new IntersectionObserver((entries) => {
  entries.forEach((entry) => {
    if (!entry.isIntersecting) return;
    links.forEach((link) => link.classList.toggle('active', link.getAttribute('href') === `#${entry.target.id}`));
  });
}, { rootMargin: '-45% 0px -48% 0px' });
sections.forEach((section) => navObserver.observe(section));

if (!reduceMotion) {
  document.querySelectorAll('.feature-card,.doc').forEach((card) => {
    card.addEventListener('pointermove', (e) => {
      const r = card.getBoundingClientRect();
      card.style.setProperty('--px', `${e.clientX - r.left}px`);
      card.style.setProperty('--py', `${e.clientY - r.top}px`);
    });
  });
}
