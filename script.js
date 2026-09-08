const observer = new IntersectionObserver((entries) => {
  entries.forEach((entry) => {
    if (entry.isIntersecting) entry.target.classList.add('visible');
  });
}, { threshold: 0.12 });

document.querySelectorAll('.reveal').forEach((el) => observer.observe(el));

const copy = document.getElementById('copy');
if (copy) {
  copy.addEventListener('click', async () => {
    const command = 'cmake --preset dev && cmake --build build/dev && ctest --preset dev';
    try {
      await navigator.clipboard.writeText(command);
      copy.textContent = 'COPIED';
      setTimeout(() => copy.textContent = 'COPY', 1400);
    } catch {
      copy.textContent = 'SELECT';
    }
  });
}

// Subtle pointer-driven depth. The page remains fully usable without a mouse.
const hero = document.querySelector('.hero-art');
if (hero && window.matchMedia('(pointer:fine)').matches && !window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
  let frame = 0;
  let x = 0;
  let y = 0;
  let tx = 0;
  let ty = 0;

  hero.addEventListener('pointermove', (event) => {
    const rect = hero.getBoundingClientRect();
    tx = (event.clientX - (rect.left + rect.width / 2)) / rect.width;
    ty = (event.clientY - (rect.top + rect.height / 2)) / rect.height;
  });

  hero.addEventListener('pointerleave', () => {
    tx = 0;
    ty = 0;
  });

  const tick = () => {
    x += (tx - x) * 0.08;
    y += (ty - y) * 0.08;
    hero.style.setProperty('--mx', `${x * 34}px`);
    hero.style.setProperty('--my', `${y * 34}px`);
    hero.style.transform = `rotateX(${y * -2.2}deg) rotateY(${x * 2.8}deg)`;

    const gem = hero.querySelector('.gem');
    const blocks = hero.querySelectorAll('.block');
    if (gem) gem.style.transform = `rotate(45deg) translate(${x * 8}px, ${y * 8}px)`;
    blocks.forEach((block, index) => {
      const depth = (index + 1) * 7;
      block.style.transform = `translate(${x * depth}px, ${y * depth}px)`;
    });

    frame = requestAnimationFrame(tick);
  };

  tick();
}

// Cards get a restrained cursor-following highlight.
document.querySelectorAll('.feature').forEach((card) => {
  card.addEventListener('pointermove', (event) => {
    const rect = card.getBoundingClientRect();
    card.style.setProperty('--px', `${event.clientX - rect.left}px`);
    card.style.setProperty('--py', `${event.clientY - rect.top}px`);
  });
});
