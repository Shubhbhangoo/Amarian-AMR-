(() => {
  const progress = document.querySelector('.film-progress');
  const sections = [...document.querySelectorAll('main > section')];
  const navLinks = [...document.querySelectorAll('.top nav a')];

  const update = () => {
    const max = document.documentElement.scrollHeight - innerHeight;
    if (progress) progress.style.width = `${max > 0 ? (scrollY / max) * 100 : 0}%`;

    let active = '';
    sections.forEach(section => {
      const r = section.getBoundingClientRect();
      if (r.top <= innerHeight * .45 && r.bottom >= innerHeight * .45) active = section.id;
    });
    navLinks.forEach(link => link.classList.toggle('active', link.getAttribute('href') === `#${active}`));
  };

  const revealTargets = document.querySelectorAll('.statement-copy,.cap,.network-visual,.object-copy,.emission-visual,.node-screen,.node-copy,.build-inner,.verify-top,.doc');
  const observer = new IntersectionObserver(entries => {
    entries.forEach(entry => {
      if (entry.isIntersecting) {
        entry.target.classList.add('in');
        observer.unobserve(entry.target);
      }
    });
  }, { threshold: .14 });
  revealTargets.forEach(el => el.classList.add('reveal'));
  revealTargets.forEach(el => observer.observe(el));

  addEventListener('scroll', update, { passive: true });
  addEventListener('resize', update, { passive: true });
  update();
})();