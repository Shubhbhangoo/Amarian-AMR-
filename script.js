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
