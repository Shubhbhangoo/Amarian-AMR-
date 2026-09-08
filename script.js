const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

const observer = new IntersectionObserver((entries) => {
  entries.forEach((entry) => {
    if (entry.isIntersecting) entry.target.classList.add('visible');
  });
}, { threshold: 0.12 });

document.querySelectorAll('.reveal').forEach((el) => observer.observe(el));

const progress = document.getElementById('progress');
const updateProgress = () => {
  if (!progress) return;
  const max = document.documentElement.scrollHeight - window.innerHeight;
  progress.style.width = `${max > 0 ? (window.scrollY / max) * 100 : 0}%`;
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

const stage = document.querySelector('.portal');
const core = document.querySelector('.core-stage');
if (stage && core && !reduceMotion && window.matchMedia('(pointer:fine)').matches) {
  let mx = 0, my = 0, tx = 0, ty = 0;
  stage.addEventListener('pointermove', (event) => {
    const rect = stage.getBoundingClientRect();
    tx = (event.clientX - rect.left) / rect.width - 0.5;
    ty = (event.clientY - rect.top) / rect.height - 0.5;
  });
  stage.addEventListener('pointerleave', () => { tx = 0; ty = 0; });
  const tick = () => {
    mx += (tx - mx) * 0.045;
    my += (ty - my) * 0.045;
    core.style.transform = `translate(${mx * 24}px, ${my * 24}px)`;
    stage.style.setProperty('--mx', `${mx}`);
    stage.style.setProperty('--my', `${my}`);
    requestAnimationFrame(tick);
  };
  tick();
}

const sections = [...document.querySelectorAll('main section[id]')];
const navLinks = [...document.querySelectorAll('.nav nav a')];
const navObserver = new IntersectionObserver((entries) => {
  entries.forEach((entry) => {
    if (!entry.isIntersecting) return;
    navLinks.forEach((link) => link.classList.toggle('active', link.getAttribute('href') === `#${entry.target.id}`));
  });
}, { rootMargin: '-40% 0px -50% 0px' });
sections.forEach((section) => navObserver.observe(section));
